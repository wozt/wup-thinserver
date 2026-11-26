#pragma once
#include <stdint.h>
#include <pthread.h>
#include <pulse/pulseaudio.h>

/* PC output monitor -> ring buffer (s16 interleaved stereo 48k) -> wupstream_audio.
   Direct libpulse stream (pa_simple) — no subprocess, ~20 ms latency. */
typedef struct {
    int running;
    void *ps;             /* pulse_t priv (threaded mainloop) */
    /* ring: samples (interleaved L,R), capacity in samples */
    int16_t *ring;
    size_t cap, head, tail;
    pthread_mutex_t mtx;
} audio_out_t;

/* wupstream_mic -> ring (mono s16 48k) -> pa_simple playback to default sink */
typedef struct {
    int running;
    void *ps;
    int16_t *ring;
    size_t cap, head, tail;
    pthread_mutex_t mtx;
} audio_in_t;

int audio_out_start(audio_out_t *a, const char *monitor_name); /* "" or "default" = default sink monitor */
void audio_out_stop(audio_out_t *a);
/* fill dst with `frames` stereo frames (frames*2 samples); zeros on underrun */
void audio_out_read(audio_out_t *a, int16_t *dst, int frames);
/* 0..1 fill of the pending buffer, for latency diagnostics */
float audio_out_level(const audio_out_t *a);

int audio_in_start(audio_in_t *a);
void audio_in_stop(audio_in_t *a);
/* push `n` mono samples; drops oldest on overflow */
void audio_in_write(audio_in_t *a, const int16_t *src, int n);
