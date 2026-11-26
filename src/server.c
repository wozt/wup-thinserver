#define _DEFAULT_SOURCE
#include "server.h"
#include "capture.h"
#include "capture_async.h" /* WUP_V912_CAPTURE_OPTIMIZATION */
#include "audio.h"
#include "input.h"
#include "x11util.h"
#include "wupstream.h"
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include "i18n.h" /* WUP_V914_GAMEPAD_LOCALE */
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

#define FW 854
#define FH 480

#define CAP_MAX_FAILS 8        /* ffmpeg restart attempts before giving up on capture */
#define SESSION_CAP_TRIES 30   /* capture_start attempts per pad session (~1 min) */

struct server_t {
    settings_t *s;
    pthread_t th;
    atomic_int running;
    atomic_int want_stream;
    atomic_int state;
    /* Only the GTK thread mutates shared settings; it publishes complete
     * snapshots here. Stream thread reads only its private local copy. */
    settings_t staged_settings;
    unsigned long settings_generation;
    /* Server-side TV/gyro changes must survive a subsequent GTK update. */
    char last_ui_monitor[64];
    double last_ui_gyro_bias[3];
    int monitor_dirty, gyro_dirty;
    server_metrics_t metrics;
    int restart_capture;
    int reload_keys;
    int reconnect;
    int calib_gyro;
    char status[256];
    pthread_mutex_t mtx;
    wupstream_input_t last_in;
    int has_in;
};

static void set_status(server_t *srv, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&srv->mtx);
    vsnprintf(srv->status, sizeof srv->status, fmt, ap);
    pthread_mutex_unlock(&srv->mtx);
    va_end(ap);
}

static int get_state(server_t *srv) {
    return atomic_load_explicit(&srv->state, memory_order_relaxed);
}

static double v98_clock(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void apply_quality_env(const settings_t *s) {
    char v[64];
    snprintf(v, sizeof v, "%d", s->crf > 0 ? s->crf : 18);
    setenv("WUPSTREAM_CRF", v, 1);
    snprintf(v, sizeof v, "%d", s->kbps > 0 ? s->kbps : 10000);
    setenv("WUPSTREAM_KBPS", v, 1);
    /* low latency: libwupstream already presets veryfast+zerolatency;
       we additionally force no B-frames and zero lookahead explicitly */
    char x264[160] = "";
    if (s->low_latency)
        snprintf(x264, sizeof x264, "bframes=0:rc-lookahead=0:sync-lookahead=0:sliced-threads=1");
    /* WUP_V913_GOP_ZERO: no explicit keyint override.
     * The pad requests IDRs itself; forcing periodic GOP corrupts video. */
    if (x264[0]) setenv("WUPSTREAM_X264", x264, 1);
    else unsetenv("WUPSTREAM_X264");
}

/* static 854x480 BGRA test frame for "gamepad only" mode (no capture) */
/* WUP_V914_GAMEPAD_LOCALE: UTF-8 text layout with font fallback and wrapping.
 * Called only on stream start or settings/language changes, NOT per video frame. */
static void wup_gamepad_test_text(cairo_t *cr, const char *message,
                                  const char *font, double center_y,
                                  double red, double green, double blue) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_from_string(font);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_width(layout, (FW - 48) * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    pango_layout_set_text(layout, message, -1);
    int text_width = 0, text_height = 0;
    pango_layout_get_pixel_size(layout, &text_width, &text_height);
    cairo_set_source_rgb(cr, red, green, blue);
    cairo_move_to(cr, 24, center_y - text_height / 2.0);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(desc);
    g_object_unref(layout);
}

static void build_test_frame(unsigned char *frame, const char *language_id) {
    /* The streaming worker owns its own short-lived catalog. Never access the
     * GTK thread's i18n instance or load fonts for a black frame. */
    wup_i18n_t *tr = wup_i18n_new(language_id);
    const char *title = wup_i18n_lookup(tr, "GAMEPAD ONLY MODE");
    const char *subtitle = wup_i18n_lookup(tr, "no capture - inputs & sensors active");
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, FW, FH);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        memset(frame, 0, (size_t)FW * FH * 4);
        cairo_surface_destroy(surf);
        wup_i18n_free(tr);
        return;
    }
    cairo_t *cr = cairo_create(surf);
    cairo_set_source_rgb(cr, 0.07, 0.08, 0.12);
    cairo_paint(cr);
    wup_gamepad_test_text(cr, title, "Sans Bold 40", FH / 2.0 - 35,
                          0.35, 0.75, 1.0);
    wup_gamepad_test_text(cr, subtitle, "Sans 22", FH / 2.0 + 35,
                          0.65, 0.65, 0.72);
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    int stride = cairo_image_surface_get_stride(surf);
    const unsigned char *data = cairo_image_surface_get_data(surf);
    for (int y = 0; y < FH; y++)
        memcpy(frame + (size_t)y * FW * 4,
               data + (size_t)y * stride, (size_t)FW * 4);
    cairo_surface_destroy(surf);
    wup_i18n_free(tr);
}

/* TV button on the pad: advance to the next monitor (screen source only) */
static void cycle_monitor(server_t *srv, settings_t *s) {
    screen_t screens[16];
    int n = list_screens(screens, 16);
    if (n <= 1) return;
    int cur = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(screens[i].name, s->source.output)) { cur = i; break; }
    int nxt = (cur + 1) % n;
    /* Fixed-size XRandR output name, never read beyond its 64-byte array. */
    memcpy(s->source.output, screens[nxt].name, sizeof s->source.output);
    s->source.output[sizeof s->source.output - 1] = '\0';
    pthread_mutex_lock(&srv->mtx);
    srv->restart_capture = 1;
    snprintf(srv->staged_settings.source.output,
             sizeof srv->staged_settings.source.output, "%s", s->source.output);
    srv->monitor_dirty = 1;
    pthread_mutex_unlock(&srv->mtx);
    settings_save(s);
    set_status(srv, "monitor: %s", screens[nxt].name);
}

/* WUP_V95_CLEAR_REGION: no selected pixels is an intentional black
 * frame, not a capture failure. Keep the encoder and input path running. */
static int region_is_cleared(const settings_t *s) {
    return s->source_type == SRC_REGION &&
           (s->source.w <= 0 || s->source.h <= 0);
}

/* start capture with retries; returns 0 on success */
static int capture_start_retry(capture_t *cap, const settings_t *s, server_t *srv) {
    for (int i = 0; i < SESSION_CAP_TRIES && srv->running && srv->want_stream; i++) {
        if (capture_start(cap, s) == 0) return 0;
        set_status(srv, "source unavailable, retrying (%d)...", i + 1);
        usleep(2000000);
    }
    return -1;
}

static void *server_thread(void *arg) {
    server_t *srv = arg;
    settings_t active_settings;
    settings_t *s = &active_settings;
    unsigned long seen_generation = 0;
    static unsigned char frame[FW * FH * 4];
    static int16_t audio_buf[2048 * 2];
    static int16_t mic_buf[2048];

    while (srv->running) {
        /* Refresh once per session. No reading the UI-owned settings here. */
        pthread_mutex_lock(&srv->mtx);
        active_settings = srv->staged_settings;
        seen_generation = srv->settings_generation;
        pthread_mutex_unlock(&srv->mtx);
        if (!srv->want_stream) {
            set_status(srv, "idle");
            srv->state = SRV_IDLE;
            usleep(200000);
            continue;
        }

        srv->state = SRV_WAITING_PAD;
        set_status(srv, "looking for GamePad...");

        apply_quality_env(s);
        wupstream *pad = wupstream_open(s->pad_ip[0] ? s->pad_ip : NULL, 8000);
        if (!pad) {
            set_status(srv, "no GamePad found, retrying...");
            usleep(2000000);
            continue;
        }
        unsigned long brightness_revision = s->pad_brightness_revision;
        int brightness_pending = 0;
        double brightness_deadline = 0;
        if (s->pad_brightness >= 1 && s->pad_brightness <= 6) {
            wupstream_backlight(pad, s->pad_brightness, 0);
            brightness_pending = s->pad_brightness;
            brightness_deadline = v98_clock() + 1.0;
        }
        set_status(srv, "GamePad connected, starting capture...");

        capture_t cap = {0};
        audio_out_t aout;
        audio_in_t ain;
        input_ctx_t *ictx = NULL;
        int have_aout = 0, have_ain = 0, cap_ok = 0, cap_fails = 0;
        /* Set WUP_CAPTURE_ASYNC=0 to compare against the original serial path. */
        const char *async_arg = getenv("WUP_CAPTURE_ASYNC");
        const int async_enabled = !async_arg || strcmp(async_arg, "0") != 0;
        capture_async_t *cap_async = NULL;

        int static_ok = 0;
        if (s->gamepad_only) {
            /* Honor Black screen before the FIRST encoded frame. */
            if (s->gamepad_only_black) memset(frame, 0, (size_t)FW * FH * 4);
            else build_test_frame(frame, s->language_id);
            static_ok = 1;
        } else if (region_is_cleared(s)) {
            memset(frame, 0, FW * FH * 4);
            set_status(srv, "region cleared: black screen");
        } else if (capture_start_retry(&cap, s, srv) == 0)
            cap_ok = 1;
        else {
            /* source permanently unavailable: keep pad, retry slower via outer loop */
            wupstream_close(pad);
            usleep(5000000);
            continue;
        }

        if (cap_ok && async_enabled) {
            cap_async = capture_async_start(&cap);
            if (!cap_async)
                set_status(srv, "async capture unavailable; using synchronous capture");
        }

        if (s->audio_pc_to_pad && !s->gamepad_only && audio_out_start(&aout, s->audio_monitor) == 0)
            have_aout = 1;
        if (s->audio_pad_to_pc && !s->gamepad_only && audio_in_start(&ain) == 0)
            have_ain = 1;
        ictx = input_init(s);
        if (ictx && s->input.mode == INMODE_GAMEPAD && !input_gamepad_ok(ictx))
            set_status(srv, "GamePad: /dev/uinput unavailable, using kb/mouse mode");

        srv->state = SRV_STREAMING;
        set_status(srv, "streaming");

        double audio_acc = 0, audio_last_t = 0;
        int frame_count = 0;
        wupstream_input_t in;
        uint32_t prev_btns = 0;
        int lost = 0;
        double sample_since = v98_clock();
        unsigned int sample_frames = 0, sample_fresh = 0, sample_capture_count = 0;
        unsigned int sample_dropped = 0; /* WUP_V913_DROP_METRICS */
        double sample_capture_ms = 0.0, sample_frame_ms = 0.0;
        double sample_x11_ms = 0.0, sample_scale_ms = 0.0;
        unsigned int capture_errors = 0;
        pthread_mutex_lock(&srv->mtx);
        memset(&srv->metrics, 0, sizeof srv->metrics);
        pthread_mutex_unlock(&srv->mtx);

        while (srv->running && srv->want_stream) {
            pthread_mutex_lock(&srv->mtx);
            int rst = srv->restart_capture, rlk = srv->reload_keys, rc = srv->reconnect, cg = srv->calib_gyro;
            srv->restart_capture = srv->reload_keys = srv->reconnect = srv->calib_gyro = 0;
            if (seen_generation != srv->settings_generation) {
                active_settings = srv->staged_settings;
                seen_generation = srv->settings_generation;
                /* Live GamePad-only black/test toggles must redraw. */
                static_ok = 0;
            }
            pthread_mutex_unlock(&srv->mtx);
            if (rc) break;
            if (s->pad_brightness_revision != brightness_revision) {
                brightness_revision = s->pad_brightness_revision;
                if (s->pad_brightness >= 1 && s->pad_brightness <= 6) {
                    wupstream_backlight(pad, s->pad_brightness, 0);
                    brightness_pending = s->pad_brightness;
                    brightness_deadline = v98_clock() + 1.0;
                }
            }
            if (rst) {
                cap_fails = 0;
                if (cap_async) { capture_async_free(cap_async); cap_async = NULL; }
                capture_stop(&cap);
                /* An intentionally cleared region does not call XGetImage
                 * or retry capture. Video switches to black immediately. */
                cap_ok = !s->gamepad_only && !region_is_cleared(s) &&
                         (capture_start(&cap, s) == 0);
                if (!cap_ok && !s->gamepad_only && !region_is_cleared(s))
                    cap_fails = 1;
                if (cap_ok && async_enabled)
                    cap_async = capture_async_start(&cap);
            }
            if (rlk && ictx) input_reload_keys(ictx, s);
            if (cg && ictx) { input_calib_start(ictx); set_status(srv, "gyro calibration: keep the pad flat and still..."); }

            /* Main thread only reads borrowed frames from the capture worker.
             * No additional frame memcpy is performed before the encoder. */
            const unsigned char *frame_pixels = frame;
            int frame_i420 = 0;
            int map_x = 0, map_y = 0, map_w = 0, map_h = 0;
            /* gamepad-only mode: static test frame, no capture at all */
            if (s->gamepad_only) {
                if (cap_ok) {
                    if (cap_async) { capture_async_free(cap_async); cap_async = NULL; }
                    capture_stop(&cap); cap_ok = 0;
                }
                if (!static_ok) {
                    if (s->gamepad_only_black) memset(frame, 0, FW * FH * 4);
                    else build_test_frame(frame, s->language_id);
                    static_ok = 1;
                }
            } else if (region_is_cleared(s)) {
                /* Live fallback: still transmit frames, audio and inputs.
                 * Never count this as a failed capture or drop the pad. */
                if (cap_ok) {
                    if (cap_async) { capture_async_free(cap_async); cap_async = NULL; }
                    capture_stop(&cap); cap_ok = 0;
                }
                memset(frame, 0, FW * FH * 4);
                static_ok = 0;
                cap_fails = 0;
            } else {
                static_ok = 0;
                /* capture dead or failing: restart capture only, keep pad session */
                if (!cap_ok) {
                    if (++cap_fails > CAP_MAX_FAILS) { lost = 1; break; }
                    set_status(srv, "capture failing, restarting (%d/%d)...", cap_fails, CAP_MAX_FAILS);
                    usleep(400000);
                    cap_ok = (capture_start(&cap, s) == 0);
                    if (cap_ok && async_enabled)
                        cap_async = capture_async_start(&cap);
                    continue;
                }

                if (cap_async && capture_async_failed(cap_async)) {
                    capture_async_sample_t taken;
                    capture_async_sample(cap_async, &taken);
                    capture_errors += taken.errors;
                    sample_dropped += taken.dropped;
                    sample_capture_count += taken.frames;
                    sample_capture_ms += taken.total_ms;
                    sample_x11_ms += taken.x11_ms;
                    sample_scale_ms += taken.scale_ms;
                    capture_async_free(cap_async); cap_async = NULL;
                    capture_stop(&cap); cap_ok = 0;
                    cap_fails = 1;
                    continue;
                }
                /* Only the synchronous path touches the capture object here. */
                if (!cap_async && (frame_count % 30) == 0 &&
                    s->source_type == SRC_WINDOW) {
                    int fr = capture_follow(&cap, s);
                    if (fr < 0) {
                        /* window gone or unreadable: mark capture dead */
                        capture_stop(&cap);
                        cap_ok = 0; cap_fails = 1;
                        continue;
                    }
                }

                if (cap_async) {
                    int fresh = 0;
                    frame_pixels = capture_async_latest(cap_async, &fresh,
                                   &map_x, &map_y, &map_w, &map_h);
                    if (!frame_pixels) {
                        /* Initial frame: worker is still capturing. */
                        usleep(1000);
                        continue;
                    }
                    if (fresh) sample_fresh++;
                } else {
                    double capture_beg = v98_clock();
                    if (capture_grab(&cap, frame) != 0) {
                        capture_errors++;
                        /* capture died: restart it, don't drop the pad */
                        capture_stop(&cap);
                        cap_ok = 0; cap_fails++;
                        if (cap_fails > CAP_MAX_FAILS) { lost = 1; break; }
                        set_status(srv, "capture lost, restarting (%d/%d)...", cap_fails, CAP_MAX_FAILS);
                        continue;
                    }
                    sample_capture_ms += (v98_clock() - capture_beg) * 1000.0;
                    sample_x11_ms += cap.last_x11_ms;
                    sample_scale_ms += cap.last_scale_ms;
                    sample_capture_count++;
                    sample_fresh++;
                    capture_target_rect(&cap, &map_x, &map_y, &map_w, &map_h);
                }
                frame_i420 = cap.capture_backend == 3 && wup_kms_i420_enabled();
                cap_fails = 0;
            }

            /* keep the touch->mouse map on the streamed area */
            if (ictx) {
                if (cap_ok) {
                    input_set_touch_map(ictx, map_x, map_y, map_w, map_h);
                } else {
                    input_set_touch_map(ictx, 0, 0, 0, 0);
                }
            }

            /* inputs */
            if (wupstream_input(pad, &in) != 0) { lost = 1; break; }
            pthread_mutex_lock(&srv->mtx);
            srv->last_in = in; srv->has_in = 1;
            /* Don't publish stale brightness while a new command is in flight. */
            if (brightness_pending) {
                if (in.backlight == brightness_pending || v98_clock() >= brightness_deadline)
                    brightness_pending = 0;
                else
                    srv->last_in.backlight = -1;
            }
            pthread_mutex_unlock(&srv->mtx);
            /* TV button: cycle monitors when streaming a full screen */
            if (((in.buttons ^ prev_btns) & WUPSTREAM_TV) &&
                (in.buttons & WUPSTREAM_TV) &&
                s->source_type == SRC_SCREEN && !s->gamepad_only)
                cycle_monitor(srv, s);
            prev_btns = in.buttons;

            if (ictx) {
                input_dispatch(ictx, s, &in);
                double gb[3];
                if (input_calib_poll(ictx, gb)) {
                    for (int i = 0; i < 3; i++) s->input.gyro_bias[i] = gb[i];
                    settings_save(s);
                    set_status(srv, "gyro calibrated (bias %.0f %.0f %.0f)", gb[0], gb[1], gb[2]);
                }
            }

            /* mic pad -> PC */
            if (have_ain && !s->gamepad_only) {
                int got = wupstream_mic(pad, mic_buf, 800);
                if (got > 0) audio_in_write(&ain, mic_buf, got);
            }

            /* canonical loop order: input -> frame -> audio */
            double frame_beg = v98_clock();
            if ((frame_i420 ? wupstream_frame_i420(pad, frame_pixels) :
                 wupstream_frame(pad, frame_pixels, FW * 4, WUPSTREAM_BGRA)) != 0) {
                lost = 1; break;
            }
            double frame_end = v98_clock();
            sample_frame_ms += (frame_end - frame_beg) * 1000.0;
            sample_frames++;
            frame_count++; /* debug/follow cadence applies to every source kind */
            double elapsed = frame_end - sample_since;
            if (elapsed >= 1.0) {
                if (cap_async) {
                    capture_async_sample_t taken;
                    capture_async_sample(cap_async, &taken);
                    sample_capture_count += taken.frames;
                    sample_capture_ms += taken.total_ms;
                    sample_x11_ms += taken.x11_ms;
                    sample_scale_ms += taken.scale_ms;
                    capture_errors += taken.errors;
                    sample_dropped += taken.dropped;
                }
                server_metrics_t m = {
                    .fps = sample_frames / elapsed,
                    .fresh_fps = (!cap_ok ? sample_frames : sample_fresh / elapsed),
                    .captured_fps = sample_capture_count / elapsed, /* WUP_V913 */
                    .repeated = sample_frames > sample_fresh && cap_ok
                        ? sample_frames - sample_fresh : 0,
                    .dropped = sample_dropped,
                    .capture_ms = sample_capture_count ? sample_capture_ms / sample_capture_count : 0.0,
                    .x11_ms = sample_capture_count ? sample_x11_ms / sample_capture_count : 0.0,
                    .scale_ms = sample_capture_count ? sample_scale_ms / sample_capture_count : 0.0,
                    .frame_call_ms = sample_frames ? sample_frame_ms / sample_frames : 0.0,
                    .capture_errors = capture_errors,
                    .valid = 1
                };
                pthread_mutex_lock(&srv->mtx);
                srv->metrics = m;
                pthread_mutex_unlock(&srv->mtx);
                sample_frames = sample_fresh = sample_capture_count = sample_dropped = 0;
                sample_capture_ms = sample_x11_ms = sample_scale_ms = sample_frame_ms = 0.0;
                sample_since = frame_end;
            }

            /* audio PC -> pad: paced by WALL CLOCK, not by video frames —
               a dropped/hitched frame must not starve the pad's speaker
               (capture2cloud decouples audio and video the same way) */
            if (have_aout && !s->gamepad_only) {
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                double t = ts.tv_sec + ts.tv_nsec / 1e9;
                if (audio_last_t == 0) audio_last_t = t;
                audio_acc += (t - audio_last_t) * 48000.0;
                audio_last_t = t;
                if (audio_acc > 2000) audio_acc = 2000;   /* bound latency; audio_buf holds 2048 frames */
                int nf = (int)audio_acc;
                audio_acc -= nf;
                if (nf > 0) {
                    audio_out_read(&aout, audio_buf, nf);
                    wupstream_audio(pad, audio_buf, nf);
                }
                if (getenv("WUPSTREAMD_DEBUG") && (frame_count % 300) == 0)
                    fprintf(stderr, "[audio] ring fill %.0f%% (~%.0f ms)\n",
                            audio_out_level(&aout) * 100.0,
                            audio_out_level(&aout) * 500.0);
            }
        }

        pthread_mutex_lock(&srv->mtx);
        memset(&srv->metrics, 0, sizeof srv->metrics);
        /* WUP_FIX_DISCONNECT_RELEASES: invalidate the last input report.
         * Otherwise the GTK visualization keeps HOME/B visually pressed
         * after the GamePad leaves (also on stop/reconnect/link loss).
         */
        memset(&srv->last_in, 0, sizeof srv->last_in);
        srv->has_in = 0;
        pthread_mutex_unlock(&srv->mtx);
        if (cap_async) { capture_async_free(cap_async); cap_async = NULL; }
        capture_stop(&cap);
        if (have_aout) audio_out_stop(&aout);
        if (have_ain) audio_in_stop(&ain);
        if (ictx) input_free(ictx);
        wupstream_close(pad);

        if (lost) {
            pthread_mutex_lock(&srv->mtx);
            int retry = srv->staged_settings.auto_reconnect && srv->want_stream;
            if (!retry) srv->want_stream = 0;
            pthread_mutex_unlock(&srv->mtx);
            if (retry) {
                srv->state = SRV_WAITING_PAD;
                set_status(srv, "GamePad disconnected - reconnecting");
                for (int i = 0; i < 20 && srv->running && srv->want_stream; ++i) {
                    pthread_mutex_lock(&srv->mtx);
                    if (!srv->staged_settings.auto_reconnect) srv->want_stream = 0;
                    pthread_mutex_unlock(&srv->mtx);
                    usleep(100000);
                }
                continue;
            }
            set_status(srv, "GamePad disconnected - stream stopped");
        } else {
            set_status(srv, "stopped");
        }
        srv->state = SRV_STOPPED;
        while (srv->running && !srv->want_stream) usleep(200000);
        if (!srv->running) break;
    }
    srv->running = 0;
    return NULL;
}

server_t *server_new(settings_t *s) {
    server_t *srv = calloc(1, sizeof *srv);
    srv->s = s;
    atomic_init(&srv->state, SRV_IDLE);
    atomic_init(&srv->running, 0);
    atomic_init(&srv->want_stream, 0);
    snprintf(srv->status, sizeof srv->status, "ready");
    pthread_mutex_init(&srv->mtx, NULL);
    srv->staged_settings = *s;
    snprintf(srv->last_ui_monitor, sizeof srv->last_ui_monitor,
             "%s", s->source.output);
    memcpy(srv->last_ui_gyro_bias, s->input.gyro_bias,
           sizeof srv->last_ui_gyro_bias);
    srv->settings_generation = 1;
    srv->running = 1;
    pthread_create(&srv->th, NULL, server_thread, srv);
    return srv;
}

void server_destroy(server_t *srv) {
    if (!srv) return;
    srv->want_stream = 0;
    srv->running = 0;
    pthread_join(srv->th, NULL);
    pthread_mutex_destroy(&srv->mtx);
    free(srv);
}

void server_request_stream(server_t *srv) { srv->want_stream = 1; }
void server_request_stop(server_t *srv) { srv->want_stream = 0; }
void server_restart_capture(server_t *srv) {
    server_sync_settings(srv);
    pthread_mutex_lock(&srv->mtx);
    srv->restart_capture = 1;
    pthread_mutex_unlock(&srv->mtx);
}
void server_request_reconnect(server_t *srv) {
    server_sync_settings(srv);
    pthread_mutex_lock(&srv->mtx);
    srv->reconnect = 1;
    pthread_mutex_unlock(&srv->mtx);
}
void server_apply_input_keys(server_t *srv) {
    server_sync_settings(srv);
    pthread_mutex_lock(&srv->mtx);
    srv->reload_keys = 1;
    pthread_mutex_unlock(&srv->mtx);
}
void server_calib_gyro(server_t *srv) {
    pthread_mutex_lock(&srv->mtx);
    srv->calib_gyro = 1;
    pthread_mutex_unlock(&srv->mtx);
}

int server_state(server_t *srv) { return get_state(srv); }

int server_input_snapshot(server_t *srv, wupstream_input_t *out) {
    pthread_mutex_lock(&srv->mtx);
    int ok = srv->has_in;
    if (ok) *out = srv->last_in;
    pthread_mutex_unlock(&srv->mtx);
    return ok;
}

const char *server_status(server_t *srv) {
    static char buf[256];
    pthread_mutex_lock(&srv->mtx);
    snprintf(buf, sizeof buf, "%s", srv->status);
    pthread_mutex_unlock(&srv->mtx);
    return buf;
}

/* WUP_V98_ALL_IN_ONE: cheap snapshot publication on the GTK thread only.
 * The existing per-frame command mutex is reused; no additional frame locks. */
void server_sync_settings(server_t *srv) {
    if (!srv) return;
    pthread_mutex_lock(&srv->mtx);
    /* Resolve worker-side updates only if the GUI has not explicitly
     * overridden the same field since its last published snapshot. */
    if (srv->monitor_dirty &&
        !strcmp(srv->s->source.output, srv->last_ui_monitor))
        snprintf(srv->s->source.output, sizeof srv->s->source.output,
                 "%s", srv->staged_settings.source.output);
    if (srv->gyro_dirty &&
        !memcmp(srv->s->input.gyro_bias, srv->last_ui_gyro_bias,
                sizeof srv->last_ui_gyro_bias))
        memcpy(srv->s->input.gyro_bias, srv->staged_settings.input.gyro_bias,
               sizeof srv->last_ui_gyro_bias);
    srv->monitor_dirty = srv->gyro_dirty = 0;
    srv->staged_settings = *srv->s;
    snprintf(srv->last_ui_monitor, sizeof srv->last_ui_monitor,
             "%s", srv->s->source.output);
    memcpy(srv->last_ui_gyro_bias, srv->s->input.gyro_bias,
           sizeof srv->last_ui_gyro_bias);
    srv->settings_generation++;
    pthread_mutex_unlock(&srv->mtx);
}

void server_metrics_snapshot(server_t *srv, server_metrics_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!srv) return;
    pthread_mutex_lock(&srv->mtx);
    *out = srv->metrics;
    pthread_mutex_unlock(&srv->mtx);
}
