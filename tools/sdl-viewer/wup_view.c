/* wup_view: SDL2 fake GamePad client for WUP Stream thin servers.
 *
 * Speaks the wire protocol (docs/PROTOCOL.md): B beacon -> H hello ->
 * I input reports -> V video -> N retransmit requests -> T mode requests.
 *
 * The 'V' payload is headerless H.264: raw CABAC slice data, no start
 * codes, no NAL header, emulation prevention stripped, slice header
 * assumed by the GamePad's hardware decoder. This client rebuilds a
 * normal Annex-B stream the same way tools/drcverify.py does upstream:
 * it writes back the slice header the encoder settings imply, then
 * feeds libavcodec.
 *
 * Input: SDL game controller -> synthesized 128-byte input report
 * (layout per upstream host/wupstream.c wupstream_input()). Mouse button
 * acts as the touch screen. Arrows/Enter/Backspace are a keyboard
 * fallback for dpad/A/B.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <SDL.h>

/* ---- protocol constants ------------------------------------------------- */
#define BEACON        "B\x01\x00\x00"
#define LISTEN_PORT   50000
#define FRAME_MAX     (128 * 1400u)
#define NASM          8
#define I_PERIOD_MS   10          /* ~100 Hz input reports */
#define TICKS         16666       /* us per 59.94 Hz refresh */
#define BACKLIGHT     5

/* SPS/PPS of the stream profile (854x480 Main, CABAC, poc_type 2,
 * log2_max_frame_num 4, init_qp 32, deblocking on) — captured from the
 * patched x264 with wupstream.c's drh-mode settings. Annex-B, already
 * emulation-prevention escaped. */
static const uint8_t SPS_NAL[] = {
    0x00,0x00,0x00,0x01,0x67,0x4d,0x40,0x1f,0xda,0x03,0x60,0xf7,0x9b,0x84,
    0x00,0x00,0x03,0x00,0x04,0x00,0x00,0x03,0x01,0xe2,0x3c,0x48,0x9a,0x80 };
static const uint8_t PPS_NAL[] = {
    0x00,0x00,0x00,0x01,0x68,0xee,0x06,0x7a };

/* ---- tiny bit writer (RBSP, MSB first) ---------------------------------- */
typedef struct { uint8_t *p; size_t n, cap; unsigned acc; int nbits; } bw_t;
static void bw_reserve(bw_t *w, size_t extra) {
    if (w->n + extra > w->cap) {
        w->cap = (w->n + extra) * 2 + 64;
        w->p = realloc(w->p, w->cap);
        if (!w->p) { fprintf(stderr, "oom\n"); exit(1); }
    }
}
static void bw_put(bw_t *w, uint32_t v, int nbits) {   /* nbits <= 25 */
    bw_reserve(w, 5);
    w->acc = (w->acc << nbits) | (v & ((1u << nbits) - 1));
    w->nbits += nbits;
    while (w->nbits >= 8) {
        w->nbits -= 8;
        w->p[w->n++] = (uint8_t)(w->acc >> w->nbits);
    }
    w->acc &= (1u << w->nbits) - 1;
}
static void bw_ue(bw_t *w, uint32_t v) {
    uint32_t x = v + 1; int l = 0;
    for (uint32_t t = x; t; t >>= 1) l++;
    bw_put(w, 0, l - 1);
    bw_put(w, x, l);
}
static void bw_se(bw_t *w, int32_t v) {
    bw_ue(w, v <= 0 ? (uint32_t)(-2 * v) : (uint32_t)(2 * v - 1));
}
static void bw_align1(bw_t *w) {                        /* cabac alignment: 1-bits */
    while (w->nbits) bw_put(w, 1, 1);
}

/* emulation-prevention escape of an RBSP */
static size_t rbsp_escape(const uint8_t *in, size_t n, uint8_t *out) {
    size_t o = 0; int zeros = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = in[i];
        if (zeros >= 2 && b <= 3) { out[o++] = 3; zeros = 0; }
        out[o++] = b;
        zeros = (b == 0) ? zeros + 1 : 0;
    }
    return o;
}

/* ---- frame reassembly ---------------------------------------------------- */
typedef struct {
    int known, idr;
    uint32_t frame, len, npk, have;
    uint8_t got[16];               /* per-packet presence bitmap (npk <= 128) */
    uint8_t *data;
} slot_t;
static slot_t slots[NASM];
static uint32_t next_decode = 0;
static int have_next = 0;         /* next_decode is valid */

static slot_t *slot_for(uint32_t frame) { return &slots[frame % NASM]; }

static void slot_reset(slot_t *s, uint32_t frame, uint32_t len, int idr) {
    s->known = 1; s->frame = frame; s->len = len; s->idr = idr;
    s->npk = (len + 1399) / 1400; s->have = 0;
    memset(s->got, 0, sizeof s->got);
    if (!s->data) s->data = malloc(FRAME_MAX);
}

/* returns 1 if a packet was consumed */
static int video_pkt(const uint8_t *d, size_t len) {
    if (len < 16) return 0;
    uint32_t frame, off, total;
    memcpy(&frame, d + 4, 4); memcpy(&off, d + 8, 4); memcpy(&total, d + 12, 4);
    size_t n = len - 16;
    int idr = d[1] & 1;
    slot_t *s = slot_for(frame);
    if (s->known && s->frame == frame && s->len == total && s->npk == (total + 1399) / 1400) {
        /* same frame, more packets */
    } else {
        if (!total || total > FRAME_MAX) return 0;
        slot_reset(s, frame, total, idr);
        if (!have_next) { next_decode = frame; have_next = 1; }
    }
    if (off % 1400 || off / 1400 >= s->npk || off + n > s->len ||
        (n != 1400 && off + n != s->len))
        return 1;
    uint32_t k = off / 1400;
    if (s->got[k >> 3] & (1 << (k & 7))) return 1;   /* duplicate (retransmit) */
    s->got[k >> 3] |= (uint8_t)(1 << (k & 7));
    memcpy(s->data + off, d + 16, n);
    s->have++;
    return 1;
}

/* is the slot holding next_decode complete? */
static slot_t *decode_slot(void) {
    if (!have_next) return NULL;
    slot_t *s = slot_for(next_decode);
    if (!s->known || s->frame != next_decode) return NULL;
    return (s->have == s->npk) ? s : NULL;
}
static void decode_done(void) {
    slot_t *s = slot_for(next_decode);
    s->known = 0;
    next_decode++;
}

/* ---- bitstream rebuild ---------------------------------------------------- */
/* Rebuild one frame as Annex-B: SPS/PPS + slice NAL with synthesized header. */
static size_t build_frame(const slot_t *s, uint8_t *out, size_t cap,
                          uint32_t *frame_num, int *idr_id) {
    int idr = s->idr;
    if (idr) *frame_num = 0;             /* frame_num restarts at IDR */
    bw_t w = {0};
    bw_ue(&w, 0);                 /* first_mb_in_slice */
    bw_ue(&w, idr ? 7 : 5);       /* slice_type: I (all) / P */
    bw_ue(&w, 0);                 /* pic_parameter_set_id */
    bw_put(&w, *frame_num & 0xf, 4);   /* frame_num (log2_max_frame_num = 4) */
    if (idr) {
        bw_ue(&w, (uint32_t)(*idr_id & 1));  /* idr_pic_id: must differ per IDR */
        *idr_id += 1;
    } else {
        bw_put(&w, 0, 1);        /* num_ref_idx_active_override_flag */
        bw_put(&w, 0, 1);        /* ref_pic_list_modification_flag_l0 */
    }
    *frame_num += 1;             /* drcverify: fn++ after every frame */
    /* nal_ref_idc = 3 -> dec_ref_pic_marking present */
    if (idr) { bw_put(&w, 0, 1); bw_put(&w, 0, 1); }       /* no_output, long_term_ref */
    else     { bw_put(&w, 0, 1); }                          /* adaptive_ref_pic_marking_mode_flag */
    if (!idr) bw_ue(&w, 0);         /* cabac_init_idc */
    bw_se(&w, 0);                   /* slice_qp_delta: 32 - init_qp(32) */
    bw_ue(&w, 0);                   /* disable_deblocking_filter_idc */
    bw_se(&w, 0); bw_se(&w, 0);     /* deblocking offsets */
    bw_align1(&w);                  /* cabac_alignment_one_bit */

    /* RBSP = header bytes ++ raw payload; escape; wrap in Annex-B */
    size_t hdr = w.n + (w.nbits ? 1 : 0);
    size_t need = hdr + s->len;
    uint8_t *rbsp = malloc(need + 8);
    memcpy(rbsp, w.p, w.n);
    if (w.nbits) rbsp[w.n] = (uint8_t)(w.acc << (8 - w.nbits));
    memcpy(rbsp + hdr, s->data, s->len);
    free(w.p);

    uint8_t *esc = malloc(need * 2 + 16);
    size_t en = rbsp_escape(rbsp, hdr + s->len, esc);
    free(rbsp);

    size_t o = 0;
    if (cap < o + 4 + 1 + en) { free(esc); return 0; }
    memcpy(out + o, "\x00\x00\x00\x01", 4); o += 4;
    out[o++] = idr ? 0x65 : 0x61;                    /* nal header: ref_idc 3 */
    memcpy(out + o, esc, en); o += en;
    free(esc);
    return o;
}

/* ---- input report --------------------------------------------------------- */
/* 128-byte report layout (upstream wupstream.c wupstream_input()):
 *  [0..1] seq u16 LE, [2..3] buttons u16 BE (bit0 = SYNC, unused),
 *  [4] power flags, [5] battery raw, [6..13] sticks u16 LE centered 0x800
 *  (order lx,ly,rx,ry; ~+-1100 typical full tilt, up/right positive),
 *  [14] volume, [15..29] accel/gyro, [36..75] 10 touch samples x,y u16
 *  (bit15 = valid), [0x50] L3/R3/TV bits. */
#define WUP_A 0x8000
#define WUP_B 0x4000
#define WUP_X 0x2000
#define WUP_Y 0x1000
#define WUP_LEFT 0x0800
#define WUP_RIGHT 0x0400
#define WUP_UP 0x0200
#define WUP_DOWN 0x0100
#define WUP_ZL 0x0080
#define WUP_ZR 0x0040
#define WUP_L 0x0020
#define WUP_R 0x0010
#define WUP_PLUS 0x0008
#define WUP_MINUS 0x0004
#define WUP_HOME 0x0002
#define WUP_L3 0x10000
#define WUP_R3 0x20000

static uint16_t g_buttons;
static double g_lx, g_ly, g_rx, g_ry;    /* -1..1, y up-positive */
static int g_l3, g_r3;
static int g_touch, g_tx, g_ty;          /* touch: 0..853, 0..479 or -1 */
static int g_seq;

static int16_t axis_to_stick(double v) {  /* SDL -32768..32767 -> raw offset */
    double d = v / 32768.0;
    if (fabs(d) < 0.18) d = 0;            /* deadzone */
    if (d > 1) d = 1;
    if (d < -1) d = -1;
    int raw = (int)lrint(d * 1400.0);
    return (int16_t)raw;
}

static void build_report(uint8_t r[128]) {
    memset(r, 0, 128);
    r[0] = (uint8_t)(g_seq & 0xff); r[1] = (uint8_t)(g_seq >> 8);
    g_seq++;
    r[2] = (uint8_t)(g_buttons >> 8); r[3] = (uint8_t)g_buttons;
    r[4] = 0x01;                 /* plugged in */
    r[5] = 150;                  /* battery raw -> ~64% */
    int16_t st[4] = { axis_to_stick(g_lx), axis_to_stick(g_ly),
                      axis_to_stick(g_rx), axis_to_stick(g_ry) };
    for (int i = 0; i < 4; i++) {
        uint16_t v = (uint16_t)(0x800 + st[i]);
        r[6 + 2 * i] = (uint8_t)v; r[7 + 2 * i] = (uint8_t)(v >> 8);
    }
    r[14] = 200;                 /* volume */
    if (g_touch >= 0) {
        /* inverse of wupstream.c touch calibration:
         * tx = (x-107)*854/(3980-107), ty = (3895-y)*480/(3895-155) */
        int xr = (int)lrint(g_tx * 3873.0 / 854.0 + 107.0);
        int yr = (int)lrint(3895.0 - g_ty * 3740.0 / 480.0);
        xr &= 0xfff; yr &= 0xfff;
        r[36] = (uint8_t)xr; r[37] = (uint8_t)(xr >> 8);
        r[38] = (uint8_t)yr; r[39] = (uint8_t)(yr >> 8);
        r[36] |= 0x80; r[38] |= 0x80;   /* bit15 set = valid */
    }
    if (g_l3) r[0x50] |= 0x80;
    if (g_r3) r[0x50] |= 0x40;
}

/* ---- globals for the main loop ------------------------------------------- */
static int sockfd = -1;
static struct sockaddr_in peer;
static int peer_on = 0;
static volatile int quit = 0;
static int want_keyframe = 1;
static int input_mode = -1;             /* -1: don't request */
static char shot_path[512] = "";
static int frames_decoded = 0, frames_lost = 0;

static void on_sigint(int s) { (void)s; quit = 1; }

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void sendto_peer(const void *d, size_t n) {
    if (peer_on)
        sendto(sockfd, d, n, 0, (struct sockaddr *)&peer, sizeof peer);
}

/* 'I' flags u16 ticks u8 backlight 0 0 0 + 128-byte report = 136 bytes */
static void send_input(uint16_t ticks) {
    uint8_t m[136];
    uint8_t rep[128];
    int waiting = 0;                    /* decoder queue depth, approx */
    build_report(rep);
    m[0] = 'I';
    m[1] = (uint8_t)(want_keyframe | (waiting << 1));
    m[2] = (uint8_t)ticks; m[3] = (uint8_t)(ticks >> 8);
    m[4] = BACKLIGHT; m[5] = m[6] = m[7] = 0;
    memcpy(m + 8, rep, 128);
    sendto_peer(m, sizeof m);
}

/* 'N' count 0 0 + count x {u32 frame, u16 packet, u16 0}; 0xffff = whole
 * frame. We ask for whole incomplete frames — a duplicate is harmless. */
static void send_nack(void) {
    uint8_t m[4 + 8 * 40];
    uint32_t c = 0;
    m[0] = 'N'; m[1] = m[2] = m[3] = 0;
    for (uint32_t f = next_decode; f < next_decode + NASM && c < 40; f++) {
        slot_t *s = slot_for(f);
        if (!s->known || s->frame != f || s->have == s->npk) continue;
        memcpy(m + 4 + 8 * c, &f, 4);
        uint16_t all = 0xffff;
        memcpy(m + 4 + 8 * c + 4, &all, 2);
        memset(m + 4 + 8 * c + 6, 0, 2);
        c++;
    }
    if (c) { m[1] = (uint8_t)c; sendto_peer(m, 4 + 8 * c); }
}

/* ---- decoder --------------------------------------------------------------- */
static AVCodecContext *dec_ctx;
static AVFrame *dec_frame;
static SDL_Texture *texture;
static SDL_Renderer *renderer;
static SDL_Window *window;
static int win_w = 854, win_h = 480;

static int decoder_open(void) {
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!c) { fprintf(stderr, "no h264 decoder\n"); return -1; }
    dec_ctx = avcodec_alloc_context3(c);
    dec_frame = av_frame_alloc();
    if (!dec_ctx || !dec_frame || avcodec_open2(dec_ctx, c, NULL) < 0) {
        fprintf(stderr, "decoder open failed\n"); return -1;
    }
    return 0;
}
static void decoder_close(void) {
    if (dec_ctx) avcodec_free_context(&dec_ctx);
    if (dec_frame) av_frame_free(&dec_frame);
}

static uint8_t *annexb;
static size_t annexb_cap;
static uint32_t cur_fn = 0;            /* frame_num across frames (4 bits) */
static int idr_id = 0;                 /* idr_pic_id toggles 0/1 per IDR */

static int decode_frame(const slot_t *s) {
    /* packet = SPS PPS + slice NAL (SPS/PPS every frame: ~50 B, keeps the
     * decoder in lock-step and survives any drift) */
    size_t need = sizeof SPS_NAL + sizeof PPS_NAL + s->len * 2 + 64;
    if (need > annexb_cap) { annexb = realloc(annexb, need); annexb_cap = need; }
    size_t o = 0;
    memcpy(annexb, SPS_NAL, sizeof SPS_NAL); o += sizeof SPS_NAL;
    memcpy(annexb + o, PPS_NAL, sizeof PPS_NAL); o += sizeof PPS_NAL;
    size_t sl = build_frame(s, annexb + o, annexb_cap - o, &cur_fn, &idr_id);
    if (!sl) return -1;
    o += sl;

    AVPacket *pkt = av_packet_alloc();
    pkt->data = annexb; pkt->size = (int)o;
    pkt->pts = frames_decoded;
    int ret = avcodec_send_packet(dec_ctx, pkt);
    av_packet_free(&pkt);
    if (ret < 0) { fprintf(stderr, "send_packet: %s\n", av_err2str(ret)); return -1; }
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, dec_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) { fprintf(stderr, "receive_frame: %s\n", av_err2str(ret)); return -1; }
        frames_decoded++;
        want_keyframe = 0;
        if (frames_decoded <= 3)
            fprintf(stderr, "frame %d: %dx%d fmt=%d linesize=%d,%d,%d ymean=%d\n",
                    frames_decoded, dec_frame->width, dec_frame->height,
                    dec_frame->format, dec_frame->linesize[0], dec_frame->linesize[1],
                    dec_frame->linesize[2],
                    dec_frame->data[0] ? (int)dec_frame->data[0][0] : -1);
        /* render */
        if (dec_frame->width != win_w || dec_frame->height != win_h) {
            win_w = dec_frame->width; win_h = dec_frame->height;
            SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                                        SDL_TEXTUREACCESS_STREAMING, win_w, win_h);
            SDL_SetWindowSize(window, win_w, win_h);
        }
        SDL_UpdateYUVTexture(texture, NULL,
                             dec_frame->data[0], dec_frame->linesize[0],
                             dec_frame->data[1], dec_frame->linesize[1],
                             dec_frame->data[2], dec_frame->linesize[2]);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        if (shot_path[0] && frames_decoded == 1) {
            SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, win_w, win_h, 32, SDL_PIXELFORMAT_RGBX8888);
            if (surf && SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGBX8888,
                                             surf->pixels, surf->pitch) == 0)
                SDL_SaveBMP(surf, shot_path);
            SDL_FreeSurface(surf);
            shot_path[0] = 0;
        }
        av_frame_unref(dec_frame);
    }
    return 0;
}

/* ---- SDL input ------------------------------------------------------------- */
static SDL_GameController *gc;
static Uint8 kb[SDL_NUM_SCANCODES];

static void poll_gamepad(void) {
    if (gc) {
        /* sticks */
        double lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX) / 32768.0;
        double ly = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY) / 32768.0;
        double rx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX) / 32768.0;
        double ry = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY) / 32768.0;
        g_lx = lx; g_ly = ly; g_rx = rx; g_ry = ry;
        g_buttons = 0;
        struct { SDL_GameControllerButton b; uint32_t bit; } map[] = {
            { SDL_CONTROLLER_BUTTON_A, WUP_A }, { SDL_CONTROLLER_BUTTON_B, WUP_B },
            { SDL_CONTROLLER_BUTTON_X, WUP_X }, { SDL_CONTROLLER_BUTTON_Y, WUP_Y },
            { SDL_CONTROLLER_BUTTON_DPAD_LEFT, WUP_LEFT }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, WUP_RIGHT },
            { SDL_CONTROLLER_BUTTON_DPAD_UP, WUP_UP }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, WUP_DOWN },
            { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, WUP_L }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, WUP_R },
            { SDL_CONTROLLER_BUTTON_START, WUP_PLUS }, { SDL_CONTROLLER_BUTTON_BACK, WUP_MINUS },
            { SDL_CONTROLLER_BUTTON_GUIDE, WUP_HOME },
        };
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (SDL_GameControllerGetButton(gc, map[i].b)) g_buttons |= map[i].bit;
        /* triggers -> ZL/ZR (digital threshold) */
        if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) g_buttons |= WUP_ZL;
        if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) g_buttons |= WUP_ZR;
        g_l3 = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK);
        g_r3 = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    } else {
        /* keyboard fallback: arrows = dpad, Return = A, Backspace = B,
         * Space = X, Tab = Y, WASD = left stick */
        g_buttons = 0; g_lx = g_ly = g_rx = g_ry = 0;
        if (kb[SDL_SCANCODE_UP]) g_buttons |= WUP_UP;
        if (kb[SDL_SCANCODE_DOWN]) g_buttons |= WUP_DOWN;
        if (kb[SDL_SCANCODE_LEFT]) g_buttons |= WUP_LEFT;
        if (kb[SDL_SCANCODE_RIGHT]) g_buttons |= WUP_RIGHT;
        if (kb[SDL_SCANCODE_RETURN]) g_buttons |= WUP_A;
        if (kb[SDL_SCANCODE_BACKSPACE]) g_buttons |= WUP_B;
        if (kb[SDL_SCANCODE_SPACE]) g_buttons |= WUP_X;
        if (kb[SDL_SCANCODE_TAB]) g_buttons |= WUP_Y;
        g_lx = (kb[SDL_SCANCODE_D] ? 1 : 0) - (kb[SDL_SCANCODE_A] ? 1 : 0);
        g_ly = (kb[SDL_SCANCODE_W] ? 1 : 0) - (kb[SDL_SCANCODE_S] ? 1 : 0);
    }
}

static void handle_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_QUIT: quit = 1; break;
    case SDL_KEYDOWN:
        if (e->key.keysym.sym == SDLK_ESCAPE) quit = 1;
        if (e->key.keysym.sym < SDL_NUM_SCANCODES) kb[e->key.keysym.sym] = 1;
        break;
    case SDL_KEYUP:
        if (e->key.keysym.sym < SDL_NUM_SCANCODES) kb[e->key.keysym.sym] = 0;
        break;
    case SDL_MOUSEBUTTONDOWN:
        if (e->button.button == SDL_BUTTON_LEFT) {
            g_touch = 1;
            g_tx = (int)(e->button.x * 854.0 / win_w);
            g_ty = (int)(e->button.y * 480.0 / win_h);
        }
        break;
    case SDL_MOUSEMOTION:
        if (g_touch) {
            g_tx = (int)(e->motion.x * 854.0 / win_w);
            g_ty = (int)(e->motion.y * 480.0 / win_h);
        }
        break;
    case SDL_MOUSEBUTTONUP:
        if (e->button.button == SDL_BUTTON_LEFT) g_touch = 0;
        break;
    case SDL_CONTROLLERDEVICEADDED:
        if (!gc) {
            gc = SDL_GameControllerOpen(e->cdevice.which);
            fprintf(stderr, "gamepad: %s\n", gc ? SDL_GameControllerName(gc) : "?");
        }
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        if (gc && e->cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc))) {
            SDL_GameControllerClose(gc); gc = NULL;
        }
        break;
    default: break;
    }
}

/* ---- main ------------------------------------------------------------------- */
static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s --server IP [--port N] [--mode M] [--screenshot BMP]\n"
        "  --port      daemon listen_port (default 50004)\n"
        "  --mode      send T request: 0 gamepad, 1 desktop, 255 toggle, -1 none (default -1)\n"
        "  --screenshot  save first decoded frame as BMP then continue\n", argv0);
    exit(2);
}

int main(int argc, char **argv) {
    const char *server_ip = NULL;
    int port = 50004;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--server") && i + 1 < argc) server_ip = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) input_mode = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) snprintf(shot_path, sizeof shot_path, "%s", argv[++i]);
        else usage(argv[0]);
    }
    if (!server_ip) usage(argv[0]);

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    /* SDL */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    SDL_GameControllerEventState(SDL_ENABLE);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    window = SDL_CreateWindow("wup-view", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              win_w, win_h, SDL_WINDOW_RESIZABLE);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, win_w, win_h);
    if (!window || !renderer || !texture) {
        fprintf(stderr, "SDL setup: %s\n", SDL_GetError()); return 1;
    }
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            gc = SDL_GameControllerOpen(i);
            fprintf(stderr, "gamepad: %s\n", SDL_GameControllerName(gc));
            break;
        }
    }

    /* UDP: bind our pad port, talk to the daemon's listen_port */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(LISTEN_PORT);
    if (bind(sockfd, (struct sockaddr *)&local, sizeof local) < 0) {
        fprintf(stderr, "bind :%d: %s\n", LISTEN_PORT, strerror(errno)); return 1;
    }
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &peer.sin_addr) != 1) {
        fprintf(stderr, "bad server ip %s\n", server_ip); return 1;
    }

    if (decoder_open() < 0) return 1;

    /* beacon until H */
    double beacon_next = 0, last_nack = 0, last_i = 0;
    uint16_t ticks = 0;
    int connected = 0;
    fprintf(stderr, "beaconing %s:%d ...\n", server_ip, port);
    while (!quit && !connected) {
        double t = now_ms();
        if (t >= beacon_next) {
            sendto(sockfd, BEACON, 4, 0, (struct sockaddr *)&peer, sizeof peer);
            beacon_next = t + 500;
        }
        uint8_t buf[2048];
        struct timeval tv = {0, 100000};
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ssize_t n = recvfrom(sockfd, buf, sizeof buf, 0, NULL, NULL);
        if (n > 0 && buf[0] == 'H') {
            peer_on = 1; connected = 1;
            fprintf(stderr, "connected: ver=%u flags=0x%02x mode=%u\n",
                    buf[1], buf[2], n > 4 ? buf[4] : 255);
            if (input_mode >= 0) {
                uint8_t tmsg[4] = { 'T', 1, (uint8_t)input_mode, 0 };
                sendto_peer(tmsg, 4);
            }
        }
        SDL_PumpEvents();
    }
    if (quit) goto out;

    /* main loop */
    fprintf(stderr, "streaming (q/Esc quit, mouse = touch)\n");
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handle_event(&e);
        poll_gamepad();

        /* drain socket */
        for (;;) {
            uint8_t buf[2048];
            ssize_t n = recvfrom(sockfd, buf, sizeof buf, MSG_DONTWAIT, NULL, NULL);
            if (n < 0) break;
            switch (buf[0]) {
            case 'H': break;                          /* keepalive */
            case 'V':
                video_pkt(buf, (size_t)n);
                /* deliver complete frames in order */
                while (decode_slot()) {
                    slot_t *s = decode_slot();
                    if (decode_frame(s) < 0) frames_lost++;
                    decode_done();
                }
                break;
            case 'N': break;                          /* server->client N unused */
            case 'Q': quit = 1; break;
            case 'T': break;                          /* mode ack */
            case 'R':                                  /* rumble -> gamepad */
                if (gc && n >= 4)
                    SDL_GameControllerRumble(gc, 0xffff, 0xffff, buf[1] ? 200 : 0);
                break;
            case 'A': break;                          /* audio: not decoded (yet) */
            default: break;
            }
        }

        /* send input at ~100 Hz */
        double t = now_ms();
        if (t - last_i >= I_PERIOD_MS) {
            ticks = (uint16_t)((ticks + TICKS) & 0xffff);
            send_input(ticks);
            last_i = t;
        }
        /* retransmit requests ~5 Hz while gaps exist */
        if (peer_on && t - last_nack > 200) {
            send_nack();
            last_nack = t;
            /* stale incomplete head -> ask for a keyframe */
            slot_t *s = have_next ? slot_for(next_decode) : NULL;
            if (s && s->known && s->frame == next_decode && s->have != s->npk)
                want_keyframe = 1;
        }
        SDL_Delay(2);
    }

    /* tell the server we're done */
    sendto_peer("Q", 1);
out:
    decoder_close();
    if (annexb) free(annexb);
    for (int i = 0; i < NASM; i++) free(slots[i].data);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    close(sockfd);
    fprintf(stderr, "decoded=%d lost=%d\n", frames_decoded, frames_lost);
    return 0;
}
