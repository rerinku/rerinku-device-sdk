/* C API contract checks that need no server: argument validation, channel
 * registration, the write path before a stream, versioned structs, logging,
 * and a start/stop cycle against an unreachable endpoint (start must return at
 * once and stop must join promptly). */
#include "rerinku/rerinku_device.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_log_lines;
static int g_saw_connecting;
static int g_saw_stopped;

static void on_log(void *user, rerinku_log_level_t level, const char *tag, const char *msg) {
    (void)level;
    (void)msg;
    assert(user == (void *)&g_log_lines);
    assert(tag != NULL);
    pthread_mutex_lock(&g_mu);
    g_log_lines++;
    pthread_mutex_unlock(&g_mu);
}

static void on_status(void *user, rerinku_device_status_t status) {
    assert(user == (void *)&g_saw_connecting);
    pthread_mutex_lock(&g_mu);
    if (status == RERINKU_DEVICE_STATUS_CONNECTING) g_saw_connecting = 1;
    if (status == RERINKU_DEVICE_STATUS_STOPPED) g_saw_stopped = 1;
    pthread_mutex_unlock(&g_mu);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static rerinku_device_channel_t channel(uint32_t id, rerinku_media_kind_t kind,
                                        rerinku_codec_t codec) {
    rerinku_device_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.struct_size = sizeof(ch);
    ch.channel_id = id;
    ch.kind = kind;
    ch.codec = codec;
    return ch;
}

int main(void) {
    /* The library linked is the one the headers describe. */
    assert(strcmp(rerinku_version(), RERINKU_VERSION_STRING) == 0);
    assert(rerinku_version_number() == RERINKU_VERSION_NUMBER);
    assert(RERINKU_VERSION_NUMBER == ((RERINKU_VERSION_MAJOR << 16) | (RERINKU_VERSION_MINOR << 8) |
                                      RERINKU_VERSION_PATCH));
    assert(strcmp(rerinku_strerror(RERINKU_ERR_INVALID_STATE), "invalid state") == 0);
    assert(strcmp(rerinku_device_status_name(RERINKU_DEVICE_STATUS_ONLINE), "online") == 0);

    rerinku_device_config_t cfg;
    rerinku_device_config_init(&cfg);
    assert(cfg.struct_size == sizeof(cfg));
    assert(cfg.gop_cache_max_bytes == 1024u * 1024u);
    assert(cfg.nack_history_packets == 512u);
    assert(cfg.stream_reconnect_max_delay_sec == 30u);

    rerinku_device_t *dev = NULL;
    /* endpoint and sn are required */
    assert(rerinku_device_create(&cfg, NULL, &dev) == RERINKU_ERR_INVALID_ARGUMENT);
    cfg.endpoint = "http://127.0.0.1:1"; /* nothing listens: registration keeps failing */
    assert(rerinku_device_create(&cfg, NULL, &dev) == RERINKU_ERR_INVALID_ARGUMENT);
    cfg.sn = "TEST-0001";
    assert(rerinku_device_create(NULL, NULL, &dev) == RERINKU_ERR_INVALID_ARGUMENT);
    assert(rerinku_device_create(&cfg, NULL, NULL) == RERINKU_ERR_INVALID_ARGUMENT);

    /* ext_json must be a JSON object. */
    {
        rerinku_device_t *d = NULL;
        cfg.ext_json = "{\"soc\":";
        assert(rerinku_device_create(&cfg, NULL, &d) == RERINKU_ERR_INVALID_ARGUMENT);
        cfg.ext_json = "[1,2]";
        assert(rerinku_device_create(&cfg, NULL, &d) == RERINKU_ERR_INVALID_ARGUMENT);
        cfg.ext_json = "{\"soc\":\"Rockchip RV1103\",\"cpuCores\":1}";
        assert(rerinku_device_create(&cfg, NULL, &d) == RERINKU_OK);
        rerinku_device_destroy(d);
        cfg.ext_json = NULL;
    }

    /* An older caller's smaller config struct still works: missing fields
     * keep their defaults. */
    {
        rerinku_device_config_t old = cfg;
        old.struct_size = offsetof(rerinku_device_config_t, gop_cache_max_bytes);
        old.gop_cache_max_bytes = 7; /* beyond the old struct: must be ignored */
        rerinku_device_t *d = NULL;
        assert(rerinku_device_create(&old, NULL, &d) == RERINKU_OK);
        rerinku_device_destroy(d);
    }

    rerinku_device_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.struct_size = sizeof(cb);
    cb.user = &g_saw_connecting;
    cb.on_status = on_status;
    assert(rerinku_device_create(&cfg, &cb, &dev) == RERINKU_OK);
    assert(dev != NULL);

    /* No channels yet: start is refused. */
    assert(rerinku_device_start(dev) == RERINKU_ERR_INVALID_STATE);

    rerinku_device_channel_t bad = channel(0, RERINKU_MEDIA_KIND_VIDEO, RERINKU_CODEC_OPUS);
    assert(rerinku_device_add_channel(dev, &bad) == RERINKU_ERR_INVALID_ARGUMENT);
    bad = channel(1, RERINKU_MEDIA_KIND_AUDIO, RERINKU_CODEC_G711A);
    bad.bitstream_format = RERINKU_BITSTREAM_FORMAT_ANNEX_B; /* video-only option */
    assert(rerinku_device_add_channel(dev, &bad) == RERINKU_ERR_INVALID_ARGUMENT);
    bad = channel(0, RERINKU_MEDIA_KIND_VIDEO, RERINKU_CODEC_H264);
    bad.struct_size = 0; /* forgot to set it */
    assert(rerinku_device_add_channel(dev, &bad) == RERINKU_ERR_INVALID_ARGUMENT);

    rerinku_device_channel_t video = channel(0, RERINKU_MEDIA_KIND_VIDEO, RERINKU_CODEC_H264);
    video.label = "main";
    video.stream_id = "main";
    rerinku_device_channel_t audio = channel(1, RERINKU_MEDIA_KIND_AUDIO, RERINKU_CODEC_G711A);
    audio.stream_id = "main";
    assert(rerinku_device_add_channel(dev, &video) == RERINKU_OK);
    assert(rerinku_device_add_channel(dev, &audio) == RERINKU_OK);
    assert(rerinku_device_add_channel(dev, &video) == RERINKU_ERR_INVALID_ARGUMENT);

    /* Writes before a stream exists are cheap no-ops. */
    static const uint8_t idr[] = {0, 0, 0, 1, 0x65, 0x88, 0x84, 0x00};
    static const uint8_t alaw[160] = {0};
    assert(rerinku_device_write(dev, 0, idr, sizeof(idr), 0) == RERINKU_WRITE_RESULT_NOT_STREAMING);
    assert(rerinku_device_write(dev, 1, alaw, sizeof(alaw), 20000) == RERINKU_WRITE_RESULT_NOT_STREAMING);
    assert(rerinku_device_write(dev, 9, idr, sizeof(idr), 0) == RERINKU_WRITE_RESULT_UNKNOWN_CHANNEL);
    assert(rerinku_device_write(dev, 0, NULL, 0, 0) == RERINKU_WRITE_RESULT_INVALID_FRAME);

    assert(rerinku_device_reply_command(dev, "", "{}") == RERINKU_ERR_INVALID_ARGUMENT);
    assert(rerinku_device_send_event(dev, NULL, "{}") == RERINKU_ERR_INVALID_ARGUMENT);
    /* An event cannot go anywhere before start(): say so, do not pretend. */
    assert(rerinku_device_send_event(dev, "motion", "{\"zone\":1}") == RERINKU_ERR_INVALID_STATE);

    /* Recording upload: arguments checked, and nothing to upload with before
     * the device has registered (no token yet). */
    assert(rerinku_device_upload_recording(dev, NULL, "main", 1) == RERINKU_ERR_INVALID_ARGUMENT);
    assert(rerinku_device_upload_recording(dev, "/nonexistent.mp4", "main", 0) ==
           RERINKU_ERR_INVALID_ARGUMENT);
    assert(rerinku_device_upload_recording(dev, "/nonexistent.mp4", "main", 1792693526000ull) ==
           RERINKU_ERR_INVALID_STATE);
    assert(strcmp(rerinku_strerror(RERINKU_ERR_REJECTED), "rejected by the platform") == 0);

    /* start() returns at once even though the endpoint is unreachable; the
     * registration retries are logged; stop() joins promptly. */
    rerinku_set_log_callback(on_log, &g_log_lines);
    uint64_t t0 = now_ms();
    assert(rerinku_device_start(dev) == RERINKU_OK);
    assert(now_ms() - t0 < 500);
    assert(rerinku_device_start(dev) == RERINKU_ERR_INVALID_STATE);
    assert(rerinku_device_add_channel(dev, &bad) == RERINKU_ERR_INVALID_ARGUMENT);
    rerinku_device_channel_t late = channel(5, RERINKU_MEDIA_KIND_VIDEO, RERINKU_CODEC_H265);
    assert(rerinku_device_add_channel(dev, &late) == RERINKU_ERR_INVALID_STATE);
    /* Started but offline (the endpoint is unreachable): the event is not
     * sent, and the caller learns that it was not. */
    assert(rerinku_device_send_event(dev, "motion", "{\"zone\":1}") == RERINKU_ERR_NETWORK);

    struct timespec wait = {0, 300 * 1000 * 1000};
    nanosleep(&wait, NULL);
    t0 = now_ms();
    rerinku_device_stop(dev);
    assert(now_ms() - t0 < 2000);
    rerinku_set_log_callback(NULL, NULL);

    pthread_mutex_lock(&g_mu);
    assert(g_saw_connecting);
    assert(g_saw_stopped);
    assert(g_log_lines > 0);
    pthread_mutex_unlock(&g_mu);

    rerinku_device_destroy(dev);
    rerinku_device_destroy(NULL);
    printf("device api ok\n");
    return 0;
}
