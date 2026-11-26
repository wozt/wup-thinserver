#define _DEFAULT_SOURCE
#include "x11util.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/XTest.h>

static Display *dpy_get(void) {
    Display *d = XOpenDisplay(NULL);
    return d;
}

/* ---------- screens ---------- */

int list_screens(screen_t *out, int max) {
    Display *d = dpy_get();
    if (!d) return -1;
    Window root = DefaultRootWindow(d);
    int n = 0;

    if (XRRQueryExtension(d, &(int){0}, &(int){0})) {
        XRRScreenResources *res = XRRGetScreenResourcesCurrent(d, root);
        if (res) {
            RROutput primary = XRRGetOutputPrimary(d, root);
            for (int i = 0; i < res->ncrtc && n < max; i++) {
                XRRCrtcInfo *ci = XRRGetCrtcInfo(d, res, res->crtcs[i]);
                if (!ci || ci->mode == None || ci->width == 0) { if (ci) XRRFreeCrtcInfo(ci); continue; }
                /* find a connected output on this crtc for the name */
                char name[64] = "?";
                for (int j = 0; j < ci->noutput; j++) {
                    XRROutputInfo *oi = XRRGetOutputInfo(d, res, ci->outputs[j]);
                    if (oi && oi->connection != RR_Disconnected) {
                        snprintf(name, sizeof name, "%s", oi->name);
                        XRRFreeOutputInfo(oi);
                        break;
                    }
                    if (oi) XRRFreeOutputInfo(oi);
                }
                screen_t *s = &out[n++];
                memset(s, 0, sizeof *s);
                snprintf(s->name, sizeof s->name, "%s", name);
                s->x = ci->x; s->y = ci->y; s->w = ci->width; s->h = ci->height;
                for (int j = 0; j < ci->noutput; j++)
                    if (ci->outputs[j] == primary) s->primary = 1;
                XRRFreeCrtcInfo(ci);
            }
            XRRFreeScreenResources(res);
            XCloseDisplay(d);
            return n;
        }
    }
    /* fallback: whole root window */
    XWindowAttributes a;
    if (XGetWindowAttributes(d, root, &a) && n < max) {
        screen_t *s = &out[n++];
        memset(s, 0, sizeof *s);
        snprintf(s->name, sizeof s->name, "root");
        s->w = a.width; s->h = a.height;
    }
    XCloseDisplay(d);
    return n;
}

screen_t *find_screen(const char *name) {
    static screen_t buf[16];
    int n = list_screens(buf, 16);
    if (n < 0) return NULL;
    for (int i = 0; i < n; i++)
        if (!strcmp(buf[i].name, name) || (name[0] == '?' && buf[i].name[0] == '?'))
            return &buf[i];
    if (!name[0]) return &buf[0];
    return NULL;
}

/* ---------- windows (EWMH _NET_CLIENT_LIST) ---------- */

static Atom xatom(Display *d, const char *n) { return XInternAtom(d, n, False); }

/* copy at most len bytes, stopping at NUL; property data is not NUL-terminated */
static void prop_text(unsigned char *data, unsigned long len, char *buf, size_t n) {
    size_t z = 0;
    while (z < len && z < n - 1 && data[z]) { buf[z] = (char)data[z]; z++; }
    buf[z] = 0;
}

static void get_text(Display *d, Window w, Atom prop, char *buf, size_t n) {
    buf[0] = 0;
    Atom utf8 = xatom(d, "UTF8_STRING");
    Atom actual; int fmt; unsigned long after, len;
    unsigned char *data = NULL;
    if (XGetWindowProperty(d, w, prop, 0, 1024, False, utf8, &actual, &fmt, &len, &after, &data) == Success && data) {
        if (fmt == 8 && len > 0) prop_text(data, len, buf, n);
        else if (fmt == 32 && len > 0) {
            /* UTF-32 -> crude ASCII */
            char *o = buf;
            for (unsigned long i = 0; i < len && (size_t)(o - buf) < n - 4; i++) {
                long c = ((long *)data)[i];
                *o++ = (c >= 32 && c < 127) ? (char)c : '?';
            }
            *o = 0;
        }
        XFree(data);
    }
    /* fallback to STRING for WM_NAME */
    if (!buf[0] && prop != xatom(d, "_NET_WM_NAME")) {
        Atom str = xatom(d, "STRING");
        if (XGetWindowProperty(d, w, prop, 0, 1024, False, str, &actual, &fmt, &len, &after, &data) == Success && data) {
            if (fmt == 8 && len > 0) prop_text(data, len, buf, n);
            XFree(data);
        }
    }
}

int list_windows(winfo_t *out, int max) {
    Display *d = dpy_get();
    if (!d) return -1;
    Window root = DefaultRootWindow(d);
    Atom list = xatom(d, "_NET_CLIENT_LIST");
    Atom actual; int fmt; unsigned long n, after;
    unsigned long *data = NULL;
    int count = 0;
    if (XGetWindowProperty(d, root, list, 0, 4096, False, XA_WINDOW,
                           &actual, &fmt, &n, &after, (unsigned char **)&data) != Success || !data) {
        XCloseDisplay(d);
        return 0;
    }
    Atom nameprop = xatom(d, "_NET_WM_NAME");
    Atom wmname = xatom(d, "WM_NAME");
    for (unsigned long i = 0; i < n && count < max; i++) {
        Window w = data[i];
        /* skip unmapped */
        XWindowAttributes a;
        if (!XGetWindowAttributes(d, w, &a) || a.map_state != IsViewable) continue;
        winfo_t *wi = &out[count];
        memset(wi, 0, sizeof *wi);
        wi->id = w;
        get_text(d, w, nameprop, wi->title, sizeof wi->title);
        if (!wi->title[0]) get_text(d, w, wmname, wi->title, sizeof wi->title);
        window_class_name(d, w, wi->wm_class, sizeof wi->wm_class);
        wi->x = a.x; wi->y = a.y; wi->w = a.width; wi->h = a.height;
        /* translate to root coords (a.x/y are parent-relative) */
        Window child;
        int rx, ry;
        if (XTranslateCoordinates(d, w, root, 0, 0, &rx, &ry, &child) == True) {
            wi->x = rx; wi->y = ry;
        }
        if (!wi->title[0] && !wi->wm_class[0]) continue;
        count++;
    }
    XFree(data);
    XCloseDisplay(d);
    return count;
}

void window_class_name(Display *d, Window w, char *buf, size_t n) {
    buf[0] = 0;
    Atom cls = xatom(d, "WM_CLASS");
    Atom actual; int fmt; unsigned long len, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(d, w, cls, 0, 1024, False, XA_STRING, &actual, &fmt, &len, &after, &data) == Success && data) {
        /* WM_CLASS is "res\0class\0"; pick the second field, bounded by len */
        const char *first = (const char *)data;
        size_t flen = strnlen(first, len);
        if (flen + 1 < len) {
            const char *second = first + flen + 1;
            size_t slen = strnlen(second, len - flen - 1);
            snprintf(buf, n, "%s", slen ? second : first);
        } else {
            snprintf(buf, n, "%.*s", (int)(flen < n - 1 ? flen : n - 1), first);
        }
        XFree(data);
    }
}

const char *window_title(Display *d, Window w) {
    static char t[256];
    get_text(d, w, xatom(d, "_NET_WM_NAME"), t, sizeof t);
    if (!t[0]) get_text(d, w, xatom(d, "WM_NAME"), t, sizeof t);
    return t;
}

int get_window_geom(Display *d, Window w, int *x, int *y, int *ww, int *hh) {
    XWindowAttributes a;
    if (!XGetWindowAttributes(d, w, &a) || a.map_state == IsUnmapped) return 0;
    Window child;
    if (!XTranslateCoordinates(d, w, DefaultRootWindow(d), 0, 0, x, y, &child)) return 0;
    *ww = a.width; *hh = a.height;
    return 1;
}

/* ---------- XTest input ---------- */

void xtest_key(Display *d, KeySym ks, int press) {
    KeyCode kc = XKeysymToKeycode(d, ks);
    if (!kc) return;
    XTestFakeKeyEvent(d, kc, press ? True : False, CurrentTime);
    XFlush(d);
}

void xtest_button(Display *d, int btn, int press) {
    XTestFakeButtonEvent(d, btn, press ? True : False, CurrentTime);
    XFlush(d);
}

void xtest_motion(Display *d, int dx, int dy) {
    XTestFakeMotionEvent(d, -1, dx, dy, CurrentTime);
    XFlush(d);
}

void xtest_warp(Display *d, int x, int y) {
    XTestFakeMotionEvent(d, -1, x, y, CurrentTime);
    XFlush(d);
}

void xtest_scroll(Display *d, int btn4_or_5) {
    XTestFakeButtonEvent(d, btn4_or_5, True, CurrentTime);
    XTestFakeButtonEvent(d, btn4_or_5, False, CurrentTime);
    XFlush(d);
}

KeySym xkeysym_for_name(const char *name) {
    if (!name || !name[0]) return NoSymbol;
    /* XStringToKeysym handles single chars and XK names ("Left", "F1", "Escape", "minus"...) */
    KeySym ks = XStringToKeysym(name);
    if (ks == NoSymbol) {
        char buf[64];
        snprintf(buf, sizeof buf, "XK_%s", name);
        ks = XStringToKeysym(buf);
    }
    if (ks == NoSymbol && name[0] >= 'a' && name[0] <= 'z' && !name[1]) {
        char up[2] = { (char)toupper((unsigned char)name[0]), 0 };
        ks = XStringToKeysym(up);
    }
    return ks;
}
