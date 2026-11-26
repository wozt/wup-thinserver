#pragma once
#include "settings.h"
#include "capture_kms.h"
#include <X11/Xlib.h>
#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <libswscale/swscale.h>

/* Direct X11 capture (MIT-SHM) -> libswscale -> BGRA 854x480.
   No ffmpeg: the freshest frame is grabbed right before wupstream_frame(). */
typedef struct {
    Display *dpy;
    Drawable drawable;          /* root window or client window */
    int is_window;
    unsigned long win_id;
    int cap_x, cap_y;           /* capture origin (root coords for screen/region) */
    int src_w, src_h;           /* captured region size */
    /* image buffer (MIT-SHM via XCB) */
    xcb_connection_t *xcb;
    xcb_shm_seg_t shmseg;
    void *shmaddr;
    int shmid;
    size_t shm_size;
    int use_shm;
    int capture_backend;
    wup_kms_t *kms;
    /* scaler */
    struct SwsContext *sws;
    unsigned char *tmp;         /* intermediate sws output */
    int tmp_w, tmp_h;
    int out_x, out_y, out_w, out_h; /* placement inside 854x480 */
    int aspect;
    double last_x11_ms, last_scale_ms; /* V9.12 diagnostic timings */
    /* window follow */
    int last_w, last_h, last_x, last_y;
} capture_t;

int capture_start(capture_t *c, const settings_t *s);
/* grab one frame, scale+letterbox/crop/stretch into out (854*480*4 BGRA). 0 ok, -1 error */
int capture_grab(capture_t *c, unsigned char *out);
/* window sources: check geometry; restart internals if resized. <0 = window gone */
int capture_follow(capture_t *c, const settings_t *s);
/* root-window rect of the streamed area (touch->mouse mapping) */
void capture_target_rect(const capture_t *c, int *x, int *y, int *w, int *h);
void capture_stop(capture_t *c);
