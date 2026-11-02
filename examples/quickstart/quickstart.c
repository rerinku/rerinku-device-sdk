/* Rerinku Device SDK — quick start.
 *
 * The smallest complete integration: one H.264 video channel, the platform
 * connection, the stream lifecycle and keyframe requests. A clip stands in for
 * the hardware encoder so the program runs on a PC; on a camera, replace
 * next_frame() with the encoder's output (e.g. RK_MPI_VENC_GetStream).
 *
 *   # a test clip: H.264, no B-frames, an access unit delimiter before each frame
 *   ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=25 -t 10 -c:v libx264 \
 *          -profile:v baseline -g 50 -bf 0 -x264-params aud=1 clip.h264
 *
 *   ./quickstart http://192.168.1.10:8080 QUICKSTART-001 clip.h264
 *
 * The device appears on the platform as soon as it registers; open it there to
 * watch the clip. Ctrl-C stops it. */
#define _POSIX_C_SOURCE 200809L /* clock_gettime, nanosleep under -std=c11 */

#include <rerinku/rerinku_device.h>

#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VIDEO_CHANNEL 0
#define FPS 25

static atomic_bool g_running = true;

static void on_signal(int sig) {
    (void)sig;
    g_running = false;
}

/* ---- callbacks: they run on SDK threads, so keep them short ----------------- */

static void on_status(void *user, rerinku_device_status_t status) {
    (void)user;
    printf("platform: %s\n", rerinku_device_status_name(status));
}

static int on_stream_start(void *user) {
    (void)user;
    printf("someone is watching: the encoder should run\n");
    return 0; /* accept */
}

static void on_stream_stop(void *user, const char *reason) {
    (void)user;
    printf("nobody is watching (%s): the encoder may idle\n", reason);
}

static void on_keyframe_request(void *user, uint32_t channel_id) {
    (void)user;
    /* A real encoder emits an IDR now (RK_MPI_VENC_RequestIDR, ...). Flag it
     * and return: this runs on a network thread. The clip cannot, but the
     * SDK's GOP cache already lets new viewers start at once. */
    printf("keyframe requested on channel %u\n", channel_id);
}

/* ---- stand-in encoder: access units from an Annex-B clip ---------------------- */

typedef struct clip {
    uint8_t *data;
    size_t len, pos;
} clip_t;

/* Offset of the next start code at or after `from` (len if none); *type gets
 * the NAL unit type behind it. */
static size_t next_nal(const clip_t *c, size_t from, int *type) {
    for (size_t i = from; i + 4 < c->len; ++i) {
        if (c->data[i] || c->data[i + 1])
            continue;
        size_t header = c->data[i + 2] == 1 ? i + 3 : (!c->data[i + 2] && c->data[i + 3] == 1) ? i + 4 : 0;
        if (header) {
            *type = c->data[header] & 0x1f;
            return i;
        }
    }
    *type = -1;
    return c->len;
}

/* One access unit: from one access unit delimiter (type 9) to the next. Loops. */
static void next_frame(clip_t *c, const uint8_t **frame, size_t *len) {
    int type;
    if (c->pos >= c->len)
        c->pos = 0;
    size_t start = next_nal(c, c->pos, &type), end = start;
    do
        end = next_nal(c, end + 3, &type);
    while (end < c->len && type != 9);
    *frame = c->data + start;
    *len = end - start;
    c->pos = end;
}

static bool load_clip(const char *path, clip_t *c) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    c->data = size > 0 ? malloc((size_t)size) : NULL;
    c->len = c->data ? fread(c->data, 1, (size_t)size, f) : 0;
    c->pos = 0;
    fclose(f);
    int type;
    for (size_t i = next_nal(c, 0, &type); i < c->len; i = next_nal(c, i + 3, &type))
        if (type == 9)
            return true;
    fprintf(stderr, "%s has no access unit delimiters: encode it with -x264-params aud=1\n", path);
    return false;
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void sleep_until_us(uint64_t deadline) {
    uint64_t now = now_us();
    if (deadline <= now)
        return;
    struct timespec ts = {(time_t)((deadline - now) / 1000000u), (long)((deadline - now) % 1000000u) * 1000};
    nanosleep(&ts, NULL);
}

/* ---- main ---------------------------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s ENDPOINT SN CLIP.h264\n", argv[0]);
        return 2;
    }
    printf("Rerinku Device SDK %s\n", rerinku_version());
    if (rerinku_version_number() != RERINKU_VERSION_NUMBER)
        fprintf(stderr, "warning: built against %s headers\n", RERINKU_VERSION_STRING);

    clip_t clip;
    if (!load_clip(argv[3], &clip)) {
        fprintf(stderr, "cannot use %s\n", argv[3]);
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* 1. Who the device is and where the platform lives. */
    rerinku_device_config_t config;
    rerinku_device_config_init(&config);
    config.endpoint = argv[1];
    config.sn = argv[2];
    config.name = "Quick start camera";
    config.model = "quickstart";

    /* 2. What the platform may ask of it. */
    rerinku_device_callbacks_t callbacks = {
        .struct_size = sizeof(callbacks),
        .on_status = on_status,
        .on_stream_start = on_stream_start,
        .on_stream_stop = on_stream_stop,
        .on_keyframe_request = on_keyframe_request,
    };

    rerinku_device_t *device = NULL;
    rerinku_err_t err = rerinku_device_create(&config, &callbacks, &device);
    if (err != RERINKU_OK) {
        fprintf(stderr, "rerinku_device_create: %s\n", rerinku_strerror(err));
        return 1;
    }

    /* 3. What it can send: one H.264 stream. */
    rerinku_device_channel_t video = {
        .struct_size = sizeof(video),
        .channel_id = VIDEO_CHANNEL,
        .kind = RERINKU_MEDIA_KIND_VIDEO,
        .codec = RERINKU_CODEC_H264,
        .label = "main",
        .stream_id = "main",
        .fps = FPS,
    };
    err = rerinku_device_add_channel(device, &video);
    /* 4. Go online. Returns at once; registration and reconnects run in the
     *    background and are reported through on_status. */
    if (err == RERINKU_OK)
        err = rerinku_device_start(device);
    if (err != RERINKU_OK) {
        fprintf(stderr, "cannot start the device: %s\n", rerinku_strerror(err));
        rerinku_device_destroy(device);
        return 1;
    }

    /* 5. The encoder loop: hand every access unit to the SDK with its capture
     *    time. This never blocks on the network and costs almost nothing while
     *    nobody watches, so feeding continuously is fine. */
    const uint64_t frame_us = 1000000u / FPS;
    uint64_t deadline = now_us();
    while (g_running) {
        const uint8_t *frame;
        size_t len;
        next_frame(&clip, &frame, &len);
        if (rerinku_device_write(device, VIDEO_CHANNEL, frame, len, now_us()) ==
            RERINKU_WRITE_RESULT_INVALID_FRAME)
            fprintf(stderr, "not a valid H.264 access unit (%zu bytes)\n", len);
        deadline += frame_us;
        sleep_until_us(deadline);
    }

    /* 6. Offline and gone. */
    rerinku_device_stop(device);
    rerinku_device_destroy(device);
    free(clip.data);
    return 0;
}
