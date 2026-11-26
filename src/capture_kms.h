#pragma once
#include <stdint.h>
typedef struct wup_kms wup_kms_t;
wup_kms_t *wup_kms_open(const char *output,int sw,int sh,int sx,int sy,int x,int y,int w,int h,int egl);
int wup_kms_cpu(wup_kms_t *k,const uint8_t **pixels,int *stride);
void wup_kms_release(wup_kms_t *k);
int wup_kms_egl(wup_kms_t *k,unsigned char *out,int x,int y,int w,int h);
int wup_kms_i420_enabled(void);
void wup_kms_close(wup_kms_t *k);
