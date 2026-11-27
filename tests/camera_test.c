#define _DEFAULT_SOURCE
#include "camera.h"
#include "camera_frame.h"
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FAKE_VIDEO_FD 4242

struct wupstream { int unused; };

static unsigned char encoded[100000];
static int encoded_size;
static atomic_int packet_budget;
static atomic_int malformed_first;
static atomic_int disable_calls;
static atomic_int video_writes;
static atomic_int nonblank_writes;
static atomic_int video_closes;
static char mock_card[32] = "Wii U GamePad Camera";

static int fail(const char *message) {
    fprintf(stderr, "camera-test: %s\n", message);
    return 1;
}

int wupstream_camera(wupstream *pad, uint8_t *h264, int max) {
    (void)pad;
    if (atomic_exchange(&malformed_first, 0)) {
        static const unsigned char bad[] = {0, 0, 1, 0xff, 0x12};
        memcpy(h264, bad, sizeof bad);
        return sizeof bad;
    }
    int budget = atomic_load(&packet_budget);
    while (budget > 0) {
        if (atomic_compare_exchange_weak(&packet_budget, &budget, budget - 1)) {
            if (encoded_size > max) return 0;
            memcpy(h264, encoded, encoded_size);
            return encoded_size;
        }
    }
    return 0;
}

void wupstream_camera_disable(wupstream *pad) {
    (void)pad;
    atomic_fetch_add(&disable_calls, 1);
}

int wupstream_camera_capable(wupstream *pad) {
    (void)pad;
    return 1;
}

int camera_test_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }
    if (!strcmp(path, "/dev/video42")) return FAKE_VIDEO_FD;
    return open(path, flags, mode);
}

int camera_test_ioctl(int fd, unsigned long request, ...) {
    va_list arguments;
    va_start(arguments, request);
    void *data = va_arg(arguments, void *);
    va_end(arguments);
    if (fd != FAKE_VIDEO_FD) return ioctl(fd, request, data);
    if (request == VIDIOC_QUERYCAP) {
        struct v4l2_capability *capability = data;
        memset(capability, 0, sizeof *capability);
        snprintf((char *)capability->card, sizeof capability->card, "%s", mock_card);
        capability->capabilities = V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_READWRITE;
        return 0;
    }
    if (request == VIDIOC_S_FMT) {
        struct v4l2_format *format = data;
        format->fmt.pix.bytesperline = WUP_CAMERA_WIDTH * 2;
        format->fmt.pix.sizeimage = WUP_CAMERA_WIDTH * WUP_CAMERA_HEIGHT * 2;
        return 0;
    }
    if (request == VIDIOC_S_PARM) return 0;
    return -1;
}

ssize_t camera_test_write(int fd, const void *data, size_t size) {
    if (fd != FAKE_VIDEO_FD) return write(fd, data, size);
    atomic_fetch_add(&video_writes, 1);
    const unsigned char *pixels = data;
    if (size == WUP_CAMERA_WIDTH * WUP_CAMERA_HEIGHT * 2 &&
        (pixels[0] != 16 || pixels[1] != 128 ||
         pixels[2] != 16 || pixels[3] != 128))
        atomic_fetch_add(&nonblank_writes, 1);
    return (ssize_t)size;
}

int camera_test_close(int fd) {
    if (fd == FAKE_VIDEO_FD) {
        atomic_fetch_add(&video_closes, 1);
        return 0;
    }
    return close(fd);
}

static int make_h264_frame(void) {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) return -1;
    AVCodecContext *context = avcodec_alloc_context3(codec);
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    if (!context || !frame || !packet) return -1;
    context->width = WUP_CAMERA_WIDTH;
    context->height = WUP_CAMERA_HEIGHT;
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->time_base = (AVRational){1, 30};
    context->framerate = (AVRational){30, 1};
    context->gop_size = 1;
    context->max_b_frames = 0;
    av_opt_set(context->priv_data, "preset", "ultrafast", 0);
    av_opt_set(context->priv_data, "tune", "zerolatency", 0);
    int result = avcodec_open2(context, codec, NULL);
    frame->format = context->pix_fmt;
    frame->width = context->width;
    frame->height = context->height;
    if (result >= 0) result = av_frame_get_buffer(frame, 32);
    if (result >= 0) result = av_frame_make_writable(frame);
    if (result >= 0) {
        for (int y = 0; y < frame->height; y++)
            memset(frame->data[0] + (size_t)y * frame->linesize[0], 76,
                   frame->width);
        for (int y = 0; y < frame->height / 2; y++) {
            memset(frame->data[1] + (size_t)y * frame->linesize[1], 85,
                   frame->width / 2);
            memset(frame->data[2] + (size_t)y * frame->linesize[2], 255,
                   frame->width / 2);
        }
        frame->pts = 0;
        result = avcodec_send_frame(context, frame);
    }
    if (result >= 0) result = avcodec_receive_packet(context, packet);
    if (result >= 0 && packet->size <= (int)sizeof encoded) {
        memcpy(encoded, packet->data, packet->size);
        encoded_size = packet->size;
    } else result = -1;
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&context);
    return result;
}

static int wait_for_frames(camera_t *camera, unsigned long minimum, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 10) {
        camera_status_t status;
        camera_status(camera, &status);
        if (status.frames >= minimum) return 0;
        usleep(10000);
    }
    return -1;
}

static int wait_for_flag(camera_t *camera, int active, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 10) {
        camera_status_t status;
        camera_status(camera, &status);
        if (status.capture_active == active) return 0;
        usleep(10000);
    }
    return -1;
}

int main(void) {
    if (make_h264_frame() != 0 || encoded_size <= 0)
        return fail("could not create the synthetic H.264 frame");
    char runtime[] = "/tmp/wup-camera-test-XXXXXX";
    if (!mkdtemp(runtime) || setenv("XDG_RUNTIME_DIR", runtime, 1) != 0)
        return fail("could not create the private runtime directory");

    struct wupstream pad = {0};
    camera_t *camera = camera_start(&pad, "/dev/video42");
    if (!camera) return fail("worker did not start");
    char preview_path[256];
    snprintf(preview_path, sizeof preview_path, "%s", camera_preview_path(camera));

    atomic_store(&malformed_first, 1);
    atomic_store(&packet_budget, 1);
    camera_set_consumers(camera, 0, 1, "/dev/video42");
    if (wait_for_frames(camera, 1, 2000) != 0)
        return fail("valid H.264 did not recover after a malformed packet");

    int fd = open(preview_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info;
    if (fd < 0 || fstat(fd, &info) != 0 || (info.st_mode & 0777) != 0600)
        return fail("preview transport permissions are not 0600");
    camera_frame_shared_t *shared = mmap(NULL, sizeof *shared, PROT_READ,
                                         MAP_SHARED, fd, 0);
    if (shared == MAP_FAILED || shared->magic != WUP_CAMERA_FRAME_MAGIC ||
        shared->version != WUP_CAMERA_FRAME_VERSION || !shared->valid ||
        (shared->sequence & 1))
        return fail("preview transport did not publish a complete frame");
    size_t center = ((size_t)WUP_CAMERA_HEIGHT / 2 * WUP_CAMERA_WIDTH +
                     WUP_CAMERA_WIDTH / 2) * 4;
    if (shared->bgra[center + 2] < 180 || shared->bgra[center] > 80)
        return fail("decoded preview does not contain the synthetic red frame");
    munmap(shared, sizeof *shared); close(fd);

    atomic_store(&packet_budget, 1);
    camera_set_consumers(camera, 1, 0, "/dev/video42");
    for (int elapsed = 0; elapsed < 2000 && !atomic_load(&nonblank_writes);
         elapsed += 10) usleep(10000);
    if (!atomic_load(&nonblank_writes))
        return fail("decoded frame was not delivered to the V4L2 output");
    camera_status_t status;
    camera_status(camera, &status);
    if (!status.webcam_available || status.frame_available)
        return fail("Announce and Preview did not remain independent");

    camera_set_consumers(camera, 0, 0, "/dev/video42");
    if (wait_for_flag(camera, 0, 1000) != 0 || !atomic_load(&disable_calls) ||
        !atomic_load(&video_closes))
        return fail("camera disable lifecycle did not close the V4L2 output");

    snprintf(mock_card, sizeof mock_card, "Unrelated Webcam");
    atomic_store(&packet_budget, 1);
    camera_set_consumers(camera, 1, 0, "/dev/video42");
    if (wait_for_frames(camera, 1, 2000) != 0)
        return fail("camera did not resume for the device-label test");
    usleep(50000);
    camera_status(camera, &status);
    if (status.webcam_available || !strstr(status.error, "Wii U GamePad Camera"))
        return fail("an unrelated webcam was not rejected by exact label");

    camera_set_consumers(camera, 0, 0, "/dev/video42");
    wait_for_flag(camera, 0, 1000);
    camera_stop(camera);
    if (access(preview_path, F_OK) == 0)
        return fail("preview transport survived worker shutdown");
    char child[320];
    snprintf(child, sizeof child, "%s/wup-thinserver", runtime);
    rmdir(child);
    rmdir(runtime);
    return 0;
}
