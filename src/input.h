#pragma once
#include "settings.h"
#include "wupstream.h"
#include <X11/Xlib.h>
#include <stdint.h>

/* Button order shared with settings.input.keys[] */
enum {
    WBTN_A, WBTN_B, WBTN_X, WBTN_Y,
    WBTN_LEFT, WBTN_RIGHT, WBTN_UP, WBTN_DOWN,
    WBTN_ZL, WBTN_ZR, WBTN_L, WBTN_R,
    WBTN_PLUS, WBTN_MINUS, WBTN_HOME,
    WBTN_L3, WBTN_R3, WBTN_TV,
    WBTN_COUNT
};

extern const char *wbtn_names[WBTN_COUNT];

/* Map a wupstream_input_t report into PC inputs.
   Call from the stream thread once per wupstream_input(). */
typedef struct input_ctx input_ctx_t;

input_ctx_t *input_init(const settings_t *s);
void input_dispatch(input_ctx_t *ctx, const settings_t *s, const wupstream_input_t *in);
void input_free(input_ctx_t *ctx);
void input_reload_keys(input_ctx_t *ctx, const settings_t *s); /* call after editing keys[] */
int input_gamepad_ok(input_ctx_t *ctx);  /* 1 if the virtual gamepad was created */
void input_calib_start(input_ctx_t *ctx);            /* ~3 s of stillness -> bias */
int input_calib_poll(input_ctx_t *ctx, double out[3]); /* 1 = fresh bias in out[] */
/* rect (root coords) the pad touch maps onto; w<=0 -> whole screen */
void input_set_touch_map(input_ctx_t *ctx, int x, int y, int w, int h);
