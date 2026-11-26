#pragma once
#include <stdint.h>
#define KMS_HELPER_PATH "/usr/local/libexec/wupstreamd/wup-kms-helper"
#define KMS_HELPER_VERSION "1"
#define KMS_WIRE_MAGIC 0x57555001u
struct kms_request { uint32_t magic, crtc; };
struct kms_reply {
    uint32_t magic, count, id, width, height, format, flags, x, y, sw, sh;
    uint32_t pitch[4], offset[4];
    uint64_t modifier;
};
