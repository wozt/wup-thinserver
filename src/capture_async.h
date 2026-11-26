#pragma once
/* WUP_V912_CAPTURE_PIPELINE: at most one published frame, no FIFO. */
#include "capture.h"

typedef struct capture_async capture_async_t;
typedef struct {
    double total_ms, x11_ms, scale_ms;
    unsigned int frames, errors, dropped; /* WUP_V913: stale pending frames superseded */
} capture_async_sample_t;

/* Ownership: caller owns capture_t, worker is its exclusive user until free(). */
capture_async_t *capture_async_start(capture_t *capture);
void capture_async_free(capture_async_t *ctx);
int capture_async_failed(capture_async_t *ctx);
/* A returned frame is owned by the caller until its next capture_async_latest()
 * call; never keep this pointer after capture_async_free(). */
const unsigned char *capture_async_latest(capture_async_t *ctx, int *fresh,
                                          int *x, int *y, int *w, int *h);
/* Reset counters on read; only main streaming thread calls this function. */
void capture_async_sample(capture_async_t *ctx, capture_async_sample_t *out);
