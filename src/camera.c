#define _DEFAULT_SOURCE
#include "camera.h"
#include "camera_frame.h"
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef WUP_CAMERA_TEST
int camera_test_open(const char *path, int flags, ...);
int camera_test_ioctl(int fd, unsigned long request, ...);
ssize_t camera_test_write(int fd, const void *data, size_t size);
int camera_test_close(int fd);
#define open camera_test_open
#define ioctl camera_test_ioctl
#define write camera_test_write
#define close camera_test_close
#endif

#define CAMERA_PACKET_MAX 100000
#define CAMERA_PREVIEW_NS 66666666ull /* 15 fps: useful preview without GTK churn */

struct camera_t {
    wupstream *pad;
    pthread_t thread;
    pthread_mutex_t mutex;
    atomic_int running;
    int thread_started;
    int announce;
    int preview;
    char device[128];
    camera_status_t state;

    AVCodecContext *decoder;
    AVFrame *frame;
    AVPacket *packet;
    struct SwsContext *rgb;
    struct SwsContext *yuv;
    unsigned char encoded[CAMERA_PACKET_MAX];
    unsigned char bgra[WUP_CAMERA_PIXELS];
    unsigned char yuyv[WUP_CAMERA_WIDTH * WUP_CAMERA_HEIGHT * 2];

    int output_fd;
    time_t output_retry;
    int shared_fd;
    camera_frame_shared_t *shared;
    char shared_path[256];
    uint64_t last_preview_ns;
};

static uint64_t mono_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static void state_error(camera_t *camera, const char *message) {
    pthread_mutex_lock(&camera->mutex);
    snprintf(camera->state.error, sizeof camera->state.error, "%s", message);
    snprintf(camera->state.status, sizeof camera->state.status, "%s", message);
    pthread_mutex_unlock(&camera->mutex);
}

static void publish_invalid(camera_t *camera) {
    if (!camera->shared) return;
    __atomic_add_fetch(&camera->shared->sequence, 1, __ATOMIC_ACQ_REL);
    camera->shared->valid = 0;
    camera->shared->monotonic_ns = mono_ns();
    __atomic_add_fetch(&camera->shared->sequence, 1, __ATOMIC_RELEASE);
    pthread_mutex_lock(&camera->mutex);
    camera->state.frame_available = 0;
    pthread_mutex_unlock(&camera->mutex);
}

static void publish_frame(camera_t *camera) {
    if (!camera->shared) return;
    uint64_t now = mono_ns();
    if (now - camera->last_preview_ns < CAMERA_PREVIEW_NS) return;
    camera->last_preview_ns = now;
    __atomic_add_fetch(&camera->shared->sequence, 1, __ATOMIC_ACQ_REL);
    memcpy(camera->shared->bgra, camera->bgra, sizeof camera->bgra);
    camera->shared->monotonic_ns = now;
    camera->shared->valid = 1;
    __atomic_add_fetch(&camera->shared->sequence, 1, __ATOMIC_RELEASE);
    pthread_mutex_lock(&camera->mutex);
    camera->state.frame_available = 1;
    pthread_mutex_unlock(&camera->mutex);
}

static int shared_open(camera_t *camera) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char fallback[160];
    char directory[220];
    if (!runtime || !runtime[0]) {
        snprintf(fallback, sizeof fallback, "/tmp/wup-thinserver-%u",
                 (unsigned)getuid());
        if (mkdir(fallback, 0700) != 0 && errno != EEXIST) return -1;
        chmod(fallback, 0700);
        runtime = fallback;
    }
    snprintf(directory, sizeof directory, "%s/wup-thinserver", runtime);
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) return -1;
    chmod(directory, 0700);
    snprintf(camera->shared_path, sizeof camera->shared_path, "%s/camera.frame", directory);
    camera->shared_fd = open(camera->shared_path,
        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (camera->shared_fd < 0 ||
        fchmod(camera->shared_fd, 0600) != 0 ||
        ftruncate(camera->shared_fd, (off_t)sizeof(camera_frame_shared_t)) != 0)
        return -1;
    camera->shared = mmap(NULL, sizeof(camera_frame_shared_t),
        PROT_READ | PROT_WRITE, MAP_SHARED, camera->shared_fd, 0);
    if (camera->shared == MAP_FAILED) { camera->shared = NULL; return -1; }
    memset(camera->shared, 0, sizeof *camera->shared);
    camera->shared->magic = WUP_CAMERA_FRAME_MAGIC;
    camera->shared->version = WUP_CAMERA_FRAME_VERSION;
    camera->shared->width = WUP_CAMERA_WIDTH;
    camera->shared->height = WUP_CAMERA_HEIGHT;
    camera->shared->stride = WUP_CAMERA_STRIDE;
    return 0;
}

static void output_blank(camera_t *camera) {
    if (camera->output_fd < 0) return;
    for (size_t i = 0; i < sizeof camera->yuyv; i += 4) {
        camera->yuyv[i] = camera->yuyv[i + 2] = 16;
        camera->yuyv[i + 1] = camera->yuyv[i + 3] = 128;
    }
    (void)write(camera->output_fd, camera->yuyv, sizeof camera->yuyv);
}

static void output_close(camera_t *camera, int blank) {
    if (camera->output_fd >= 0) {
        if (blank) output_blank(camera);
        close(camera->output_fd);
    }
    camera->output_fd = -1;
    pthread_mutex_lock(&camera->mutex);
    camera->state.webcam_available = 0;
    pthread_mutex_unlock(&camera->mutex);
}

static void output_open(camera_t *camera, const char *path) {
    if (camera->output_fd >= 0 || time(NULL) < camera->output_retry) return;
    int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    struct v4l2_capability capability = {0};
    struct v4l2_format format = {0};
    if (fd < 0) goto fail;
    if (ioctl(fd, VIDIOC_QUERYCAP, &capability) < 0 ||
        strncmp((const char *)capability.card, "Wii U GamePad Camera",
                sizeof capability.card)) goto fail;
    unsigned caps = capability.capabilities & V4L2_CAP_DEVICE_CAPS ?
        capability.device_caps : capability.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_OUTPUT) || !(caps & V4L2_CAP_READWRITE)) goto fail;
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    format.fmt.pix.width = WUP_CAMERA_WIDTH;
    format.fmt.pix.height = WUP_CAMERA_HEIGHT;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    format.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(fd, VIDIOC_S_FMT, &format) < 0 ||
        format.fmt.pix.width != WUP_CAMERA_WIDTH ||
        format.fmt.pix.height != WUP_CAMERA_HEIGHT ||
        format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV ||
        format.fmt.pix.bytesperline != WUP_CAMERA_WIDTH * 2 ||
        format.fmt.pix.sizeimage != WUP_CAMERA_WIDTH * WUP_CAMERA_HEIGHT * 2)
        goto fail;
    struct v4l2_streamparm parameters = {0};
    parameters.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    parameters.parm.output.timeperframe.numerator = 1;
    parameters.parm.output.timeperframe.denominator = 30;
    (void)ioctl(fd, VIDIOC_S_PARM, &parameters);
    camera->output_fd = fd;
    pthread_mutex_lock(&camera->mutex);
    camera->state.webcam_available = 1;
    camera->state.error[0] = 0;
    pthread_mutex_unlock(&camera->mutex);
    return;
fail:
    if (fd >= 0) close(fd);
    camera->output_retry = time(NULL) + 5;
    char error[160];
    snprintf(error, sizeof error,
             "Cannot announce %.64s; create a v4l2loopback device named Wii U GamePad Camera",
             path);
    state_error(camera, error);
}

static int decode_frame(camera_t *camera, int bytes, int announce, int preview,
                        const char *device) {
    camera->packet->data = camera->encoded;
    camera->packet->size = bytes;
    if (avcodec_send_packet(camera->decoder, camera->packet) < 0) return -1;
    int fresh = 0;
    while (avcodec_receive_frame(camera->decoder, camera->frame) == 0) {
        AVFrame *frame = camera->frame;
        if (frame->width != WUP_CAMERA_WIDTH || frame->height != WUP_CAMERA_HEIGHT)
            continue;
        if (preview) {
            camera->rgb = sws_getCachedContext(camera->rgb,
                WUP_CAMERA_WIDTH, WUP_CAMERA_HEIGHT, frame->format,
                WUP_CAMERA_WIDTH, WUP_CAMERA_HEIGHT, AV_PIX_FMT_BGRA,
                SWS_FAST_BILINEAR, NULL, NULL, NULL);
            uint8_t *pixels[] = {camera->bgra, NULL, NULL, NULL};
            int strides[] = {WUP_CAMERA_STRIDE, 0, 0, 0};
            if (camera->rgb && sws_scale(camera->rgb,
                    (const uint8_t *const *)frame->data, frame->linesize, 0,
                    WUP_CAMERA_HEIGHT, pixels, strides) == WUP_CAMERA_HEIGHT)
                publish_frame(camera);
        }
        if (announce) {
            output_open(camera, device);
            camera->yuv = sws_getCachedContext(camera->yuv,
                WUP_CAMERA_WIDTH, WUP_CAMERA_HEIGHT, frame->format,
                WUP_CAMERA_WIDTH, WUP_CAMERA_HEIGHT, AV_PIX_FMT_YUYV422,
                SWS_FAST_BILINEAR, NULL, NULL, NULL);
            uint8_t *pixels[] = {camera->yuyv, NULL, NULL, NULL};
            int strides[] = {WUP_CAMERA_WIDTH * 2, 0, 0, 0};
            if (camera->output_fd >= 0 && camera->yuv &&
                sws_scale(camera->yuv, (const uint8_t *const *)frame->data,
                    frame->linesize, 0, WUP_CAMERA_HEIGHT, pixels, strides) ==
                    WUP_CAMERA_HEIGHT) {
                ssize_t written = write(camera->output_fd, camera->yuyv,
                                        sizeof camera->yuyv);
                if (written != (ssize_t)sizeof camera->yuyv &&
                    !(written < 0 && (errno == EAGAIN || errno == EINTR)))
                    output_close(camera, 0);
            }
        } else {
            output_close(camera, 1);
        }
        fresh = 1;
        pthread_mutex_lock(&camera->mutex);
        camera->state.frames++;
        camera->state.frame_available = preview ? camera->state.frame_available : 0;
        camera->state.client_capable = 1;
        if (!announce || camera->output_fd >= 0) {
            snprintf(camera->state.status, sizeof camera->state.status,
                     "Camera active%s%s",
                     announce ? "; virtual webcam enabled" : "",
                     preview ? "; preview enabled" : "");
            camera->state.error[0] = 0;
        }
        pthread_mutex_unlock(&camera->mutex);
    }
    return fresh;
}

static void *camera_thread(void *argument) {
    camera_t *camera = argument;
    int was_active = 0;
    uint64_t request_since = 0;
    while (atomic_load_explicit(&camera->running, memory_order_relaxed)) {
        pthread_mutex_lock(&camera->mutex);
        int announce = camera->announce;
        int preview = camera->preview;
        char device[128];
        snprintf(device, sizeof device, "%s", camera->device);
        camera->state.configured_announce = announce;
        camera->state.configured_preview = preview;
        pthread_mutex_unlock(&camera->mutex);
        int active = announce || preview;
        if (!active) {
            if (was_active) {
                wupstream_camera_disable(camera->pad);
                avcodec_flush_buffers(camera->decoder);
                output_close(camera, 1);
                publish_invalid(camera);
                pthread_mutex_lock(&camera->mutex);
                camera->state.capture_active = 0;
                snprintf(camera->state.status, sizeof camera->state.status,
                         "Camera disabled");
                camera->state.error[0] = 0;
                pthread_mutex_unlock(&camera->mutex);
            }
            was_active = 0; request_since = 0;
            usleep(20000);
            continue;
        }
        if (!was_active) {
            request_since = mono_ns();
            pthread_mutex_lock(&camera->mutex);
            camera->state.frames = 0;
            camera->state.frame_available = 0;
            camera->state.capture_active = 1;
            snprintf(camera->state.status, sizeof camera->state.status,
                     "Waiting for a camera frame");
            pthread_mutex_unlock(&camera->mutex);
        }
        was_active = 1;
        int explicit_capability = wupstream_camera_capable(camera->pad);
        if (explicit_capability > 0) {
            pthread_mutex_lock(&camera->mutex);
            camera->state.client_capable = 1;
            pthread_mutex_unlock(&camera->mutex);
        }
        int bytes = wupstream_camera(camera->pad, camera->encoded,
                                     sizeof camera->encoded);
        if (bytes > 0) {
            decode_frame(camera, bytes, announce, preview, device);
        } else {
            if (mono_ns() - request_since > 3000000000ull) {
                pthread_mutex_lock(&camera->mutex);
                if (!camera->state.frames)
                    snprintf(camera->state.status, sizeof camera->state.status,
                             "Waiting for camera frames; this client may not support camera uplink");
                pthread_mutex_unlock(&camera->mutex);
            }
            usleep(5000);
        }
    }
    if (was_active) wupstream_camera_disable(camera->pad);
    output_close(camera, 1);
    publish_invalid(camera);
    return NULL;
}

camera_t *camera_start(wupstream *pad, const char *device) {
    camera_t *camera = calloc(1, sizeof *camera);
    if (!camera) return NULL;
    camera->pad = pad;
    camera->output_fd = camera->shared_fd = -1;
    pthread_mutex_init(&camera->mutex, NULL);
    atomic_init(&camera->running, 0);
    snprintf(camera->device, sizeof camera->device, "%s",
             device && device[0] ? device : "/dev/video42");
    snprintf(camera->state.status, sizeof camera->state.status, "Camera disabled");
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec) camera->decoder = avcodec_alloc_context3(codec);
    camera->frame = av_frame_alloc();
    camera->packet = av_packet_alloc();
    if (camera->decoder) {
        camera->decoder->thread_count = 1;
        camera->decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
    }
    if (!camera->decoder || !camera->frame || !camera->packet ||
        avcodec_open2(camera->decoder, codec, NULL) < 0 ||
        shared_open(camera) != 0) {
        camera_stop(camera);
        return NULL;
    }
    atomic_store_explicit(&camera->running, 1, memory_order_relaxed);
    if (pthread_create(&camera->thread, NULL, camera_thread, camera) != 0) {
        atomic_store_explicit(&camera->running, 0, memory_order_relaxed);
        camera_stop(camera);
        return NULL;
    }
    camera->thread_started = 1;
    return camera;
}

void camera_set_consumers(camera_t *camera, int announce, int preview,
                          const char *device) {
    if (!camera) return;
    pthread_mutex_lock(&camera->mutex);
    camera->announce = announce != 0;
    camera->preview = preview != 0;
    snprintf(camera->device, sizeof camera->device, "%s",
             device && device[0] ? device : "/dev/video42");
    pthread_mutex_unlock(&camera->mutex);
}

void camera_status(camera_t *camera, camera_status_t *status) {
    memset(status, 0, sizeof *status);
    if (!camera) { snprintf(status->status, sizeof status->status, "Camera inactive"); return; }
    pthread_mutex_lock(&camera->mutex);
    *status = camera->state;
    pthread_mutex_unlock(&camera->mutex);
}

const char *camera_preview_path(camera_t *camera) {
    return camera ? camera->shared_path : "";
}

void camera_stop(camera_t *camera) {
    if (!camera) return;
    if (camera->thread_started) {
        atomic_store_explicit(&camera->running, 0, memory_order_relaxed);
        pthread_join(camera->thread, NULL);
    }
    output_close(camera, 0);
    if (camera->shared) munmap(camera->shared, sizeof *camera->shared);
    if (camera->shared_fd >= 0) close(camera->shared_fd);
    if (camera->shared_path[0]) unlink(camera->shared_path);
    sws_freeContext(camera->rgb);
    sws_freeContext(camera->yuv);
    av_packet_free(&camera->packet);
    av_frame_free(&camera->frame);
    avcodec_free_context(&camera->decoder);
    pthread_mutex_destroy(&camera->mutex);
    free(camera);
}
