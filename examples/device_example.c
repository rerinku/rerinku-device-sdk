/* rerinku device example — the integration a camera firmware writes, in C,
 * against <rerinku/rerinku_device.h> only.
 *
 * Instead of a hardware encoder it replays an H.264/H.265 Annex-B file (and
 * optionally raw G.711 A-law audio), paced in real time, whenever the platform
 * asks for the stream:
 *
 *   rerinku_device_example --endpoint http://192.168.1.10:8080 --sn CAM-0001 \
 *       --video clip.h264 [--fps 25] [--audio clip.alaw] [--jpeg thumb.jpg]
 *
 * Make test media with ffmpeg:
 *   ffmpeg -f lavfi -i testsrc2=size=640x360:rate=25 -t 10 -c:v libx264 \
 *       -profile:v baseline -g 50 -bsf:v h264_mp4toannexb -f h264 clip.h264
 *   ffmpeg -f lavfi -i sine=frequency=440:sample_rate=8000 -t 10 \
 *       -f alaw -ar 8000 -ac 1 clip.alaw
 *
 * On a real device the pump below is replaced by the encoder's output loop:
 * every encoded frame goes to rerinku_device_write() with its capture PTS.
 */
#include "rerinku/rerinku_device.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VIDEO_CHANNEL 0u
#define AUDIO_CHANNEL 1u
#define ALAW_FRAME_BYTES 160 /* 20 ms at 8 kHz */

typedef struct {
    uint8_t *data;
    size_t len;
} buffer_t;

typedef struct {
    size_t offset;
    size_t len;
} span_t;

typedef struct {
    const char *endpoint, *sn, *video_path, *audio_path, *jpeg_path;
    int fps, h265;
} options_t;

static options_t g_opt = {NULL, NULL, NULL, NULL, NULL, 25, 0};
static rerinku_device_t *g_dev;
static buffer_t g_video, g_audio;
static span_t *g_frames; /* access units in g_video */
static size_t g_frame_count;
static time_t g_started; /* process start, for uptimeSec */

static volatile sig_atomic_t g_quit;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_streaming; /* guarded by g_mu */

/* ---- media helpers ------------------------------------------------------- */

static int read_file(const char *path, buffer_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out->data = size > 0 ? malloc((size_t)size) : NULL;
    out->len = out->data ? fread(out->data, 1, (size_t)size, f) : 0;
    fclose(f);
    return out->len > 0 ? 0 : -1;
}

/* Position of the next 00 00 01 start code at or after `from`, or len. */
static size_t next_start_code(const uint8_t *d, size_t len, size_t from) {
    for (size_t i = from; i + 3 <= len; ++i)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)
            return i;
    return len;
}

/* Split an Annex-B stream into access units. A new picture starts at a
 * parameter set / AUD / SEI after picture data, or at a slice whose
 * first_mb_in_slice (H.264) / first_slice_segment_in_pic_flag (H.265) marks
 * the first slice of a picture. */
static int split_access_units(const buffer_t *es, int h265) {
    size_t cap = 256, count = 0, au_start = 0;
    int au_has_vcl = 0;
    span_t *aus = malloc(cap * sizeof(span_t));
    if (!aus)
        return -1;
    size_t sc = next_start_code(es->data, es->len, 0);
    while (sc < es->len) {
        size_t nal = sc + 3;
        size_t next = next_start_code(es->data, es->len, nal);
        /* A 4-byte start code's leading zero belongs to the next NAL. */
        size_t nal_end = (next < es->len && next > 0 && es->data[next - 1] == 0) ? next - 1 : next;
        if (nal + 2 < nal_end) {
            const uint8_t *p = es->data + nal;
            int type = h265 ? (p[0] >> 1) & 0x3f : p[0] & 0x1f;
            int vcl = h265 ? type <= 31 : (type >= 1 && type <= 5);
            int first_slice = h265 ? (p[2] & 0x80) != 0 : (p[1] & 0x80) != 0;
            /* VPS/SPS/PPS/AUD/prefix SEI open a picture; H.265 suffix SEI
             * (40) closes one and stays with it. */
            int prefix = h265 ? ((type >= 32 && type <= 35) || type == 39)
                              : (type >= 6 && type <= 9);
            int start_code_pos = (sc > 0 && es->data[sc - 1] == 0) ? (int)(sc - 1) : (int)sc;
            if (au_has_vcl && ((vcl && first_slice) || prefix)) {
                if (count == cap) {
                    span_t *grown = realloc(aus, (cap *= 2) * sizeof(span_t));
                    if (!grown) {
                        free(aus);
                        return -1;
                    }
                    aus = grown;
                }
                aus[count].offset = au_start;
                aus[count].len = (size_t)start_code_pos - au_start;
                count++;
                au_start = (size_t)start_code_pos;
                au_has_vcl = 0;
            }
            if (vcl)
                au_has_vcl = 1;
        }
        sc = next;
    }
    if (au_has_vcl && au_start < es->len) {
        if (count == cap) {
            span_t *grown = realloc(aus, (cap + 1) * sizeof(span_t));
            if (!grown) {
                free(aus);
                return -1;
            }
            aus = grown;
        }
        aus[count].offset = au_start;
        aus[count].len = es->len - au_start;
        count++;
    }
    g_frames = aus;
    g_frame_count = count;
    return count > 0 ? 0 : -1;
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void sleep_until_us(uint64_t t) {
    uint64_t now = now_us();
    if (t <= now)
        return;
    uint64_t d = t - now;
    struct timespec ts = {(time_t)(d / 1000000u), (long)(d % 1000000u) * 1000};
    nanosleep(&ts, NULL);
}

/* ---- the "encoder": replay the files while streaming ---------------------- */

static void *pump(void *arg) {
    (void)arg;
    const uint64_t frame_us = 1000000u / (uint64_t)g_opt.fps;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_streaming && !g_quit)
            pthread_cond_wait(&g_cv, &g_mu);
        pthread_mutex_unlock(&g_mu);
        if (g_quit)
            break;

        /* Replay from the start (the file's first frame is a keyframe) with a
         * monotonic timeline, as an encoder would. */
        uint64_t t0 = now_us();
        size_t v = 0, a = 0;
        for (;;) {
            pthread_mutex_lock(&g_mu);
            int streaming = g_streaming;
            pthread_mutex_unlock(&g_mu);
            if (!streaming || g_quit)
                break;

            uint64_t v_pts = v * frame_us;
            uint64_t a_pts = UINT64_MAX;
            if (g_audio.len >= ALAW_FRAME_BYTES)
                a_pts = (uint64_t)a * 20000u;
            if (a_pts < v_pts) {
                size_t frames = g_audio.len / ALAW_FRAME_BYTES;
                sleep_until_us(t0 + a_pts);
                rerinku_device_write(g_dev, AUDIO_CHANNEL,
                                     g_audio.data + (a % frames) * ALAW_FRAME_BYTES,
                                     ALAW_FRAME_BYTES, a_pts);
                a++;
                continue;
            }
            sleep_until_us(t0 + v_pts);
            const span_t *f = &g_frames[v % g_frame_count];
            rerinku_device_write(g_dev, VIDEO_CHANNEL, g_video.data + f->offset, f->len, v_pts);
            v++;
        }
    }
    return NULL;
}

/* ---- SDK callbacks --------------------------------------------------------- */

static void on_status(void *user, rerinku_device_status_t status) {
    (void)user;
    printf("[example] device %s\n", rerinku_device_status_name(status));
}

static int on_stream_start(void *user) {
    (void)user;
    printf("[example] stream requested: starting the encoder\n");
    pthread_mutex_lock(&g_mu);
    g_streaming = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return 0;
}

static void on_stream_stop(void *user, const char *reason) {
    (void)user;
    printf("[example] stream stopped (%s): idling the encoder\n", reason);
    pthread_mutex_lock(&g_mu);
    g_streaming = 0;
    pthread_mutex_unlock(&g_mu);
}

static void on_keyframe_request(void *user, uint32_t channel_id) {
    (void)user;
    /* A real device calls its encoder here, e.g. RK_MPI_VENC_RequestIDR(). A
     * file can't make a keyframe on demand; the SDK's GOP cache covers it. */
    printf("[example] keyframe requested on channel %u\n", channel_id);
}

static void on_command(void *user, const char *command_id, const char *action,
                       const char *params_json) {
    (void)user;
    printf("[example] command %s params=%s\n", action, params_json);
    if (strcmp(action, "ping") == 0)
        rerinku_device_reply_command(g_dev, command_id, "{\"pong\":true}");
    else
        rerinku_device_fail_command(g_dev, command_id, "unsupported action");
}

static void on_snapshot(void *user, const char *request_id) {
    (void)user;
    buffer_t jpeg = {NULL, 0};
    if (g_opt.jpeg_path)
        read_file(g_opt.jpeg_path, &jpeg);
    /* len 0 tells the platform there is no frame. */
    rerinku_device_reply_snapshot(g_dev, request_id, jpeg.data, jpeg.len);
    free(jpeg.data);
}

/* get_runtime: live state while someone looks at the device. Standard keys
 * (uptimeSec, load1, memTotalKB, memAvailableKB) are rendered by the platform;
 * anything else is listed as-is. Linux /proc here; a camera reads its own. */
static size_t on_runtime_info(void *user, char *buf, size_t cap) {
    (void)user;
    double load1 = -1;
    long mem_total = -1, mem_avail = -1;
    FILE *f = fopen("/proc/loadavg", "r");
    if (f) {
        if (fscanf(f, "%lf", &load1) != 1)
            load1 = -1;
        fclose(f);
    }
    if ((f = fopen("/proc/meminfo", "r")) != NULL) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            sscanf(line, "MemTotal: %ld", &mem_total);
            sscanf(line, "MemAvailable: %ld", &mem_avail);
        }
        fclose(f);
    }
    int n = snprintf(buf, cap, "{\"uptimeSec\":%ld,\"clipFrames\":%zu",
                     (long)(time(NULL) - g_started), g_frame_count);
    if (n > 0 && load1 >= 0 && (size_t)n < cap)
        n += snprintf(buf + n, cap - n, ",\"load1\":%.2f", load1);
    if (n > 0 && mem_total > 0 && mem_avail >= 0 && (size_t)n < cap)
        n += snprintf(buf + n, cap - n, ",\"memTotalKB\":%ld,\"memAvailableKB\":%ld",
                      mem_total, mem_avail);
    if (n > 0 && (size_t)n + 2 <= cap) {
        buf[n++] = '}';
        buf[n] = '\0';
        return (size_t)n;
    }
    return 0;
}

static void on_signal(int sig) {
    (void)sig;
    g_quit = 1;
}

/* ---- main ------------------------------------------------------------------- */

static int usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s --endpoint URL --sn SN --video FILE.h264|.h265 [--fps N]\n"
            "          [--audio FILE.alaw] [--jpeg FILE.jpg] [--debug]\n",
            argv0);
    return 2;
}

int main(int argc, char **argv) {
    int debug = 0;
    /* Line-buffer stdout so our lines interleave correctly with the SDK's
     * (which flushes every line) when the output goes to a file. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_started = time(NULL);
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--endpoint") && v) g_opt.endpoint = argv[++i];
        else if (!strcmp(a, "--sn") && v) g_opt.sn = argv[++i];
        else if (!strcmp(a, "--video") && v) g_opt.video_path = argv[++i];
        else if (!strcmp(a, "--audio") && v) g_opt.audio_path = argv[++i];
        else if (!strcmp(a, "--jpeg") && v) g_opt.jpeg_path = argv[++i];
        else if (!strcmp(a, "--fps") && v) g_opt.fps = atoi(argv[++i]);
        else if (!strcmp(a, "--debug")) debug = 1;
        else return usage(argv[0]);
    }
    if (!g_opt.endpoint || !g_opt.sn || !g_opt.video_path || g_opt.fps <= 0)
        return usage(argv[0]);
    const char *ext = strrchr(g_opt.video_path, '.');
    g_opt.h265 = ext && (!strcmp(ext, ".h265") || !strcmp(ext, ".265") || !strcmp(ext, ".hevc"));

    if (read_file(g_opt.video_path, &g_video) != 0 ||
        split_access_units(&g_video, g_opt.h265) != 0) {
        fprintf(stderr, "no access units in %s\n", g_opt.video_path);
        return 1;
    }
    if (g_opt.audio_path && read_file(g_opt.audio_path, &g_audio) != 0)
        return 1;
    printf("[example] rerinku %s, %zu video frames at %d fps%s\n", rerinku_version(),
           g_frame_count, g_opt.fps, g_audio.len ? " + G.711A audio" : "");

    if (debug)
        rerinku_set_log_level(RERINKU_LOG_LEVEL_DEBUG);

    rerinku_device_config_t cfg;
    rerinku_device_config_init(&cfg);
    cfg.endpoint = g_opt.endpoint;
    cfg.sn = g_opt.sn;
    cfg.name = g_opt.sn;
    cfg.model = "rerinku-device-example";
    cfg.fw_version = rerinku_version();
    /* Static facts beyond model/firmware: whatever the device wants shown. */
    char ext_json[160];
    snprintf(ext_json, sizeof ext_json, "{\"source\":\"file replay\",\"fps\":%d,\"codec\":\"%s\"}",
             g_opt.fps, g_opt.h265 ? "H.265" : "H.264");
    cfg.ext_json = ext_json;

    rerinku_device_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.struct_size = sizeof(cb);
    cb.on_status = on_status;
    cb.on_stream_start = on_stream_start;
    cb.on_stream_stop = on_stream_stop;
    cb.on_keyframe_request = on_keyframe_request;
    cb.on_command = on_command;
    cb.on_snapshot = on_snapshot;
    cb.on_runtime_info = on_runtime_info;

    int rc = rerinku_device_create(&cfg, &cb, &g_dev);
    if (rc != RERINKU_OK) {
        fprintf(stderr, "create failed: %s\n", rerinku_strerror(rc));
        return 1;
    }

    rerinku_device_channel_t video;
    memset(&video, 0, sizeof(video));
    video.struct_size = sizeof(video);
    video.channel_id = VIDEO_CHANNEL;
    video.kind = RERINKU_MEDIA_KIND_VIDEO;
    video.codec = g_opt.h265 ? RERINKU_CODEC_H265 : RERINKU_CODEC_H264;
    video.label = "main";
    video.stream_id = "main";
    video.fps = (uint32_t)g_opt.fps;
    rc = rerinku_device_add_channel(g_dev, &video);

    if (rc == RERINKU_OK && g_audio.len) {
        rerinku_device_channel_t audio;
        memset(&audio, 0, sizeof(audio));
        audio.struct_size = sizeof(audio);
        audio.channel_id = AUDIO_CHANNEL;
        audio.kind = RERINKU_MEDIA_KIND_AUDIO;
        audio.codec = RERINKU_CODEC_G711A;
        audio.label = "audio";
        audio.stream_id = "main";
        audio.sample_rate = 8000;
        audio.audio_channels = 1;
        rc = rerinku_device_add_channel(g_dev, &audio);
    }
    if (rc != RERINKU_OK) {
        fprintf(stderr, "add_channel failed: %s\n", rerinku_strerror(rc));
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    pthread_t pump_thread;
    pthread_create(&pump_thread, NULL, pump, NULL);

    rc = rerinku_device_start(g_dev);
    if (rc != RERINKU_OK) {
        fprintf(stderr, "start failed: %s\n", rerinku_strerror(rc));
        g_quit = 1;
    }
    printf("[example] waiting for the platform to request the stream (Ctrl-C to quit)\n");
    while (!g_quit) {
        struct timespec ts = {0, 200 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }

    printf("[example] shutting down\n");
    rerinku_device_stop(g_dev);
    pthread_mutex_lock(&g_mu);
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    pthread_join(pump_thread, NULL);
    rerinku_device_destroy(g_dev);
    free(g_frames);
    free(g_video.data);
    free(g_audio.data);
    return 0;
}
