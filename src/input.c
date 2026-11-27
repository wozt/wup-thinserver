#include "input.h"
#include "x11util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <sys/ioctl.h>
#include <time.h>
#include <strings.h>
#include <glib.h>
#include <linux/uinput.h>
#include <linux/input.h>

const char *wbtn_names[WBTN_COUNT] = {
    "A", "B", "X", "Y", "←", "→", "↑", "↓",
    "ZL", "ZR", "L", "R", "+", "−", "⌂", "L3", "R3", "TV"
};

/* WUPSTREAM_* bit per button index */
static const uint32_t wbtn_bit[WBTN_COUNT] = {
    WUPSTREAM_A, WUPSTREAM_B, WUPSTREAM_X, WUPSTREAM_Y,
    WUPSTREAM_LEFT, WUPSTREAM_RIGHT, WUPSTREAM_UP, WUPSTREAM_DOWN,
    WUPSTREAM_ZL, WUPSTREAM_ZR, WUPSTREAM_L, WUPSTREAM_R,
    WUPSTREAM_PLUS, WUPSTREAM_MINUS, WUPSTREAM_HOME,
    WUPSTREAM_L3, WUPSTREAM_R3, WUPSTREAM_TV
};

/* evdev codes per button index (gamepad mode) */
static const uint16_t wbtn_evkey[WBTN_COUNT] = {
    BTN_EAST, BTN_SOUTH, BTN_NORTH, BTN_WEST,          /* A B X Y: physical Nintendo positions */
    BTN_DPAD_LEFT, BTN_DPAD_RIGHT, BTN_DPAD_UP, BTN_DPAD_DOWN,
    BTN_TL2, BTN_TR2,                                   /* ZL ZR analog triggers -> digital */
    BTN_TL, BTN_TR,                                     /* L R shoulders */
    BTN_START, BTN_SELECT,                              /* + - */
    BTN_MODE,                                           /* HOME */
    BTN_THUMBL, BTN_THUMBR,                             /* L3 R3 */
    BTN_TRIGGER_HAPPY1                                 /* TV: gamepad-only auxiliary button */
};

struct input_ctx {
    int uifd;                    /* -1 if not in gamepad mode */
    int active_mode;             /* actual mode; may fall back to Desktop */
    char gamepad_error[160];
    Display *dpy;                /* XTest connection (kbmouse mode) */
    uint32_t prev_buttons;
    int prev_lx, prev_ly, prev_rx, prev_ry;
    int prev_touched, prev_tx, prev_ty;
    unsigned prev_stick_dirs;
    int pad_volume;              /* accepted physical position (0..255) */
    int vol_candidate;           /* anchor of the current stable window */
    double vol_candidate_since;  /* monotonic seconds */
    double vol_last;             /* last time we spawned pactl */
    /* mouse accumulators */
    double mx, my;               /* pending relative motion */
    double sx, sy;               /* pending scroll clicks (fractional) */
    int mb_down;                 /* X11 mouse buttons currently held (bit n-1 = button n) */
    uint32_t chord_down;         /* pad buttons whose keysym chord is currently held */
    double grx, gry;             /* gyro-integrated right stick */
    int scr_w, scr_h;
    /* gyro calibration */
    double gyro_bias[3];
    int calib_active, calib_done;
    double calib_acc[3];
    int calib_n;
    /* touch gesture state machine */
    double touch_t0;             /* when current touch started */
    double touch_last_up;        /* when previous touch ended */
    int touch_taps;              /* consecutive taps in the current burst */
    int touch_gesture;           /* 0=undecided, 1=drag, 2=scroll, 3=rclick held */
    int touch_ly;                /* last touch y for scroll deltas */
    int touch_px, touch_py;      /* last warped pointer pos */
    int touch_ax, touch_ay;      /* anchor point (pad coords) for slop */
    int map_x, map_y, map_w, map_h, map_valid;  /* touch -> mouse target rect */
};

/* ---------- uinput virtual gamepad ---------- */

static int uinput_fail(int fd, char *error, size_t error_size,
                       const char *operation) {
    if (error && error_size)
        snprintf(error, error_size, "%s: %s", operation, strerror(errno));
    if (fd >= 0) close(fd);
    return -1;
}

static int uinput_open(char *error, size_t error_size) {
    /* WUP_UINPUT_PATH: containers may expose uinput under a differently
       named node (e.g. a privately mknod'd /dev/uinput-wup) so the host's
       device permissions stay untouched. Defaults to /dev/uinput. */
    const char *path = getenv("WUP_UINPUT_PATH");
    int fd = open(path && path[0] ? path : "/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return uinput_fail(-1, error, error_size, "cannot open /dev/uinput");

    if (ioctl(fd, UI_SET_EVBIT, EV_SYN) != 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_KEY) != 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_ABS) != 0)
        return uinput_fail(fd, error, error_size, "cannot configure uinput event types");

    static const uint16_t keys[] = {
        BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
        BTN_DPAD_LEFT, BTN_DPAD_RIGHT, BTN_DPAD_UP, BTN_DPAD_DOWN,
        BTN_TL2, BTN_TR2, BTN_TL, BTN_TR,
        BTN_START, BTN_SELECT, BTN_MODE,
        BTN_THUMBL, BTN_THUMBR, BTN_TRIGGER_HAPPY1
    };
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++)
        if (ioctl(fd, UI_SET_KEYBIT, keys[i]) != 0)
            return uinput_fail(fd, error, error_size, "cannot configure uinput buttons");

    static const uint16_t axes[] = { ABS_X, ABS_Y, ABS_RX, ABS_RY };
    for (size_t i = 0; i < sizeof(axes)/sizeof(axes[0]); i++) {
        if (ioctl(fd, UI_SET_ABSBIT, axes[i]) != 0)
            return uinput_fail(fd, error, error_size, "cannot configure uinput axes");
        struct uinput_abs_setup st;
        memset(&st, 0, sizeof st);
        st.code = axes[i];
        st.absinfo.minimum = -32767;
        st.absinfo.maximum = 32767;
        if (ioctl(fd, UI_ABS_SETUP, &st) != 0)
            return uinput_fail(fd, error, error_size, "cannot configure uinput axis range");
    }

    struct uinput_user_dev ud;
    memset(&ud, 0, sizeof ud);
    snprintf(ud.name, UINPUT_MAX_NAME_SIZE, "WUP ThinServer Gamepad");
    ud.id.bustype = BUS_USB;
    ud.id.vendor = 0x057e;   /* Nintendo */
    ud.id.product = 0x2001;
    ud.id.version = 1;
    if (write(fd, &ud, sizeof ud) != (ssize_t)sizeof ud)
        return uinput_fail(fd, error, error_size, "cannot configure uinput identity");
    if (ioctl(fd, UI_DEV_CREATE) != 0)
        return uinput_fail(fd, error, error_size, "cannot create virtual gamepad");
    if (error && error_size) error[0] = 0;
    return fd;
}

static void uinput_emit(input_ctx_t *ctx, int type, int code, int value) {
    if (ctx->uifd < 0) return;
    struct input_event ev = { .type = (uint16_t)type, .code = (uint16_t)code, .value = value };
    write(ctx->uifd, &ev, sizeof ev);
}

static void uinput_syn(input_ctx_t *ctx) {
    uinput_emit(ctx, EV_SYN, SYN_REPORT, 0);
}

static int scale_stick(int raw, int deadzone) {
    if (abs(raw) < deadzone) return 0;
    /* rescale so deadzone edge -> small value, full -> 32767 */
    int v = raw * 32767 / 1100;
    if (v > 32767) v = 32767;
    if (v < -32767) v = -32767;
    return v;
}

/* ---------- kb/mouse mode ---------- */

/* a key chord: "Ctrl+Alt+G" -> mods pressed first, then the key.
   mods are stored in press order; release is reverse. */
typedef struct {
    KeySym key;
    KeySym mods[4];
    int nmods;
} chord_t;

static chord_t btn_chord[WBTN_COUNT];
static chord_t stick_chord[8];
static int keys_loaded = 0;
static void input_release_held(input_ctx_t *ctx);

/* friendly modifier names -> X keysym names */
static const char *mod_keysym_name(const char *tok) {
    if (!strcasecmp(tok, "ctrl") || !strcasecmp(tok, "control") || !strcasecmp(tok, "ctl"))
        return "Control_L";
    if (!strcasecmp(tok, "alt") || !strcasecmp(tok, "option"))
        return "Alt_L";
    if (!strcasecmp(tok, "shift"))
        return "Shift_L";
    if (!strcasecmp(tok, "super") || !strcasecmp(tok, "meta") ||
        !strcasecmp(tok, "win") || !strcasecmp(tok, "cmd") || !strcasecmp(tok, "command"))
        return "Super_L";
    return NULL;
}

/* parse "Ctrl+Alt+G" / "a" / "F5" / "" into a chord. Unknown -> key=NoSymbol. */
static void parse_chord(const char *str, chord_t *out) {
    memset(out, 0, sizeof *out);
    out->key = NoSymbol;
    if (!str || !*str) return;
    char buf[96];
    snprintf(buf, sizeof buf, "%s", str);
    char *toks[8];
    int n = 0;
    for (char *t = strtok(buf, "+"); t && n < 8; t = strtok(NULL, "+")) {
        while (*t == ' ') t++;
        char *e = t + strlen(t);
        while (e > t && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (*t) toks[n++] = t;
    }
    if (!n) return;
    /* last token = the key; earlier ones = modifiers */
    KeySym ks = xkeysym_for_name(toks[n - 1]);
    if (ks == NoSymbol) return;
    out->key = ks;
    for (int i = 0; i < n - 1 && out->nmods < 4; i++) {
        const char *mn = mod_keysym_name(toks[i]);
        KeySym m = xkeysym_for_name(mn ? mn : toks[i]);
        if (m != NoSymbol) out->mods[out->nmods++] = m;
    }
}

static void apply_keysyms(const input_cfg_t *cfg);

static void load_keysyms(const input_cfg_t *cfg) {
    if (keys_loaded) return;
    apply_keysyms(cfg);
    keys_loaded = 1;
}

static void apply_keysyms(const input_cfg_t *cfg) {
    for (int i = 0; i < WBTN_COUNT; i++)
        parse_chord(cfg->keys[i], &btn_chord[i]);
    for (int i = 0; i < 8; i++)
        parse_chord(cfg->stick_keys[i], &stick_chord[i]);
}

void input_reload_keys(input_ctx_t *ctx, const settings_t *s) {
    /* Release using the old parsed mapping before replacing it. This is
       required for live rebinding: otherwise a held modifier or mouse
       button can remain down forever after its binding disappears. */
    if (ctx) input_release_held(ctx);
    apply_keysyms(&s->input);
}

int input_gamepad_ok(input_ctx_t *ctx) {
    return ctx && ctx->uifd >= 0;
}

int input_active_mode(input_ctx_t *ctx) {
    return ctx ? ctx->active_mode : INMODE_KBMOUSE;
}

const char *input_gamepad_error(input_ctx_t *ctx) {
    return ctx && ctx->gamepad_error[0] ? ctx->gamepad_error : "virtual gamepad unavailable";
}

int input_probe_gamepad(char *error, size_t error_size) {
    int fd = uinput_open(error, error_size);
    if (fd < 0) return -1;
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 0;
}

void input_calib_start(input_ctx_t *ctx) {
    if (!ctx) return;
    memset(ctx->calib_acc, 0, sizeof ctx->calib_acc);
    ctx->calib_n = 0;
    ctx->calib_done = 0;
    ctx->calib_active = 1;
}

/* returns 1 when a fresh bias is available in out[] */
int input_calib_poll(input_ctx_t *ctx, double out[3]) {
    if (!ctx || !ctx->calib_done) return 0;
    ctx->calib_done = 0;
    memcpy(out, ctx->gyro_bias, sizeof ctx->gyro_bias);
    return 1;
}

static void chord_press(input_ctx_t *ctx, const chord_t *c, int down) {
    Display *d = ctx->dpy;
    if (down) {
        for (int i = 0; i < c->nmods; i++) xtest_key(d, c->mods[i], 1);
        xtest_key(d, c->key, 1);
    } else {
        xtest_key(d, c->key, 0);
        for (int i = c->nmods - 1; i >= 0; i--) xtest_key(d, c->mods[i], 0);
    }
}

/* WUP_THIN_CMDS: run a configured shell command on a button press edge.
   Detached: the stream thread never waits on the command. Children are
   reaped automatically by GSpawn (no DO_NOT_REAP_CHILD: that flag left a
   zombie per toggle). */
static void run_button_command(const char *cmd) {
    if (!cmd || !cmd[0]) return;
    gchar *argv[] = {"/bin/sh", "-c", (gchar *)cmd, NULL};
    GError *error = NULL;
    if (!g_spawn_async(NULL, argv, NULL,
            G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
            G_SPAWN_STDERR_TO_DEV_NULL,
            NULL, NULL, NULL, &error)) {
        g_warning("button command failed to launch: %s", error->message);
        g_clear_error(&error);
    }
}

static void kb_buttons(input_ctx_t *ctx, const settings_t *s, uint32_t buttons) {
    uint32_t changed = buttons ^ ctx->prev_buttons;
    for (int i = 0; i < WBTN_COUNT; i++) {
        if (!(changed & wbtn_bit[i])) continue;
        int down = (buttons & wbtn_bit[i]) != 0;
        /* WUP_THIN_CMDS: a configured command owns the button entirely —
           it fires on the press edge and produces no key/mouse state, so
           the release edge must not fall through to a keysym or mouse
           release that was never pressed. */
        if (s->input.commands[i][0]) {
            if (down) run_button_command(s->input.commands[i]);
            continue;
        }
        /* WUP_THIN_CMDS: configured X11 mouse button beats the keysym chord.
           Held state is tracked in mb_down so disconnect can release it. */
        int mb = s->input.mouse_buttons[i];
        if (mb >= 1 && mb <= 5) {
            xtest_button(ctx->dpy, mb, down);
            if (down) {
                ctx->mb_down |= 1 << (mb - 1);
            } else {
                /* another held pad button may share this X button */
                int still = 0;
                for (int j = 0; j < WBTN_COUNT; j++)
                    if (j != i && (buttons & wbtn_bit[j]) &&
                        s->input.mouse_buttons[j] == mb) { still = 1; break; }
                if (!still) ctx->mb_down &= ~(1 << (mb - 1));
            }
            continue;
        }
        const chord_t *c = &btn_chord[i];
        if (c->key == NoSymbol) continue;
        chord_press(ctx, c, down);
        if (down) ctx->chord_down |= wbtn_bit[i];
        else ctx->chord_down &= ~wbtn_bit[i];
    }
}

/* stick direction -> key chord (kb/mouse): 8 cardinal dirs, edge-triggered.
   LX-,LX+,LY-,LY+,RX-,RX+,RY-,RY+ ; a diagonal presses both keys. */
static void kb_stick_keys(input_ctx_t *ctx, const settings_t *s,
                          const wupstream_input_t *in) {
    (void)s;
    int dz = s->input.deadzone;
    int inv = s->input.invert_y;
    int v[8];
    v[0] = in->lx < -dz;  v[1] = in->lx > dz;
    v[2] = (inv ? -in->ly : in->ly) < -dz;  v[3] = (inv ? -in->ly : in->ly) > dz;
    v[4] = in->rx < -dz;  v[5] = in->rx > dz;
    v[6] = (inv ? -in->ry : in->ry) < -dz;  v[7] = (inv ? -in->ry : in->ry) > dz;
    unsigned dirs = 0;
    for (int i = 0; i < 8; i++) if (v[i]) dirs |= 1u << i;
    unsigned diff = dirs ^ ctx->prev_stick_dirs;
    for (int i = 0; i < 8; i++) {
        if (!(diff & (1u << i))) continue;
        const chord_t *c = &stick_chord[i];
        if (c->key == NoSymbol) continue;
        chord_press(ctx, c, (dirs & (1u << i)) != 0);
    }
    ctx->prev_stick_dirs = dirs;
}

/* Wii U gamepad gyro: s24 raw, ~+-2000 dps full scale -> ~4194 raw per dps */
#define GYRO_RAW_PER_DPS 4194.0
/* ignore noise below ~0.3 dps */
#define GYRO_DPS_DEADZONE 0.3
/* mouse: pixels per second per dps at sens=100% */
#define GYRO_MOUSE_PX_PER_S 900.0
/* stick: stick units (of 32767) per dps at sens=100% */
#define GYRO_STICK_PER_DPS 60.0
#define CALIB_SAMPLES 180          /* ~3 s at 60 Hz */

/* corrected + filtered gyro in deg/s; returns via out[3] */
static void gyro_dps(const input_ctx_t *ctx, const wupstream_input_t *in, double out[3]) {
    for (int i = 0; i < 3; i++) {
        double r = (in->gyro[i] - ctx->gyro_bias[i]) / GYRO_RAW_PER_DPS;
        if (fabs(r) < GYRO_DPS_DEADZONE) r = 0;
        if (r > 2000) r = 2000;
        if (r < -2000) r = -2000;
        out[i] = r;
    }
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void kb_pointer(input_ctx_t *ctx, const settings_t *s, const wupstream_input_t *in) {
    Display *d = ctx->dpy;
    const input_cfg_t *ic = &s->input;
    int dz = ic->deadzone;

    /* touch maps onto the streamed area (monitor/window/region), not the
       whole X desktop: px = map_x + tx * map_w / 853 */
    int mx0 = 0, my0 = 0, mw = ctx->scr_w, mh = ctx->scr_h;
    if (ctx->map_valid && ctx->map_w > 0 && ctx->map_h > 0) {
        mx0 = ctx->map_x; my0 = ctx->map_y; mw = ctx->map_w; mh = ctx->map_h;
    }
#define TOUCH_PX(tx_) ({ int _v = mx0 + (int)((tx_) * (double)mw / 853.0); \
    _v < mx0 ? mx0 : (_v >= mx0 + mw ? mx0 + mw - 1 : _v); })
#define TOUCH_PY(ty_) ({ int _v = my0 + (int)((ty_) * (double)mh / 479.0); \
    _v < my0 ? my0 : (_v >= my0 + mh ? my0 + mh - 1 : _v); })

    /* touch -> mouse gestures (capacitive-touch spec values):
       - tap (<500 ms, <12 px wander)      : left click on release
       - move >12 px from anchor          : left drag (button goes down at
         once -> drawing works, zero added latency)
       - hold still >500 ms inside slop   : right click (held until release)
       - 2nd tap within 300 ms            : drag & drop
       - 3rd tap within 300 ms + move     : scroll (no pointer warp) */
    const double TAP_WIN = 0.30, LONG_PRESS = 0.50, SLOP = 6.0;
    if (ic->touch_as_mouse && in->touched && !ctx->prev_touched) {
        /* finger down */
        int px = TOUCH_PX(in->tx);
        int py = TOUCH_PY(in->ty);
        xtest_warp(d, px, py);
        ctx->touch_px = px; ctx->touch_py = py;
        ctx->touch_ax = in->tx; ctx->touch_ay = in->ty;
        double t = now_sec();
        ctx->touch_taps = (t - ctx->touch_last_up < TAP_WIN) ? ctx->touch_taps + 1 : 1;
        ctx->touch_t0 = t;
        ctx->touch_ly = in->ty;
        ctx->touch_gesture = 0;
        if (ctx->touch_taps >= 3) {
            ctx->touch_gesture = 2;              /* scroll mode */
            ctx->touch_taps = 0;
        }
        /* taps == 2 stays undecided: released quickly -> normal click
           (a real double-click); held + moved -> drag & drop; held
           still -> right click. The button only goes down on slop exit. */
    } else if (ic->touch_as_mouse && !in->touched && ctx->prev_touched) {
        /* finger up */
        double held = now_sec() - ctx->touch_t0;
        if (ctx->touch_gesture == 1) {
            xtest_button(d, 1, 0);               /* drop */
        } else if (ctx->touch_gesture == 2) {
            /* scroll mode ends */
        } else if (ctx->touch_gesture == 3) {
            xtest_button(d, 3, 0);               /* release right button */
        } else if (held < LONG_PRESS) {
            xtest_button(d, 1, 1);               /* tap = click */
            xtest_button(d, 1, 0);
        } else {
            xtest_button(d, 3, 1);               /* fallback: long hold = right click */
            xtest_button(d, 3, 0);
        }
        /* multi-tap counting only chains REAL taps: a drag, scroll or
           long-press stroke resets the counter, so drawing several quick
           strokes never degrades into scroll mode */
        if (ctx->touch_gesture != 0 || held >= LONG_PRESS)
            ctx->touch_taps = 0;
        ctx->touch_last_up = now_sec();
        ctx->touch_gesture = 0;
    } else if (ic->touch_as_mouse && in->touched) {
        /* finger moving / held */
        double t = now_sec();
        double ddx = in->tx - ctx->touch_ax, ddy = in->ty - ctx->touch_ay;
        int past_slop = (ddx * ddx + ddy * ddy) > SLOP * SLOP;

        if (ctx->touch_gesture == 2) {
            /* scroll: vertical deltas, pointer stays put */
            int dy = in->ty - ctx->touch_ly;
            while (dy > 28) { xtest_scroll(d, 5); dy -= 28; }
            while (dy < -28) { xtest_scroll(d, 4); dy += 28; }
            ctx->touch_ly = in->ty - dy;
        } else {
            /* every other gesture keeps the pointer under the finger */
            int px = TOUCH_PX(in->tx);
            int py = TOUCH_PY(in->ty);
            if (abs(px - ctx->touch_px) > 2 || abs(py - ctx->touch_py) > 2) {
                xtest_warp(d, px, py);
                ctx->touch_px = px; ctx->touch_py = py;
            }
            if (ctx->touch_gesture == 0) {
                if (past_slop) {
                    /* real movement -> drag begins immediately (drawing!) */
                    ctx->touch_gesture = 1;
                    xtest_button(d, 1, 1);
                } else if (t - ctx->touch_t0 > LONG_PRESS) {
                    ctx->touch_gesture = 3;      /* still inside slop: right click */
                    xtest_button(d, 3, 1);
                }
            }
            /* gesture 1: left already held, warp tracks the finger.
               gesture 3: right held until release. */
        }
    }

    /* WUP_THIN_DESKTOP sticks (kb/mouse mode):
       - left stick  -> relative pointer motion
       - right stick -> wheel scroll (vertical + horizontal), or relative
         pointer when right_stick_mouse is set instead of right_stick_scroll.
       A stick never emits keys here; stick_keys[] is a separate opt-in. */
    double sens = ic->sensitivity / 100.0;
    if (ctx->active_mode == INMODE_KBMOUSE) {
        /* the report's stick Y axis is up-positive while screen Y is
           down-positive: negate once here; invert_y still flips on top.
           Gains are per-frame px/notch units — the sticks report ~±1900. */
        int lx = in->lx, ly = -in->ly;
        if (abs(lx) < dz) lx = 0;
        if (abs(ly) < dz) ly = 0;
        if (ic->invert_y) ly = -ly;
        if (ic->left_stick_mouse) {
            ctx->mx += lx * 0.02 * sens;
            ctx->my += ly * 0.02 * sens;
        }
        int rx = in->rx, ry = -in->ry;
        if (abs(rx) < dz) rx = 0;
        if (abs(ry) < dz) ry = 0;
        if (ic->invert_y) ry = -ry;
        if (ic->right_stick_scroll) {
            /* accumulate fractional motion; one wheel click per ~28 units */
            ctx->sx += rx * 0.01 * sens;
            ctx->sy += ry * 0.01 * sens;
            while (ctx->sy > 28) { xtest_scroll(d, 4); ctx->sy -= 28; }
            while (ctx->sy < -28) { xtest_scroll(d, 5); ctx->sy += 28; }
            while (ctx->sx > 28) { xtest_scroll(d, 7); ctx->sx -= 28; }
            while (ctx->sx < -28) { xtest_scroll(d, 6); ctx->sx += 28; }
        } else if (ic->right_stick_mouse) {
            ctx->mx += rx * 0.02 * sens;
            ctx->my += ry * 0.02 * sens;
        }
    }
    /* gyro -> mouse (both modes): px/frame = dps * px_per_s / 60 */
    if (ic->gyro_mode == GYRO_MOUSE ||
        (ic->gyro_mode == GYRO_STICK && ctx->active_mode == INMODE_KBMOUSE)) {
        double g = ic->gyro_sens / 100.0;
        double dps[3];
        gyro_dps(ctx, in, dps);
        ctx->mx += dps[0] * GYRO_MOUSE_PX_PER_S * g / 60.0;
        ctx->my += dps[1] * GYRO_MOUSE_PX_PER_S * g / 60.0;
    }
    /* flush accumulated motion */
    int dx = (int)ctx->mx, dy = (int)ctx->my;
    ctx->mx -= dx; ctx->my -= dy;
    if (dx || dy) xtest_motion(d, dx, dy);

    ctx->prev_touched = in->touched;
    ctx->prev_tx = in->tx; ctx->prev_ty = in->ty;
}

/* ---------- public ---------- */

input_ctx_t *input_init(const settings_t *s) {
    input_ctx_t *ctx = calloc(1, sizeof *ctx);
    if (!ctx) return NULL;
    ctx->uifd = -1;
    ctx->active_mode = INMODE_KBMOUSE;
    ctx->pad_volume = -1;
    ctx->scr_w = 1920; ctx->scr_h = 1080;
    memcpy(ctx->gyro_bias, s->input.gyro_bias, sizeof ctx->gyro_bias);
    /* X display is needed for touch-as-mouse / gyro-mouse in BOTH modes */
    ctx->dpy = XOpenDisplay(NULL);
    if (ctx->dpy) {
        ctx->scr_w = DisplayWidth(ctx->dpy, DefaultScreen(ctx->dpy));
        ctx->scr_h = DisplayHeight(ctx->dpy, DefaultScreen(ctx->dpy));
    }
    if (s->input.mode == INMODE_GAMEPAD) {
        ctx->uifd = uinput_open(ctx->gamepad_error, sizeof ctx->gamepad_error);
        if (ctx->uifd >= 0) ctx->active_mode = INMODE_GAMEPAD;
        else load_keysyms(&s->input); /* real Desktop fallback */
    } else {
        load_keysyms(&s->input);
    }
    return ctx;
}

void input_set_touch_map(input_ctx_t *ctx, int x, int y, int w, int h) {
    if (!ctx) return;
    ctx->map_x = x; ctx->map_y = y; ctx->map_w = w; ctx->map_h = h;
    ctx->map_valid = (w > 0 && h > 0);
}

/* WUP_FIX_DISCONNECT_RELEASES: do not leave host inputs pressed if the
 * GamePad quits with HOME+B held, loses Wi-Fi or the server is stopped.
 * Releasing the last snapshot is essential: there may be no final "key up".
 */
static void input_release_held(input_ctx_t *ctx) {
    if (ctx->uifd >= 0) {
        /* uinput/evdev: send key-up and centered axes before destroying device. */
        for (int i = 0; i < WBTN_COUNT; i++) {
            if (ctx->prev_buttons & wbtn_bit[i])
                uinput_emit(ctx, EV_KEY, wbtn_evkey[i], 0);
        }
        uinput_emit(ctx, EV_ABS, ABS_X, 0);
        uinput_emit(ctx, EV_ABS, ABS_Y, 0);
        uinput_emit(ctx, EV_ABS, ABS_RX, 0);
        uinput_emit(ctx, EV_ABS, ABS_RY, 0);
        uinput_syn(ctx);
        /* Give consumers a chance to process the neutral report. */
        g_usleep(10000);
    } else if (ctx->dpy) {
        /* Keyboard mapping: undo only chords that were actually pressed —
           buttons bound to commands[]/mouse_buttons[] never pressed a
           chord, so prev_buttons alone would release keys never held. */
        for (int i = 0; i < WBTN_COUNT; i++) {
            if ((ctx->chord_down & wbtn_bit[i]) &&
                btn_chord[i].key != NoSymbol)
                chord_press(ctx, &btn_chord[i], 0);
        }
        ctx->chord_down = 0;
        for (int i = 0; i < 8; i++) {
            if ((ctx->prev_stick_dirs & (1u << i)) &&
                stick_chord[i].key != NoSymbol)
                chord_press(ctx, &stick_chord[i], 0);
        }
        /* WUP_THIN_CMDS: mouse buttons held via mouse_buttons[] are not
           chords — release whatever mb_down still has pressed. */
        for (int mb = 1; mb <= 5; mb++)
            if (ctx->mb_down & (1 << (mb - 1)))
                xtest_button(ctx->dpy, mb, 0);
        ctx->mb_down = 0;
        ctx->sx = ctx->sy = 0;
    }
    /* Touch-as-mouse works in both input modes. */
    if (ctx->dpy && ctx->touch_gesture == 1)
        xtest_button(ctx->dpy, 1, 0);  /* left drag */
    else if (ctx->dpy && ctx->touch_gesture == 3)
        xtest_button(ctx->dpy, 3, 0);  /* right-click hold */
    ctx->prev_buttons = 0;
    ctx->prev_stick_dirs = 0;
    ctx->prev_lx = ctx->prev_ly = ctx->prev_rx = ctx->prev_ry = 0;
    ctx->prev_touched = ctx->touch_gesture = 0;
}

void input_free(input_ctx_t *ctx) {
    if (!ctx) return;
    input_release_held(ctx);
    if (ctx->uifd >= 0) {
        ioctl(ctx->uifd, UI_DEV_DESTROY);
        close(ctx->uifd);
    }
    if (ctx->dpy) XCloseDisplay(ctx->dpy);
    free(ctx);
}

int input_set_mode(input_ctx_t *ctx, const settings_t *s,
                   char *error, size_t error_size) {
    if (!ctx || !s) {
        if (error && error_size) snprintf(error, error_size, "input context unavailable");
        return -1;
    }
    int requested = s->input.mode;
    input_release_held(ctx);
    if (requested == ctx->active_mode) {
        if (error && error_size) error[0] = 0;
        if (requested == INMODE_KBMOUSE) apply_keysyms(&s->input);
        return 0;
    }
    if (ctx->uifd >= 0) {
        ioctl(ctx->uifd, UI_DEV_DESTROY);
        close(ctx->uifd);
        ctx->uifd = -1;
    }
    ctx->active_mode = INMODE_KBMOUSE;
    if (requested == INMODE_GAMEPAD) {
        ctx->uifd = uinput_open(ctx->gamepad_error, sizeof ctx->gamepad_error);
        if (ctx->uifd < 0) {
            apply_keysyms(&s->input);
            if (error && error_size)
                snprintf(error, error_size, "%s", input_gamepad_error(ctx));
            return -1;
        }
        ctx->active_mode = INMODE_GAMEPAD;
    } else {
        apply_keysyms(&s->input);
    }
    if (error && error_size) error[0] = 0;
    return 0;
}

void input_dispatch(input_ctx_t *ctx, const settings_t *s, const wupstream_input_t *in) {
    const input_cfg_t *ic = &s->input;
    /* keymap may have been edited in the UI: reload cheaply each call? no —
       apply_keysyms called by UI on change. cached here. */

    /* WUP_PAD_VOLUME_DEBOUNCE: analog hysteresis and stable-window filtering.
     * First valid report establishes a baseline without changing PC volume. */
    gboolean pc_volume_enabled = s->pad_volume_controls_pc &&
        !s->gamepad_only &&
        !(s->source_type == SRC_REGION &&
          (s->source.w <= 0 || s->source.h <= 0));
    if (!pc_volume_enabled) {
        ctx->pad_volume = -1;
        ctx->vol_candidate = -1;
    } else if (in->volume >= 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double t = now.tv_sec + now.tv_nsec / 1e9;
        int raw = in->volume > 255 ? 255 : in->volume;
        if (ctx->pad_volume < 0) {
            ctx->pad_volume = raw;
            ctx->vol_candidate = -1;
        } else if (abs(raw - ctx->pad_volume) < 5) {
            ctx->vol_candidate = -1;
        } else {
            if (ctx->vol_candidate < 0 || abs(raw - ctx->vol_candidate) > 2) {
                ctx->vol_candidate = raw;
                ctx->vol_candidate_since = t;
            }
            if (t - ctx->vol_candidate_since >= 0.10 &&
                t - ctx->vol_last >= 0.25) {
                int pct = (raw * 150 + 127) / 255;
                char value[16];
                snprintf(value, sizeof value, "%d%%", pct);
                gchar *argv[] = {"pactl", "set-sink-volume", "@DEFAULT_SINK@", value, NULL};
                GError *error = NULL;
                if (g_spawn_async(NULL, argv, NULL,
                        G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                        G_SPAWN_STDERR_TO_DEV_NULL,
                        NULL, NULL, NULL, &error)) {
                    ctx->pad_volume = raw;
                    ctx->vol_candidate = -1;
                } else {
                    g_warning("Cannot launch pactl: %s", error->message);
                    g_clear_error(&error);
                }
                ctx->vol_last = t;
            }
        }
    }

    uint32_t buttons = in->buttons;
    int dz = ic->deadzone;

    /* gyro calibration: average raw gyro while the pad is held still */
    if (ctx->calib_active) {
        for (int i = 0; i < 3; i++) ctx->calib_acc[i] += in->gyro[i];
        if (++ctx->calib_n >= CALIB_SAMPLES) {
            for (int i = 0; i < 3; i++)
                ctx->gyro_bias[i] = ctx->calib_acc[i] / ctx->calib_n;
            ctx->calib_active = 0;
            ctx->calib_done = 1;
        }
    }
    /* corrected gyro (bias removed, deg/s) */
    double gyro[3];
    gyro_dps(ctx, in, gyro);

    if (ctx->active_mode == INMODE_GAMEPAD) {
        int changed = 0;
        /* buttons */
        uint32_t diff = buttons ^ ctx->prev_buttons;
        for (int i = 0; i < WBTN_COUNT; i++) {
            if (!(diff & wbtn_bit[i])) continue;
            uinput_emit(ctx, EV_KEY, wbtn_evkey[i], (buttons & wbtn_bit[i]) ? 1 : 0);
            changed = 1;
        }
        /* sticks */
        int lx = scale_stick(in->lx, dz), ly = scale_stick(in->ly, dz);
        int rx = scale_stick(in->rx, dz), ry = scale_stick(in->ry, dz);
        if (ic->invert_y) { ly = -ly; ry = -ry; }
        /* gyro -> right stick (gamepad mode): proportional to rotation speed */
        if (ic->gyro_mode == GYRO_STICK) {
            double g = ic->gyro_sens / 100.0;
            int gx = (int)(gyro[0] * GYRO_STICK_PER_DPS * g);
            int gy = (int)(gyro[1] * GYRO_STICK_PER_DPS * g);
            rx += gx; ry += gy;
            if (rx > 32767) rx = 32767;
            if (rx < -32767) rx = -32767;
            if (ry > 32767) ry = 32767;
            if (ry < -32767) ry = -32767;
        }
        if (lx != ctx->prev_lx) { uinput_emit(ctx, EV_ABS, ABS_X, lx); changed = 1; }
        if (ly != ctx->prev_ly) { uinput_emit(ctx, EV_ABS, ABS_Y, ly); changed = 1; }
        if (rx != ctx->prev_rx) { uinput_emit(ctx, EV_ABS, ABS_RX, rx); changed = 1; }
        if (ry != ctx->prev_ry) { uinput_emit(ctx, EV_ABS, ABS_RY, ry); changed = 1; }
        if (changed || (buttons ^ ctx->prev_buttons)) uinput_syn(ctx);
        ctx->prev_buttons = buttons;
        ctx->prev_lx = lx; ctx->prev_ly = ly; ctx->prev_rx = rx; ctx->prev_ry = ry;
    } else if (ctx->dpy) {
        /* kb/mouse mode */
        kb_buttons(ctx, s, buttons);
        kb_stick_keys(ctx, s, in);
        ctx->prev_buttons = buttons;
        ctx->prev_lx = scale_stick(in->lx, dz);
        ctx->prev_ly = scale_stick(in->ly, dz);
        ctx->prev_rx = scale_stick(in->rx, dz);
        ctx->prev_ry = scale_stick(in->ry, dz);
    }

    /* pointer features usable in BOTH modes: touch-as-mouse, gyro->mouse.
       kb_pointer itself gates on the relevant flags. */
    if (ctx->dpy && (ic->touch_as_mouse || ic->gyro_mode != GYRO_OFF))
        kb_pointer(ctx, s, in);
}
