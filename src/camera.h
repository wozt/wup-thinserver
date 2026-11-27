#pragma once
#include "wupstream.h"
#include <stddef.h>

typedef struct camera_t camera_t;

typedef struct {
    int configured_announce;
    int configured_preview;
    int capture_active;
    int client_capable;       /* 1 explicit/inferred, 0 not yet known */
    int frame_available;
    int webcam_available;
    unsigned long frames;
    char status[160];
    char error[160];
} camera_status_t;

camera_t *camera_start(wupstream *pad, const char *device);
void camera_set_consumers(camera_t *camera, int announce, int preview,
                          const char *device);
void camera_status(camera_t *camera, camera_status_t *status);
const char *camera_preview_path(camera_t *camera);
void camera_stop(camera_t *camera);

