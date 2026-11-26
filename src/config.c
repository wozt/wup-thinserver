#include "config.h"
#include <string.h>

const char *config_validate(const settings_t *s) {
    if (!s) return "null configuration";
    if (s->source_type < SRC_SCREEN || s->source_type > SRC_REGION)
        return "source_type must be 0 (screen), 1 (window) or 2 (region)";
    if (s->capture_backend < 0 || s->capture_backend > 3)
        return "capture_backend must be 0 (MIT-SHM), 1 (XCB GetImage), 2 (KMS CPU) or 3 (KMS EGL)";
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
    if (s->input.sensitivity < 1 || s->input.sensitivity > 400)
        return "input.sensitivity must be between 1 and 400";
    if (s->input.gyro_mode < GYRO_OFF || s->input.gyro_mode > GYRO_STICK)
        return "input.gyro_mode must be 0 (off), 1 (mouse) or 2 (stick)";
    if (s->source_type == SRC_REGION &&
        (s->source.w <= 0 || s->source.h <= 0))
        return "region source needs a positive width and height";
    /* A thin client streams one whole desktop: window/region sources are
       legal but uncommon; only warn-free validation here. */
    (void)strlen("");
    return NULL;
}
