#pragma once
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

typedef struct {
    char name[64];
    int x, y, w, h;      /* position in root coords + pixel size */
    int primary;
} screen_t;

typedef struct {
    unsigned long id;
    char title[256];
    char wm_class[256];
    int x, y, w, h;
} winfo_t;

/* screens via XRandr */
int list_screens(screen_t *out, int max);
screen_t *find_screen(const char *name);

/* EWMH window list + geometry */
int list_windows(winfo_t *out, int max);           /* top-level client windows */
int get_window_geom(Display *d, Window win, int *rx, int *ry, int *rww, int *rhh);
const char *window_title(Display *d, Window w);
void window_class_name(Display *d, Window w, char *buf, size_t n);

/* XTest input */
void xtest_key(Display *d, KeySym ks, int press);
void xtest_button(Display *d, int btn, int press);   /* 1=left 2=middle 3=right */
void xtest_motion(Display *d, int dx, int dy);
void xtest_warp(Display *d, int x, int y);
void xtest_scroll(Display *d, int btn4_or_5);
KeySym xkeysym_for_name(const char *name);            /* "z", "Left", "F1", ... */
