#include "config.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <string.h>

const char *config_validate(const settings_t *s) {
    if (!s) return "null configuration";
    if (s->source_type < SRC_SCREEN || s->source_type > SRC_REGION)
        return "source_type must be 0 (screen), 1 (window) or 2 (region)";
    if (s->capture_backend < 0 || s->capture_backend > 3)
        return "capture_backend must be 0 (MIT-SHM), 1 (XCB GetImage), 2 (KMS CPU) or 3 (KMS EGL)";
    if (s->cursor_visible != 0 && s->cursor_visible != 1)
        return "cursor_visible must be 0 or 1";
    if (s->crf < 0 || s->crf > 51)
        return "crf must be between 0 and 51";
    if (s->kbps < 0 || s->kbps > 50000)
        return "kbps must be between 0 and 50000";
    if (s->aspect < ASPECT_STRETCH || s->aspect > ASPECT_CROP)
        return "aspect must be 0 (stretch), 1 (letterbox) or 2 (crop)";
    if (s->input.mode != INMODE_GAMEPAD && s->input.mode != INMODE_KBMOUSE)
        return "input.mode must be 0 (gamepad) or 1 (kb/mouse)";
    if (s->input.deadzone < 0 || s->input.deadzone > 500)
        return "input.deadzone must be between 0 and 500";
    if (s->input.sensitivity < 0 || s->input.sensitivity > 50)
        return "input.sensitivity must be between 0 and 50";
    if (s->input.gyro_mode < GYRO_OFF || s->input.gyro_mode > GYRO_STICK)
        return "input.gyro_mode must be 0 (off), 1 (mouse) or 2 (stick)";
    if (s->input.gyro_sens < 1 || s->input.gyro_sens > 400)
        return "input.gyro_sens must be between 1 and 400";
    if (s->pad_brightness != -1 && (s->pad_brightness < 1 || s->pad_brightness > 6))
        return "pad_brightness must be -1 (automatic) or 1..6";
    if ((s->camera_announce != 0 && s->camera_announce != 1) ||
        (s->camera_preview != 0 && s->camera_preview != 1))
        return "camera_announce and camera_preview must be 0 or 1";
    if (strncmp(s->camera_device, "/dev/video", 10) ||
        !s->camera_device[10])
        return "camera_device must be a numbered /dev/video device";
    for (const char *p = s->camera_device + 10; *p; p++)
        if (!isdigit((unsigned char)*p))
            return "camera_device must be a numbered /dev/video device";
    if (s->listen_port < 1 || s->listen_port > 65535)
        return "listen_port must be between 1 and 65535";
    if (s->listen_address[0]) {
        struct in_addr a;
        if (inet_pton(AF_INET, s->listen_address, &a) != 1)
            return "listen_address must be a numeric IPv4 address or empty";
    }
    if (s->source_type == SRC_REGION &&
        (s->source.w <= 0 || s->source.h <= 0))
        return "region source needs a positive width and height";
    /* A thin client streams one whole desktop: window/region sources are
       legal but uncommon; only warn-free validation here. */
    (void)strlen("");
    return NULL;
}
