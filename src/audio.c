#define _GNU_SOURCE
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pulse/thread-mainloop.h>
#include <pulse/context.h>
#include <pulse/stream.h>
#include <pulse/error.h>

#define RATE 48000
#define RING_CAP RATE

static void low_latency_attr(pa_buffer_attr *attr, int channels) {
    memset(attr, 0, sizeof *attr);
    attr->maxlength = (uint32_t)-1;
    attr->tlength = (uint32_t)-1;
    attr->prebuf = (uint32_t)-1;
    attr->minreq = (uint32_t)-1;
    attr->fragsize = (uint32_t)(64 * channels * 2);
}

/* playback side: tiny target buffer so the pad mic reaches the PC fast
   (capture2cloud recipe: tlength = 2 chunks, minreq = 1 chunk) */
static void playback_attr(pa_buffer_attr *attr, int channels) {
    memset(attr, 0, sizeof *attr);
    uint32_t chunk = 256 * channels * 2;      /* ~5 ms */
    attr->maxlength = chunk * 4;
    attr->tlength = chunk * 2;
    attr->prebuf = 0;
    attr->minreq = chunk;
    attr->fragsize = (uint32_t)-1;
}

static void ring_init(int16_t **ring, size_t *cap) {
    *cap = RING_CAP;
    *ring = malloc(*cap * sizeof(int16_t));
}

static size_t ring_write(int16_t *ring, size_t cap, size_t *head, size_t *tail,
                         const int16_t *src, size_t n, int drop_oldest) {
    size_t free_ = (*tail - *head - 1 + cap) % cap;
    if (n > free_) {
        if (!drop_oldest) n = free_;
        else { size_t over = n - free_; *head = (*head + over) % cap; }
    }
    for (size_t i = 0; i < n; i++)
        ring[(*head + i) % cap] = src[i];
    *head = (*head + n) % cap;
    return n;
}

static size_t ring_read(int16_t *ring, size_t cap, size_t *head, size_t *tail,
                        int16_t *dst, size_t n, int zero_fill) {
    size_t avail = (*head - *tail + cap) % cap;
    size_t got = avail < n ? avail : n;
    for (size_t i = 0; i < n; i++) {
        if (i < got) dst[i] = ring[(*tail + i) % cap];
        else if (zero_fill) dst[i] = 0;
    }
    *tail = (*tail + got) % cap;
    return got;
}

static int resolve_monitor(char *out, size_t n, const char *monitor_name) {
    if (!monitor_name || !monitor_name[0] ||
        !strcmp(monitor_name, "default") ||
        !strcmp(monitor_name, "@DEFAULT_MONITOR@")) {
        FILE *p = popen("pactl get-default-sink 2>/dev/null", "r");
        if (!p) return -1;
        char sink[256] = {0};
        if (!fgets(sink, sizeof sink, p)) { pclose(p); return -1; }
        pclose(p);
        sink[strcspn(sink, "\r\n")] = 0;
        if (!sink[0]) return -1;
        snprintf(out, n, "%s.monitor", sink);
        return 0;
    }
    snprintf(out, n, "%s", monitor_name);
    return 0;
}

typedef struct {
    pa_threaded_mainloop *ml;
    pa_context *ctx;
    pa_stream *st;
} pulse_t;

static int pulse_connect(pulse_t *p, const char *name) {
    memset(p, 0, sizeof *p);
    p->ml = pa_threaded_mainloop_new();
    if (!p->ml) return -1;
    p->ctx = pa_context_new(pa_threaded_mainloop_get_api(p->ml), name);
    if (!p->ctx) return -1;
    if (pa_context_connect(p->ctx, NULL, 0, NULL) < 0) return -1;
    pa_threaded_mainloop_start(p->ml);
    for (int i = 0; i < 100; i++) {
        pa_context_state_t st = pa_context_get_state(p->ctx);
        if (st == PA_CONTEXT_READY) return 0;
        if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED) return -1;
        usleep(20000);
    }
    return -1;
}

static void pulse_disconnect(pulse_t *p) {
    if (p->st) { pa_stream_disconnect(p->st); pa_stream_unref(p->st); p->st = NULL; }
    if (p->ctx) { pa_context_disconnect(p->ctx); pa_context_unref(p->ctx); p->ctx = NULL; }
    if (p->ml) { pa_threaded_mainloop_stop(p->ml); pa_threaded_mainloop_free(p->ml); p->ml = NULL; }
}

/* ---------- PC monitor -> pad ---------- */

static void aout_cb(pa_stream *st, size_t nbytes, void *userdata) {
    audio_out_t *a = userdata;
    (void)nbytes;
    const void *data = NULL;
    size_t have = 0;
    pthread_mutex_lock(&a->mtx);
    if (pa_stream_peek(st, &data, &have) == 0 && data && have)
        ring_write(a->ring, a->cap, &a->head, &a->tail,
                   data, have / sizeof(int16_t), 1);
    pthread_mutex_unlock(&a->mtx);
    pa_stream_drop(st);
}

int audio_out_start(audio_out_t *a, const char *monitor_name) {
    memset(a, 0, sizeof *a);
    pthread_mutex_init(&a->mtx, NULL);
    ring_init(&a->ring, &a->cap);
    a->head = a->tail = 0;

    char dev[300];
    if (resolve_monitor(dev, sizeof dev, monitor_name) != 0) return -1;

    pulse_t *p = calloc(1, sizeof *p);
    if (!p) return -1;
    if (pulse_connect(p, "wupstreamd-out") != 0) { free(p); return -1; }

    pa_sample_spec ss = { .format = PA_SAMPLE_S16LE, .rate = RATE, .channels = 2 };
    pa_channel_map map;
    pa_channel_map_init_stereo(&map);
    pa_buffer_attr attr; low_latency_attr(&attr, 2);

    p->st = pa_stream_new(p->ctx, "PC monitor", &ss, &map);
    if (!p->st) { pulse_disconnect(p); free(p); return -1; }
    pa_stream_set_read_callback(p->st, aout_cb, a);
    pa_stream_flags_t flags = PA_STREAM_ADJUST_LATENCY |
        PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE |
        PA_STREAM_NOT_MONOTONIC;
    if (pa_stream_connect_record(p->st, dev, &attr, flags) < 0) {
        pulse_disconnect(p); free(p); return -1;
    }
    a->ps = p;
    a->running = 1;
    return 0;
}

void audio_out_stop(audio_out_t *a) {
    a->running = 0;
    pulse_t *p = a->ps;
    a->ps = NULL;
    if (p) { pulse_disconnect(p); free(p); }
    pthread_mutex_destroy(&a->mtx);
    free(a->ring); a->ring = NULL;
}

float audio_out_level(const audio_out_t *a) {
    if (!a->ring || !a->cap) return 0;
    size_t occ = (a->head - a->tail + a->cap) % a->cap;
    return (float)occ / (float)a->cap;
}

void audio_out_read(audio_out_t *a, int16_t *dst, int frames) {
    size_t want = (size_t)frames * 2;
    pthread_mutex_lock(&a->mtx);
    ring_read(a->ring, a->cap, &a->head, &a->tail, dst, want, 1);
    pthread_mutex_unlock(&a->mtx);
}

/* ---------- pad mic -> PC ---------- */

static void ain_read_cb(pa_stream *st, size_t nbytes, void *userdata) {
    audio_in_t *a = userdata;
    (void)nbytes;
    int16_t *buf = malloc(nbytes ? nbytes : 960 * 2);
    if (!buf) return;
    pthread_mutex_lock(&a->mtx);
    size_t want = nbytes / sizeof(int16_t);
    size_t got = ring_read(a->ring, a->cap, &a->head, &a->tail, buf, want, 1);
    pthread_mutex_unlock(&a->mtx);
    if (got)
        pa_stream_write(st, buf, got * sizeof(int16_t), NULL, 0, PA_SEEK_RELATIVE);
    free(buf);
}

int audio_in_start(audio_in_t *a) {
    memset(a, 0, sizeof *a);
    pthread_mutex_init(&a->mtx, NULL);
    ring_init(&a->ring, &a->cap);
    a->head = a->tail = 0;

    pulse_t *p = calloc(1, sizeof *p);
    if (!p) return -1;
    if (pulse_connect(p, "wupstreamd-in") != 0) { free(p); return -1; }

    pa_sample_spec ss = { .format = PA_SAMPLE_S16LE, .rate = RATE, .channels = 1 };
    pa_channel_map map;
    pa_channel_map_init_mono(&map);
    pa_buffer_attr attr; playback_attr(&attr, 1);

    p->st = pa_stream_new(p->ctx, "GamePad mic", &ss, &map);
    if (!p->st) { pulse_disconnect(p); free(p); return -1; }
    pa_stream_set_write_callback(p->st, ain_read_cb, a);
    pa_stream_flags_t flags = PA_STREAM_ADJUST_LATENCY |
        PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE |
        PA_STREAM_NOT_MONOTONIC | PA_STREAM_EARLY_REQUESTS;
    if (pa_stream_connect_playback(p->st, NULL, &attr, flags, NULL, NULL) < 0) {
        pulse_disconnect(p); free(p); return -1;
    }
    a->ps = p;
    a->running = 1;
    return 0;
}

void audio_in_stop(audio_in_t *a) {
    a->running = 0;
    pulse_t *p = a->ps;
    a->ps = NULL;
    if (p) { pulse_disconnect(p); free(p); }
    pthread_mutex_destroy(&a->mtx);
    free(a->ring); a->ring = NULL;
}

void audio_in_write(audio_in_t *a, const int16_t *src, int n) {
    pthread_mutex_lock(&a->mtx);
    ring_write(a->ring, a->cap, &a->head, &a->tail, src, (size_t)n, 1);
    pthread_mutex_unlock(&a->mtx);
}
