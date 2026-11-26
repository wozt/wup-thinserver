/* Automated capture test: grab one frame from $DISPLAY, scale to
 * 854x480 BGRA and verify it is non-empty. Run under a private Xvfb with a
 * solid-colour root window (see tests/run_capture_test.sh). */
#include "capture.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FW 854
#define FH 480

int main(void) {
    settings_t s;
    settings_defaults(&s);
    s.source_type = SRC_SCREEN;
    s.source.output[0] = 0;          /* first screen = the Xvfb root */
    s.aspect = ASPECT_STRETCH;
    s.capture_backend = 0;

    capture_t cap;
    if (capture_start(&cap, &s) != 0) {
        fprintf(stderr, "capture_start failed\n");
        return 2;
    }
    static unsigned char frame[FW * FH * 4];
    memset(frame, 0, sizeof frame);
    if (capture_grab(&cap, frame) != 0) {
        fprintf(stderr, "capture_grab failed\n");
        capture_stop(&cap);
        return 3;
    }
    capture_stop(&cap);

    /* A solid root must produce non-zero pixels somewhere. */
    unsigned long sum = 0;
    int nz = 0;
    for (size_t i = 0; i < sizeof frame; i += 4) {
        sum += frame[i] + frame[i + 1] + frame[i + 2];
        if (frame[i] || frame[i + 1] || frame[i + 2]) nz++;
    }
    printf("frame: %d/%d pixels non-zero, sum=%lu\n",
           nz, FW * FH, sum);
    if (!nz) {
        fprintf(stderr, "captured frame is entirely black\n");
        return 4;
    }
    return 0;
}
