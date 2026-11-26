#pragma once
#include "settings.h"
#include "wupstream.h"

typedef struct server_t server_t;

/* Once-per-second sampling, no lock or UI callback per frame. "Frame call"
 * includes libwupstream pacing; it is not end-to-end Wi-Fi latency. */
typedef struct {
    double fps, fresh_fps, capture_ms, x11_ms, scale_ms, frame_call_ms;
    double captured_fps; /* WUP_V913: actual worker throughput */
    unsigned int capture_errors, repeated, dropped;
    int valid;
} server_metrics_t;

enum { SRV_IDLE, SRV_WAITING_PAD, SRV_STREAMING, SRV_STOPPED };

server_t *server_new(settings_t *s);
void server_destroy(server_t *srv);

void server_request_stream(server_t *srv);   /* user asked to stream */
void server_request_stop(server_t *srv);     /* user asked to stop */
void server_restart_capture(server_t *srv);  /* settings changed -> rebuild ffmpeg */
void server_request_reconnect(server_t *srv);/* drop pad session, re-init everything */
void server_apply_input_keys(server_t *srv); /* keys edited -> reload keysyms */
void server_calib_gyro(server_t *srv);      /* calibrate gyro bias (~3 s) */

int server_state(server_t *srv);             /* SRV_* */
const char *server_status(server_t *srv);    /* human-readable status */
int server_input_snapshot(server_t *srv, wupstream_input_t *out); /* 1 = fresh pad data */

/* WUP_V98_ALL_IN_ONE */
void server_sync_settings(server_t *srv); /* stage a copy; never read GTK's settings on stream thread */
void server_metrics_snapshot(server_t *srv, server_metrics_t *out);
