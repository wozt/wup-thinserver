#define _POSIX_C_SOURCE 200809L
/* WUP_V912_CAPTURE_PIPELINE: capture/resize on a dedicated producer thread.
 * Three 854x480 buffers: one may belong to the encoder, one is being
 * written, and at most one is published. The published frame is replaced,
 * not queued, keeping only the most recent available image. */
#include "capture_async.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FRAME_BYTES ((size_t)854 * 480 * 4)
/* WUP_V913_FEEDBACK_CAPTURE: let the video consumer clock the producer.
 * Legacy independent 59.94 Hz pacing: WUP_CAPTURE_DEMAND=0. */
#define TARGET_INTERVAL (1.0 / 59.94)

struct capture_async {
    capture_t *capture;    /* exclusively accessed by the worker */
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t demand_cv;
    int demand_mode;  /* 1: capture after consumer accepts the preceding frame */
    int requested;    /* coalesced one-bit request, never a frame queue */
    atomic_int running;
    atomic_int failed;
    unsigned char *buffers[3];
    int writing;           /* producer-private index */
    int pending;           /* latest completed image, -1 if none */
    int borrowed;          /* reader-owned image, -1 until first success */
    int x, y, w, h;
    capture_async_sample_t sample;
};

static double clock_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_seconds(double seconds) {
    if (seconds <= 0) return;
    struct timespec t;
    t.tv_sec = (time_t)seconds;
    t.tv_nsec = (long)((seconds - t.tv_sec) * 1e9);
    if (t.tv_nsec < 0) t.tv_nsec = 0;
    nanosleep(&t, NULL);
}

static void *producer(void *arg) {
    capture_async_t *ctx = arg;
    unsigned int count = 0;
    double target = clock_seconds();
    while (atomic_load_explicit(&ctx->running, memory_order_relaxed)) {
        if (ctx->demand_mode) {
            pthread_mutex_lock(&ctx->mutex);
            while (!ctx->requested &&
                   atomic_load_explicit(&ctx->running, memory_order_relaxed))
                pthread_cond_wait(&ctx->demand_cv, &ctx->mutex);
            if (!atomic_load_explicit(&ctx->running, memory_order_relaxed)) {
                pthread_mutex_unlock(&ctx->mutex);
                break;
            }
            ctx->requested = 0;
            pthread_mutex_unlock(&ctx->mutex);
        }
        /* The worker alone owns Xlib/XCB, including window resize handling. */
        if (ctx->capture->is_window && (count++ % 30) == 0 &&
            capture_follow(ctx->capture, NULL) < 0) {
            pthread_mutex_lock(&ctx->mutex);
            ctx->sample.errors++;
            pthread_mutex_unlock(&ctx->mutex);
            atomic_store(&ctx->failed, 1);
            break;
        }
        int slot = ctx->writing;
        double begin = clock_seconds();
        if (capture_grab(ctx->capture, ctx->buffers[slot]) != 0) {
            pthread_mutex_lock(&ctx->mutex);
            ctx->sample.errors++;
            pthread_mutex_unlock(&ctx->mutex);
            atomic_store(&ctx->failed, 1);
            break;
        }
        double elapsed_ms = (clock_seconds() - begin) * 1000.0;
        int mx, my, mw, mh;
        capture_target_rect(ctx->capture, &mx, &my, &mw, &mh);
        pthread_mutex_lock(&ctx->mutex);
        ctx->sample.frames++;
        ctx->sample.total_ms += elapsed_ms;
        ctx->sample.x11_ms += ctx->capture->last_x11_ms;
        ctx->sample.scale_ms += ctx->capture->last_scale_ms;
        ctx->x = mx; ctx->y = my; ctx->w = mw; ctx->h = mh;
        /* Drop any unconsumed older image (no frame backlog). */
        if (ctx->pending >= 0) ctx->sample.dropped++;
        ctx->pending = slot;
        int next = -1;
        for (int i = 0; i < 3; i++) {
            if (i != ctx->pending && i != ctx->borrowed) { next = i; break; }
        }
        /* Three buffers guarantee one free slot. */
        ctx->writing = next;
        pthread_mutex_unlock(&ctx->mutex);
        if (next < 0) { atomic_store(&ctx->failed, 1); break; }

        if (!ctx->demand_mode) {
            /* Legacy free-running mode, retained for A/B measurements. */
            target += TARGET_INTERVAL;
            double now = clock_seconds();
            if (target < now - TARGET_INTERVAL) target = now;
            sleep_seconds(target - now);
        }
    }
    return NULL;
}

capture_async_t *capture_async_start(capture_t *capture) {
    if (!capture) return NULL;
    capture_async_t *ctx = calloc(1, sizeof *ctx);
    if (!ctx) return NULL;
    ctx->capture = capture;
    const char *mode = getenv("WUP_CAPTURE_DEMAND");
    ctx->demand_mode = !mode || strcmp(mode, "0") != 0;
    ctx->requested = 1; /* prime first frame before the first read */
    ctx->borrowed = ctx->pending = -1;
    for (int i = 0; i < 3; i++) {
        ctx->buffers[i] = malloc(FRAME_BYTES);
        if (!ctx->buffers[i]) {
            for (int j = 0; j < i; j++) free(ctx->buffers[j]);
            free(ctx);
            return NULL;
        }
    }
    if (pthread_mutex_init(&ctx->mutex, NULL) != 0) {
        for (int i = 0; i < 3; i++) free(ctx->buffers[i]);
        free(ctx);
        return NULL;
    }
    if (pthread_cond_init(&ctx->demand_cv, NULL) != 0) {
        pthread_mutex_destroy(&ctx->mutex);
        for (int i = 0; i < 3; i++) free(ctx->buffers[i]);
        free(ctx);
        return NULL;
    }
    capture_target_rect(capture, &ctx->x, &ctx->y, &ctx->w, &ctx->h);
    atomic_init(&ctx->running, 1);
    atomic_init(&ctx->failed, 0);
    if (pthread_create(&ctx->thread, NULL, producer, ctx) != 0) {
        pthread_cond_destroy(&ctx->demand_cv);
        pthread_mutex_destroy(&ctx->mutex);
        for (int i = 0; i < 3; i++) free(ctx->buffers[i]);
        free(ctx);
        return NULL;
    }
    return ctx;
}

void capture_async_free(capture_async_t *ctx) {
    if (!ctx) return;
    pthread_mutex_lock(&ctx->mutex);
    atomic_store(&ctx->running, 0);
    pthread_cond_broadcast(&ctx->demand_cv);
    pthread_mutex_unlock(&ctx->mutex);
    pthread_join(ctx->thread, NULL);
    pthread_cond_destroy(&ctx->demand_cv);
    pthread_mutex_destroy(&ctx->mutex);
    for (int i = 0; i < 3; i++) free(ctx->buffers[i]);
    free(ctx);
}

int capture_async_failed(capture_async_t *ctx) {
    return ctx && atomic_load(&ctx->failed);
}

const unsigned char *capture_async_latest(capture_async_t *ctx, int *fresh,
                                          int *x, int *y, int *w, int *h) {
    if (!ctx) return NULL;
    pthread_mutex_lock(&ctx->mutex);
    if (fresh) *fresh = ctx->pending >= 0;
    if (ctx->pending >= 0) {
        ctx->borrowed = ctx->pending;
        ctx->pending = -1;
    }
    if (x) *x = ctx->x;
    if (y) *y = ctx->y;
    if (w) *w = ctx->w;
    if (h) *h = ctx->h;
    const unsigned char *ptr = ctx->borrowed >= 0
        ? ctx->buffers[ctx->borrowed] : NULL;
    /* A frame has been acquired for transmission. Begin the next
     * capture NOW, overlapping the consumer's paced frame call.
     * Request is a boolean: repeated polls never build a backlog. */
    if (ptr && ctx->demand_mode) {
        ctx->requested = 1;
        pthread_cond_signal(&ctx->demand_cv);
    }
    pthread_mutex_unlock(&ctx->mutex);
    return ptr;
}

void capture_async_sample(capture_async_t *ctx, capture_async_sample_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!ctx) return;
    pthread_mutex_lock(&ctx->mutex);
    *out = ctx->sample;
    memset(&ctx->sample, 0, sizeof ctx->sample);
    pthread_mutex_unlock(&ctx->mutex);
}
