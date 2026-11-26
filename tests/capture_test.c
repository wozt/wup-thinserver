/* Automated capture test: grab a solid Xvfb root through both X11 backends,
 * then verify that a custom X cursor is composited at the scaled hotspot. */
#include "capture.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FW 854
#define FH 480

static int test_backend(int backend) {
    settings_t s;
    settings_defaults(&s);
    s.source_type = SRC_SCREEN;
    s.source.output[0] = 0;          /* first screen = the Xvfb root */
    s.aspect = ASPECT_STRETCH;
    s.capture_backend = backend;
    s.cursor_visible = 0;

    capture_t cap;
    if (capture_start(&cap, &s) != 0) {
        fprintf(stderr, "capture_start failed for backend %d\n", backend);
        return 2;
    }

    Window root = DefaultRootWindow(cap.dpy);
    static const char bits[8] = {
        (char)0xff, (char)0xff, (char)0xff, (char)0xff,
        (char)0xff, (char)0xff, (char)0xff, (char)0xff
    };
    Pixmap shape = XCreateBitmapFromData(cap.dpy, root, bits, 8, 8);
    XColor white = { .red = 65535, .green = 65535, .blue = 65535 };
    XColor black = {0};
    Cursor cursor = XCreatePixmapCursor(cap.dpy, shape, shape, &white, &black, 2, 3);
    XDefineCursor(cap.dpy, root, cursor);
    XWarpPointer(cap.dpy, None, root, 0, 0, 0, 0, 640, 360);
    XSync(cap.dpy, False);

    static unsigned char plain[FW * FH * 4], cursor_frame[FW * FH * 4];
    memset(plain, 0, sizeof plain);
    if (capture_grab(&cap, plain) != 0) {
        fprintf(stderr, "plain capture failed for backend %d\n", backend);
        capture_stop(&cap);
        return 3;
    }
    cap.cursor_visible = 1;
    memset(cursor_frame, 0, sizeof cursor_frame);
    if (capture_grab(&cap, cursor_frame) != 0) {
        fprintf(stderr, "cursor capture failed for backend %d\n", backend);
        capture_stop(&cap);
        return 4;
    }

    XUndefineCursor(cap.dpy, root);
    XFreeCursor(cap.dpy, cursor);
    XFreePixmap(cap.dpy, shape);
    capture_stop(&cap);

    int changed = 0, min_x = FW, min_y = FH, max_x = -1, max_y = -1;
    for (int y = 0; y < FH; y++) {
        for (int x = 0; x < FW; x++) {
            size_t off = ((size_t)y * FW + x) * 4;
            if (memcmp(plain + off, cursor_frame + off, 3)) {
                changed++;
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    }
    printf("backend %d cursor: %d pixels, bbox=%d,%d..%d,%d\n",
           backend, changed, min_x, min_y, max_x, max_y);
    /* Pointer 640,360 with hotspot 2,3: cursor starts at 638,357 before
     * 1280x720 -> 854x480 scaling. Allow one pixel for integer rounding. */
    int expected_x = 638 * FW / 1280;
    int expected_y = 357 * FH / 720;
    if (!changed || min_x < expected_x - 1 || min_x > expected_x + 1 ||
        min_y < expected_y - 1 || min_y > expected_y + 1 ||
        max_x - min_x < 3 || max_x - min_x > 7 ||
        max_y - min_y < 3 || max_y - min_y > 7) {
        fprintf(stderr, "cursor missing or incorrectly scaled for backend %d\n", backend);
        return 5;
    }
    return 0;
}

int main(void) {
    int ret = test_backend(0);
    if (ret) return ret;
    return test_backend(1);
}
