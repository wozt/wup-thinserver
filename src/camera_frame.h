#pragma once
#include <stddef.h>
#include <stdint.h>

#define WUP_CAMERA_WIDTH 640
#define WUP_CAMERA_HEIGHT 480
#define WUP_CAMERA_STRIDE (WUP_CAMERA_WIDTH * 4)
#define WUP_CAMERA_PIXELS ((size_t)WUP_CAMERA_STRIDE * WUP_CAMERA_HEIGHT)
#define WUP_CAMERA_FRAME_MAGIC 0x57434631u /* WCF1 */
#define WUP_CAMERA_FRAME_VERSION 1u

/* Same-UID mmap transport for the Settings preview. The sequence is odd
 * while the daemon writes and even when a complete latest frame is ready.
 * There is deliberately one slot: readers drop obsolete frames. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t sequence;
    uint32_t valid;
    uint32_t reserved;
    uint64_t monotonic_ns;
    unsigned char bgra[WUP_CAMERA_PIXELS];
} camera_frame_shared_t;

