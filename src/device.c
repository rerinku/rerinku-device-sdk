/* The device agent behind <rerinku/rerinku_device.h>.
 *
 * Threads (each with one job):
 *   agent   registers with the endpoint (retrying with backoff), starts MQTT,
 *           then keeps time: device-token refresh and media-session rebuilds.
 *   command executes jobs: platform commands, session rebuilds, snapshot
 *           uploads. All blocking work (WHIP, ICE gathering, the application's
 *           command handler, HTTP uploads) happens here, never on the MQTT
 *           loop: a stalled keep-alive makes the broker fire our Last Will.
 *   mqtt    (mqtt.c) the control connection.
 *   session (peer.c) one per media session.
 * Encoder threads call rerinku_device_write(): it only takes the channel's
 * lock and a reference to the live session, so a session rebuild in progress
 * drops frames instead of stalling the encoder. */
#include "rerinku/rerinku_device.h"

#include "bitstream.h"
#include "common.h"
#include "http.h"
#include "log.h"
#include "mqtt.h"
#include "net.h"
#include "peer.h"
#include "whip.h"

#include <cJSON.h>

#include <stdatomic.h>
#include <ctype.h>
#include <stdio.h>


#define GOP_MAX_AGE_MS 5000
#define GOP_MAX_FRAMES 180
/* Device tokens expire (Rerinku View: 24 h). Renew after three quarters of the
 * lifetime the platform granted, timed on the monotonic clock: the device's
 * wall clock may be anything (1970 until NTP runs). Tokens of unknown
 * lifetime are renewed hourly. A refused token (HTTP 401) is renewed at once
 * whatever the schedule says. */
#define TOKEN_CHECK_INTERVAL_MS (5 * 60 * 1000)
#define TOKEN_UNKNOWN_RENEW_MS (60 * 60 * 1000)
#define TOKEN_MIN_RENEW_MS (60 * 1000)
/* PLI bursts (viewers joining, a lossy link) must not make the encoder emit
 * back-to-back IDRs; one per channel per window is plenty. */
#define KEYFRAME_MIN_INTERVAL_MS 500
#define MAX_PENDING_SNAPSHOTS 4
#define REPLIED_RING 64
#define MAX_BOOTSTRAP_DELAY_SEC 30
#define GATHER_TIMEOUT_MS 10000

/* ---- types ------------------------------------------------------------------ */

typedef struct gop_frame {
    uint8_t *data;
    size_t len;
    uint64_t pts;
    bool keyframe;
} gop_frame;

typedef struct channel {
    uint32_t id;
    rerinku_media_kind_t kind;
    rerinku_codec_t codec;
    rerinku_bitstream_format_t format;
    char *label, *stream_id, *codec_parameters;
    uint32_t width, height, fps, bitrate_bps, sample_rate, audio_channels, sample_bits;
    uint32_t clock_rate;
    bool video;

    pthread_mutex_t mu; /* the write path of this channel */
    rk_vnorm norm;
    gop_frame *gop;
    size_t gop_count, gop_cap, gop_bytes;
    uint64_t gop_keyframe_ms;
    uint64_t last_keyframe_request_ms; /* under the device's keyframe_mu */
    /* Whether the platform wants this channel's stream right now (push_sfu
     * "streams"). Frames of an unwanted channel are not sent, the session
     * keeps its m-line. Audio channels are always wanted while streaming. */
    atomic_bool wanted;
} channel;

typedef enum job_type { JOB_COMMAND, JOB_REBUILD, JOB_UPLOAD } job_type;

typedef struct job {
    struct job *next;
    job_type type;
    char *text;      /* command JSON / snapshot request id */
    uint8_t *data;   /* snapshot JPEG */
    size_t len;
    uint64_t gen;    /* rebuild */
    int attempt;
} job;

/* Passed to a session's callbacks; outlives nothing: freed after the session
 * is closed (no callbacks after rk_peer_close returns). */
typedef struct session_ctx {
    rerinku_device_t *dev;
    uint64_t gen;
} session_ctx;

struct rerinku_device {
    /* Configuration (owned copies). */
    char *endpoint, *sn, *secret, *name, *device_id, *model, *fw_version, *ca_file;
    cJSON *ext; /* static facts of the device's choosing, sent with the status */
    bool tls_insecure;
    size_t gop_max_bytes;
    size_t nack_history;
    int reconnect_max_delay_sec;
    int reconnect_max_attempts;
    rerinku_device_callbacks_t cb;

    channel *channels; /* fixed once started */
    size_t channel_count;

    pthread_mutex_t life_mu; /* start/stop/add_channel */
    bool started;
    volatile bool running;

    /* Agent thread timers. */
    pthread_t agent_thread;
    pthread_mutex_t timer_mu;
    pthread_cond_t timer_cv;
    bool retry_pending;
    uint64_t retry_at, retry_gen;
    int retry_attempt;
    bool renew_requested; /* the MQTT broker refused the token */

    /* Command worker. */
    pthread_t cmd_thread;
    pthread_mutex_t job_mu;
    pthread_cond_t job_cv;
    job *job_head, *job_tail;

    /* Pipeline (mu): registration profile, intent, the session's bookkeeping.
     * Held across a whole session start (seconds). */
    pthread_mutex_t mu;
    rk_profile profile;
    uint64_t token_renew_at; /* monotonic ms */
    uint64_t token_issued_at; /* monotonic ms */
    bool streaming;
    /* rev of the last push_sfu applied to the live session (0 = none):
     * stream-set updates are declarative and an older one is ignored. */
    int64_t streams_rev;
    uint64_t gen; /* bumped on every session build/teardown */
    char *whip_resource;
    session_ctx *sess_ctx;
    atomic_int retry_attempt_now;

    /* The live session for the write path (sess_mu: pointer swap only). */
    pthread_mutex_t sess_mu;
    rk_peer *peer;

    pthread_mutex_t mqtt_mu;
    rk_mqtt *mqtt;

    pthread_mutex_t reply_mu;
    char *replied[REPLIED_RING];
    size_t replied_next;
    char *pending_snapshots[MAX_PENDING_SNAPSHOTS];

    pthread_mutex_t keyframe_mu;
};

/* ---- small helpers ------------------------------------------------------------ */

static uint32_t clock_rate_for(rerinku_media_kind_t kind, rerinku_codec_t codec) {
    if (kind == RERINKU_MEDIA_KIND_VIDEO)
        return 90000;
    return codec == RERINKU_CODEC_OPUS ? 48000 : 8000;
}

/* Microseconds to codec ticks without overflow even for epoch-based PTS. */
static uint64_t us_to_clock(uint64_t us, uint32_t rate) {
    return (us / 1000000u) * rate + (us % 1000000u) * rate / 1000000u;
}

static bool valid_kind_codec(rerinku_media_kind_t kind, rerinku_codec_t codec) {
    if (kind == RERINKU_MEDIA_KIND_VIDEO)
        return codec == RERINKU_CODEC_H264 || codec == RERINKU_CODEC_H265;
    if (kind == RERINKU_MEDIA_KIND_AUDIO)
        return codec == RERINKU_CODEC_OPUS || codec == RERINKU_CODEC_G711U ||
               codec == RERINKU_CODEC_G711A;
    return false;
}

static const char *codec_name(rerinku_codec_t c) {
    switch (c) {
    case RERINKU_CODEC_H265:  return "h265";
    case RERINKU_CODEC_OPUS:  return "opus";
    case RERINKU_CODEC_G711U: return "g711u";
    case RERINKU_CODEC_G711A: return "g711a";
    default:                  return "h264";
    }
}

/* Copy a caller's versioned struct over defaults: an older (smaller) struct
 * keeps the defaults for fields it lacks; newer fields are ignored. */
static bool copy_versioned(void *dst, size_t dst_size, const void *src) {
    if (!src)
        return false;
    size_t src_size = *(const size_t *)src;
    if (src_size == 0)
        return false;
    memcpy(dst, src, RK_MIN(src_size, dst_size));
    *(size_t *)dst = dst_size;
    return true;
}

static channel *find_channel(rerinku_device_t *d, uint32_t id) {
    for (size_t i = 0; i < d->channel_count; ++i)
        if (d->channels[i].id == id)
            return &d->channels[i];
    return NULL;
}

static bool is_running(rerinku_device_t *d) { return d->running; }

/* ---- status and acks ------------------------------------------------------------ */

static void set_status(rerinku_device_t *d, rerinku_device_status_t s) {
    RLOG_INFO("agent", "device %s", rerinku_device_status_name(s));
    if (d->cb.on_status)
        d->cb.on_status(d->cb.user, s);
}

/* Publish under mqtt_mu so stop() cannot destroy the connection mid-call. */
typedef enum { PUB_STATUS, PUB_ACK, PUB_DATA } pub_kind;

static bool mqtt_publish(rerinku_device_t *d, pub_kind kind, const char *json) {
    bool sent = false;
    pthread_mutex_lock(&d->mqtt_mu);
    if (d->mqtt) {
        if (kind == PUB_STATUS)
            rk_mqtt_set_status(d->mqtt, json);
        else if (kind == PUB_ACK)
            rk_mqtt_publish_ack(d->mqtt, json);
        else
            sent = rk_mqtt_publish_data(d->mqtt, json);
    }
    pthread_mutex_unlock(&d->mqtt_mu);
    return sent;
}

static cJSON *channels_json(rerinku_device_t *d) {
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; arr && i < d->channel_count; ++i) {
        const channel *c = &d->channels[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "channelId", c->id);
        cJSON_AddStringToObject(item, "kind", c->video ? "video" : "audio");
        cJSON_AddStringToObject(item, "codec", codec_name(c->codec));
        cJSON_AddStringToObject(item, "streamId", c->stream_id);
        if (c->label && *c->label)
            cJSON_AddStringToObject(item, "label", c->label);
        /* Declared parameters; 0 = unknown, omitted. */
        cJSON *params = cJSON_CreateObject();
        if (c->video) {
            if (c->width) cJSON_AddNumberToObject(params, "width", c->width);
            if (c->height) cJSON_AddNumberToObject(params, "height", c->height);
            if (c->fps) cJSON_AddNumberToObject(params, "fps", c->fps);
        } else {
            if (c->sample_rate) cJSON_AddNumberToObject(params, "sampleRate", c->sample_rate);
            if (c->audio_channels) cJSON_AddNumberToObject(params, "channels", c->audio_channels);
            if (c->sample_bits) cJSON_AddNumberToObject(params, "sampleBits", c->sample_bits);
        }
        if (c->bitrate_bps)
            cJSON_AddNumberToObject(params, "bitrateBps", c->bitrate_bps);
        if (params && params->child)
            cJSON_AddItemToObject(item, c->video ? "video" : "audio", params);
        else
            cJSON_Delete(params);
        cJSON_AddItemToArray(arr, item);
    }
    return arr;
}

/* The status the platform shows and stores. Caller holds mu. */
static cJSON *status_json_locked(rerinku_device_t *d) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "online", 1);
    cJSON_AddStringToObject(j, "mode", d->streaming ? "sfu" : "standby");
    cJSON_AddBoolToObject(j, "streaming", d->streaming);
    cJSON_AddStringToObject(j, "name", d->name && *d->name ? d->name : d->sn);
    cJSON_AddStringToObject(j, "deviceId", d->device_id ? d->device_id : "");
    cJSON_AddStringToObject(j, "model", d->model ? d->model : "");
    cJSON_AddStringToObject(j, "fw", d->fw_version ? d->fw_version : "");
    if (d->ext)
        cJSON_AddItemToObject(j, "ext", cJSON_Duplicate(d->ext, 1));
    cJSON_AddItemToObject(j, "channels", channels_json(d));
    if (d->streaming) {
        /* The streams being sent (video channels the platform wants). */
        cJSON *active = cJSON_CreateArray();
        for (size_t i = 0; active && i < d->channel_count; ++i) {
            const channel *c = &d->channels[i];
            if (c->video && atomic_load(&c->wanted))
                cJSON_AddItemToArray(active, cJSON_CreateString(c->stream_id));
        }
        cJSON_AddItemToObject(j, "streams", active);
    }
    return j;
}

/* Caller holds mu. */
static void report_status_locked(rerinku_device_t *d) {
    cJSON *j = status_json_locked(d);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (s)
        mqtt_publish(d, PUB_STATUS, s);
    cJSON_free(s);
}

/* Embed text as JSON when it parses, else as a string. */
static void add_json_or_string(cJSON *obj, const char *key, const char *text) {
    cJSON *parsed = cJSON_Parse(text);
    if (parsed)
        cJSON_AddItemToObject(obj, key, parsed);
    else
        cJSON_AddStringToObject(obj, key, text);
}

static void ack(rerinku_device_t *d, const char *command_id, const char *status, const char *err,
                const char *result) {
    if (!command_id || !*command_id)
        return;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "commandId", command_id);
    cJSON_AddStringToObject(j, "status", status);
    if (err && *err)
        cJSON_AddStringToObject(j, "err", err);
    if (result && *result)
        add_json_or_string(j, "result", result);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (s)
        mqtt_publish(d, PUB_ACK, s);
    cJSON_free(s);
}

/* A command gets exactly one terminal answer, whoever sends it. */
static bool first_reply(rerinku_device_t *d, const char *command_id) {
    pthread_mutex_lock(&d->reply_mu);
    for (size_t i = 0; i < REPLIED_RING; ++i)
        if (d->replied[i] && !strcmp(d->replied[i], command_id)) {
            pthread_mutex_unlock(&d->reply_mu);
            return false;
        }
    free(d->replied[d->replied_next]);
    d->replied[d->replied_next] = rk_strdup(command_id);
    d->replied_next = (d->replied_next + 1) % REPLIED_RING;
    pthread_mutex_unlock(&d->reply_mu);
    return true;
}

/* ---- jobs ---------------------------------------------------------------------- */

static void job_free(job *j) {
    if (!j)
        return;
    free(j->text);
    free(j->data);
    free(j);
}

static void enqueue(rerinku_device_t *d, job *j) {
    pthread_mutex_lock(&d->job_mu);
    if (!d->running) {
        pthread_mutex_unlock(&d->job_mu);
        job_free(j);
        return;
    }
    j->next = NULL;
    if (d->job_tail)
        d->job_tail->next = j;
    else
        d->job_head = j;
    d->job_tail = j;
    pthread_cond_signal(&d->job_cv);
    pthread_mutex_unlock(&d->job_mu);
}

/* ---- keyframes ------------------------------------------------------------------- */

static void request_keyframe(rerinku_device_t *d, uint32_t channel_id) {
    if (!d->cb.on_keyframe_request)
        return;
    channel *c = find_channel(d, channel_id);
    if (!c)
        return;
    uint64_t now = rk_now_ms();
    pthread_mutex_lock(&d->keyframe_mu);
    bool throttled = c->last_keyframe_request_ms &&
                     now - c->last_keyframe_request_ms < KEYFRAME_MIN_INTERVAL_MS;
    if (!throttled)
        c->last_keyframe_request_ms = now;
    pthread_mutex_unlock(&d->keyframe_mu);
    if (throttled)
        return;
    RLOG_DEBUG("agent", "keyframe requested on channel %u", channel_id);
    d->cb.on_keyframe_request(d->cb.user, channel_id);
}

/* ---- media session --------------------------------------------------------------- */

static int reconnect_delay_sec(rerinku_device_t *d, int attempt) {
    int max = d->reconnect_max_delay_sec > 0 ? d->reconnect_max_delay_sec : 30;
    int delay = 1;
    for (int i = 0; i < attempt && delay < max; ++i)
        delay *= 2;
    return RK_MIN(delay, max);
}

static void schedule_rebuild(rerinku_device_t *d, uint64_t gen, int attempt, int delay_sec) {
    pthread_mutex_lock(&d->timer_mu);
    d->retry_pending = true;
    d->retry_gen = gen;
    d->retry_attempt = attempt;
    d->retry_at = rk_now_ms() + (uint64_t)delay_sec * 1000u;
    pthread_cond_broadcast(&d->timer_cv);
    pthread_mutex_unlock(&d->timer_mu);
}

/* Session thread: bookkeeping and a timer poke only, never mu (a session start
 * may hold it for seconds). */
static void on_session_state(void *user, rerinku_stream_state_t state) {
    session_ctx *ctx = user;
    rerinku_device_t *d = ctx->dev;
    if (d->cb.on_stream_state)
        d->cb.on_stream_state(d->cb.user, state);
    if (state == RERINKU_STREAM_STATE_CONNECTED) {
        atomic_store(&d->retry_attempt_now, 0);
    } else if (state == RERINKU_STREAM_STATE_FAILED || state == RERINKU_STREAM_STATE_CLOSED) {
        int attempt = atomic_load(&d->retry_attempt_now);
        int delay = reconnect_delay_sec(d, attempt);
        RLOG_WARN("agent", "media session %s — rebuilding in %ds",
                  state == RERINKU_STREAM_STATE_FAILED ? "failed" : "closed by peer", delay);
        schedule_rebuild(d, ctx->gen, attempt, delay);
    }
}

static void on_session_keyframe(void *user, uint32_t channel_id) {
    session_ctx *ctx = user;
    /* The server asks for keyframes on every track (periodic PLIs too); an
     * encoder whose stream nobody wants should not emit IDRs for nothing. */
    channel *c = find_channel(ctx->dev, channel_id);
    if (c && !atomic_load(&c->wanted))
        return;
    request_keyframe(ctx->dev, channel_id);
}

static rk_peer *peer_ref(rerinku_device_t *d) {
    pthread_mutex_lock(&d->sess_mu);
    rk_peer *p = d->peer;
    if (p)
        rk_peer_retain(p);
    pthread_mutex_unlock(&d->sess_mu);
    return p;
}

/* Tear down the media session. Used by stop, by start (teardown-then-build)
 * and on failure, so it does not own the `streaming` intent. Caller holds mu. */
static void teardown_locked(rerinku_device_t *d) {
    d->gen++; /* any in-flight rebuild for the old session is void */
    /* Unhook from the write path first: a write racing this sees the old
     * session (alive) or none, never a half-destroyed one. */
    pthread_mutex_lock(&d->sess_mu);
    rk_peer *old = d->peer;
    d->peer = NULL;
    pthread_mutex_unlock(&d->sess_mu);
    /* Close locally before the WHIP DELETE so the server's answering close
     * can't surface as a "closed by peer" worth rebuilding. */
    if (old) {
        rk_peer_close(old);
        rk_peer_release(old);
    }
    if (d->whip_resource) {
        rk_whip_delete(d->whip_resource, d->profile.token);
        free(d->whip_resource);
        d->whip_resource = NULL;
    }
    free(d->sess_ctx);
    d->sess_ctx = NULL;
}

/* Build the media session: one WHIP push of every channel. Caller holds mu. */
static bool renew_token(rerinku_device_t *d, bool mu_held, const char *seen);

static bool start_session_locked(rerinku_device_t *d, char *err, size_t errlen) {
    teardown_locked(d);

    rk_peer_track tracks[8];
    size_t n = RK_MIN(d->channel_count, RK_ARRAY_LEN(tracks));
    for (size_t i = 0; i < n; ++i) {
        const channel *c = &d->channels[i];
        tracks[i] = (rk_peer_track){c->id, c->kind, c->codec, c->codec_parameters, c->stream_id};
    }
    session_ctx *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    ctx->dev = d;
    ctx->gen = ++d->gen;
    char label[96];
    snprintf(label, sizeof(label), "%s/sfu", d->sn);
    rk_peer_config pc = {
        .tracks = tracks,
        .track_count = n,
        .stun_server = d->profile.stun_server,
        .turn_server = d->profile.turn_server,
        .nack_history = d->nack_history,
        .label = label,
        .on_state = on_session_state,
        .on_keyframe = on_session_keyframe,
        .user = ctx,
    };
    rk_peer *p = rk_peer_create(&pc, err, errlen);
    if (!p) {
        free(ctx);
        return false;
    }
    d->sess_ctx = ctx;
    char *offer = rk_peer_create_offer(p, GATHER_TIMEOUT_MS, err, errlen);
    char *answer = NULL, *resource = NULL;
    int status = 0;
    bool ok = offer && rk_whip_post(d->profile.whip_url, d->profile.token, offer, &answer,
                                    &resource, &status, err, errlen);
    if (!ok && status == 401) {
        /* Expired or no longer known (platform reset): renew, retry once. */
        RLOG_WARN("webrtc", "device token refused; renewing it");
        char *seen = rk_strdup(d->profile.token);
        if (renew_token(d, true, seen))
            ok = rk_whip_post(d->profile.whip_url, d->profile.token, offer, &answer, &resource,
                              &status, err, errlen);
        free(seen);
    }
    free(offer);
    d->whip_resource = resource;
    if (ok)
        ok = rk_peer_set_answer(p, answer, err, errlen);
    free(answer);
    if (!ok) {
        rk_peer_close(p);
        rk_peer_release(p);
        return false;
    }
    pthread_mutex_lock(&d->sess_mu);
    d->peer = p;
    pthread_mutex_unlock(&d->sess_mu);
    return true;
}

static void handle_rebuild(rerinku_device_t *d, uint64_t gen, int attempt);

/* ---- registration and timers (agent thread) --------------------------------------- */

static bool sleep_unless_stopped(rerinku_device_t *d, uint64_t ms) {
    uint64_t deadline = rk_now_ms() + ms;
    pthread_mutex_lock(&d->timer_mu);
    while (d->running && rk_cond_wait_until(&d->timer_cv, &d->timer_mu, deadline)) {
    }
    bool running = d->running;
    pthread_mutex_unlock(&d->timer_mu);
    return running;
}

static uint64_t token_renew_deadline(const rk_profile *p) {
    uint64_t in = TOKEN_UNKNOWN_RENEW_MS;
    if (p->token_lifetime_sec > 0)
        in = RK_MAX((uint64_t)p->token_lifetime_sec * 750u, (uint64_t)TOKEN_MIN_RENEW_MS);
    return rk_now_ms() + in;
}

static rk_identity identity(rerinku_device_t *d) {
    return (rk_identity){d->endpoint, d->sn, d->secret, d->name, d->device_id, d->model,
                         d->fw_version};
}

static bool register_until_stopped(rerinku_device_t *d) {
    int delay = 1;
    for (int attempt = 1; is_running(d); ++attempt) {
        rk_identity id = identity(d);
        rk_profile p;
        char err[256];
        if (rk_register(&id, &p, err, sizeof(err))) {
            pthread_mutex_lock(&d->mu);
            rk_profile_free(&d->profile);
            d->profile = p;
            d->token_renew_at = token_renew_deadline(&p);
            d->token_issued_at = rk_now_ms();
            pthread_mutex_unlock(&d->mu);
            return true;
        }
        RLOG_WARN("agent", "register with %s failed (attempt %d): %s — retrying in %ds",
                  d->endpoint, attempt, err, delay);
        if (!sleep_unless_stopped(d, (uint64_t)delay * 1000u))
            return false;
        delay = RK_MIN(delay * 2, MAX_BOOTSTRAP_DELAY_SEC);
    }
    return false;
}

static void on_mqtt_command(void *user, const char *payload, size_t len);
static void on_mqtt_connection(void *user, bool up) {
    set_status(user, up ? RERINKU_DEVICE_STATUS_ONLINE : RERINKU_DEVICE_STATUS_OFFLINE);
}

/* MQTT thread: the broker turned our token down (it expired while we were
 * offline, or the platform no longer trusts it). The agent renews it; the
 * MQTT loop keeps retrying and logs in with the new one. */
static void on_mqtt_refused(void *user) {
    rerinku_device_t *d = user;
    pthread_mutex_lock(&d->timer_mu);
    d->renew_requested = true;
    pthread_cond_broadcast(&d->timer_cv);
    pthread_mutex_unlock(&d->timer_mu);
}

static bool start_mqtt(rerinku_device_t *d) {
    pthread_mutex_lock(&d->mu);
    char *broker = rk_strdup(d->profile.mqtt_broker);
    char *token = rk_strdup(d->profile.token);
    pthread_mutex_unlock(&d->mu);
    rk_mqtt_config mc = {
        .broker = broker,
        .sn = d->sn,
        .token = token,
        .on_command = on_mqtt_command,
        .on_connection = on_mqtt_connection,
        .on_refused = on_mqtt_refused,
        .user = d,
    };
    char err[256];
    rk_mqtt *m = broker ? rk_mqtt_create(&mc, err, sizeof(err)) : NULL;
    free(broker);
    free(token);
    if (!m) {
        RLOG_ERR("agent", "MQTT control unavailable: %s", broker ? err : "no broker");
        return false;
    }
    pthread_mutex_lock(&d->mqtt_mu);
    d->mqtt = m;
    pthread_mutex_unlock(&d->mqtt_mu);
    /* The first status goes out on connect. */
    pthread_mutex_lock(&d->mu);
    report_status_locked(d);
    pthread_mutex_unlock(&d->mu);
    return rk_mqtt_start(m);
}

/* Register again for a fresh device token. `seen` is the token the caller
 * judged stale (refused by the platform, or due for renewal): if another
 * thread has replaced it meanwhile, its result stands and ours is dropped.
 * The registration is a network round trip; with mu_held the caller keeps mu
 * across it, as a session start does for its WHIP request. */
static bool renew_token(rerinku_device_t *d, bool mu_held, const char *seen) {
    rk_identity id = identity(d);
    rk_profile p;
    char err[256];
    if (!rk_register(&id, &p, err, sizeof(err))) {
        RLOG_ERR("agent", "device token renewal failed: %s", err);
        return false;
    }
    if (!mu_held)
        pthread_mutex_lock(&d->mu);
    bool install = rk_streq(d->profile.token, seen);
    if (install) {
        if (!rk_streq(p.mqtt_broker, d->profile.mqtt_broker))
            RLOG_WARN("agent", "platform now names MQTT broker %s; staying on %s until restart",
                      p.mqtt_broker, d->profile.mqtt_broker);
        /* Take the new profile but keep the broker the control connection
         * runs on; `p` then holds what is no longer needed. */
        rk_profile fresh = p;
        p = d->profile;
        char *running_broker = p.mqtt_broker;
        p.mqtt_broker = fresh.mqtt_broker;
        fresh.mqtt_broker = running_broker;
        d->profile = fresh;
        d->token_renew_at = token_renew_deadline(&d->profile);
        d->token_issued_at = rk_now_ms();
        /* Lock order mu -> mqtt_mu, as everywhere. */
        pthread_mutex_lock(&d->mqtt_mu);
        if (d->mqtt)
            rk_mqtt_set_token(d->mqtt, d->profile.token);
        pthread_mutex_unlock(&d->mqtt_mu);
    }
    int64_t lifetime = d->profile.token_lifetime_sec;
    if (!mu_held)
        pthread_mutex_unlock(&d->mu);
    rk_profile_free(&p);
    if (install)
        RLOG_INFO("agent", "renewed device token (valid %llds)", (long long)lifetime);
    return true;
}

static void renew_token_if_due(rerinku_device_t *d) {
    pthread_mutex_lock(&d->mu);
    bool due = rk_now_ms() >= d->token_renew_at;
    char *seen = due ? rk_strdup(d->profile.token) : NULL;
    pthread_mutex_unlock(&d->mu);
    if (due)
        renew_token(d, false, seen);
    free(seen);
}

/* The broker refused the token MQTT logged in with. A token we got moments
 * ago is not the problem (nor would another one be): leave it, and renew at
 * most once a minute however often the broker refuses. */
static void renew_refused_token(rerinku_device_t *d) {
    pthread_mutex_lock(&d->mu);
    uint64_t age = rk_now_ms() - d->token_issued_at;
    char *seen = age >= TOKEN_MIN_RENEW_MS ? rk_strdup(d->profile.token) : NULL;
    pthread_mutex_unlock(&d->mu);
    if (!seen) {
        RLOG_WARN("agent", "MQTT broker refused a device token issued %llus ago",
                  (unsigned long long)(age / 1000));
        return;
    }
    RLOG_WARN("agent", "MQTT broker refused the device token; renewing it");
    renew_token(d, false, seen);
    free(seen);
}

static void *agent_thread(void *arg) {
    rerinku_device_t *d = arg;
    if (!register_until_stopped(d))
        return NULL;
    RLOG_INFO("agent", "registered with %s", d->endpoint);
    if (!start_mqtt(d))
        return NULL;

    uint64_t next_token_check = rk_now_ms() + TOKEN_CHECK_INTERVAL_MS;
    pthread_mutex_lock(&d->timer_mu);
    while (d->running) {
        uint64_t wake = next_token_check;
        if (d->retry_pending && d->retry_at < wake)
            wake = d->retry_at;
        if (!d->renew_requested)
            rk_cond_wait_until(&d->timer_cv, &d->timer_mu, wake);
        if (!d->running)
            break;
        if (d->renew_requested) {
            d->renew_requested = false;
            pthread_mutex_unlock(&d->timer_mu);
            renew_refused_token(d);
            pthread_mutex_lock(&d->timer_mu);
        }
        uint64_t now = rk_now_ms();
        if (d->retry_pending && now >= d->retry_at) {
            d->retry_pending = false;
            job *j = calloc(1, sizeof(*j));
            if (j) {
                j->type = JOB_REBUILD;
                j->gen = d->retry_gen;
                j->attempt = d->retry_attempt;
            }
            pthread_mutex_unlock(&d->timer_mu);
            if (j)
                enqueue(d, j);
            pthread_mutex_lock(&d->timer_mu);
        }
        if (now >= next_token_check) {
            pthread_mutex_unlock(&d->timer_mu);
            renew_token_if_due(d);
            pthread_mutex_lock(&d->timer_mu);
            next_token_check = rk_now_ms() + TOKEN_CHECK_INTERVAL_MS;
        }
    }
    pthread_mutex_unlock(&d->timer_mu);
    return NULL;
}

/* ---- commands (command thread) ---------------------------------------------------- */

static void upload_snapshot(rerinku_device_t *d, const char *request_id, const uint8_t *jpeg,
                            size_t len) {
    size_t n = strlen(d->endpoint);
    while (n > 0 && d->endpoint[n - 1] == '/')
        --n;
    char *url = rk_sprintf("%.*s/api/v1/devices/%s/snapshot", (int)n, d->endpoint, d->sn);
    if (!url) {
        ack(d, request_id, "failed", "out of memory", NULL);
        return;
    }
    char err[256];
    int status = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        pthread_mutex_lock(&d->mu);
        char *token = rk_strdup(d->profile.token);
        pthread_mutex_unlock(&d->mu);
        rk_http_request req = {.method = "POST", .url = url, .content_type = "image/jpeg",
                               .body = jpeg, .body_len = len, .bearer = token};
        rk_http_response resp;
        bool sent = rk_http_perform(&req, &resp, err, sizeof(err));
        status = sent ? resp.status : 0;
        if (sent)
            rk_http_response_free(&resp);
        /* A refused token: renew it and send once more. */
        bool retry = status == 401 && attempt == 0 && renew_token(d, false, token);
        if (retry)
            RLOG_WARN("command", "snapshot upload: device token refused; renewed it");
        free(token);
        if (!retry)
            break;
    }
    free(url);
    if (status >= 200 && status < 300) {
        char result[48];
        snprintf(result, sizeof(result), "{\"bytes\":%zu}", len);
        RLOG_INFO("command", "get_snapshot -> uploaded %zu bytes", len);
        ack(d, request_id, "done", NULL, result);
        return;
    }
    if (status)
        snprintf(err, sizeof(err), "snapshot upload HTTP %d", status);
    RLOG_ERR("command", "snapshot upload failed: %s", err);
    ack(d, request_id, "failed", err, NULL);
}

static void handle_snapshot(rerinku_device_t *d, const char *command_id) {
    if (!d->cb.on_snapshot) {
        ack(d, command_id, "failed", "no snapshot handler", NULL);
        return;
    }
    char *superseded = NULL;
    pthread_mutex_lock(&d->reply_mu);
    if (d->pending_snapshots[MAX_PENDING_SNAPSHOTS - 1]) {
        /* Never let an application that does not answer grow this. */
        superseded = d->pending_snapshots[0];
        memmove(d->pending_snapshots, d->pending_snapshots + 1,
                sizeof(char *) * (MAX_PENDING_SNAPSHOTS - 1));
        d->pending_snapshots[MAX_PENDING_SNAPSHOTS - 1] = NULL;
    }
    for (size_t i = 0; i < MAX_PENDING_SNAPSHOTS; ++i)
        if (!d->pending_snapshots[i]) {
            d->pending_snapshots[i] = rk_strdup(command_id);
            break;
        }
    pthread_mutex_unlock(&d->reply_mu);
    if (superseded) {
        ack(d, superseded, "failed", "snapshot request superseded", NULL);
        free(superseded);
    }
    d->cb.on_snapshot(d->cb.user, command_id);
}

/* push_sfu's "streams": which video channels to send, by stream id. Absent
 * (an older platform) or naming none of ours = all. Audio is always sent:
 * a camera's audio serves every stream. Returns whether the set changed;
 * newly_wanted gets the channels that start sending. Caller holds mu. */
static bool apply_wanted_streams_locked(rerinku_device_t *d, const cJSON *streams,
                                        uint32_t *newly_wanted, size_t *newly_count) {
    bool any_known = false;
    if (cJSON_IsArray(streams)) {
        for (size_t i = 0; i < d->channel_count; ++i) {
            const cJSON *item;
            cJSON_ArrayForEach(item, streams) {
                if (cJSON_IsString(item) && d->channels[i].video &&
                    !strcmp(item->valuestring, d->channels[i].stream_id))
                    any_known = true;
            }
        }
    }
    bool changed = false;
    *newly_count = 0;
    for (size_t i = 0; i < d->channel_count; ++i) {
        channel *c = &d->channels[i];
        bool want = true;
        if (c->video && any_known) {
            want = false;
            const cJSON *item;
            cJSON_ArrayForEach(item, streams) {
                if (cJSON_IsString(item) && !strcmp(item->valuestring, c->stream_id))
                    want = true;
            }
        }
        bool was = atomic_exchange(&c->wanted, want);
        if (was != want) {
            changed = true;
            if (want)
                newly_wanted[(*newly_count)++] = c->id;
        }
    }
    return changed;
}

static void log_wanted_streams_locked(rerinku_device_t *d, const char *what) {
    char list[160] = "";
    size_t n = 0;
    for (size_t i = 0; i < d->channel_count && n < sizeof(list); ++i) {
        const channel *c = &d->channels[i];
        if (c->video && atomic_load(&c->wanted))
            n += (size_t)snprintf(list + n, sizeof(list) - n, "%s%s", n ? "," : "", c->stream_id);
    }
    RLOG_INFO("command", "push_sfu -> %s (streams: %s)", what, list);
}

static void handle_push(rerinku_device_t *d, const char *command_id, const cJSON *params) {
    const cJSON *streams = cJSON_GetObjectItemCaseSensitive(params, "streams");
    const cJSON *rev_item = cJSON_GetObjectItemCaseSensitive(params, "rev");
    const int64_t rev = cJSON_IsNumber(rev_item) ? (int64_t)rev_item->valuedouble : 0;
    uint32_t newly[8];
    size_t newly_count = 0;

    pthread_mutex_lock(&d->mu);
    rk_peer *live = peer_ref(d);
    bool connected = live && rk_peer_is_connected(live);
    if (d->streaming && connected) {
        /* The session stays; only which streams it carries may change. The
         * command is declarative and carries a rev: a stale one (delivered
         * after a newer one) changes nothing. */
        if (rev && d->streams_rev && rev <= d->streams_rev) {
            rk_peer_release(live);
            pthread_mutex_unlock(&d->mu);
            RLOG_INFO("command", "push_sfu -> stale stream set (rev %lld), ignored", (long long)rev);
            ack(d, command_id, "done", NULL, NULL);
            return;
        }
        if (rev)
            d->streams_rev = rev;
        bool changed = apply_wanted_streams_locked(d, streams, newly, &newly_count);
        /* A channel that starts sending mid-session begins at a keyframe. */
        for (size_t i = 0; i < newly_count; ++i)
            rk_peer_restart_at_keyframe(live, newly[i]);
        rk_peer_release(live);
        if (changed) {
            log_wanted_streams_locked(d, "stream set updated");
            report_status_locked(d);
        } else {
            RLOG_INFO("command", "push_sfu -> already streaming, unchanged");
        }
        pthread_mutex_unlock(&d->mu);
        ack(d, command_id, "done", NULL, NULL);
        return;
    }
    if (live)
        rk_peer_release(live);
    /* A new session (or the rebuild of a dead one) takes the set as given,
     * whatever rev the previous session reached. */
    d->streams_rev = rev;
    apply_wanted_streams_locked(d, streams, newly, &newly_count);
    if (d->streaming) {
        /* Our intent says streaming but the session is dead (server restarted,
         * or a rebuild is pending): treat the command as the rebuild. */
        RLOG_INFO("command", "push_sfu -> session not connected, rebuilding");
    } else if (d->cb.on_stream_start && d->cb.on_stream_start(d->cb.user) != 0) {
        report_status_locked(d);
        pthread_mutex_unlock(&d->mu);
        ack(d, command_id, "failed", "refused by device", NULL);
        return;
    }
    log_wanted_streams_locked(d, "starting media session (WHIP)");
    d->streaming = true;
    char err[256];
    if (!start_session_locked(d, err, sizeof(err))) {
        RLOG_ERR("command", "push_sfu failed: %s", err);
        teardown_locked(d);
        d->streaming = false;
        if (d->cb.on_stream_stop)
            d->cb.on_stream_stop(d->cb.user, "start_failed");
        report_status_locked(d);
        pthread_mutex_unlock(&d->mu);
        ack(d, command_id, "failed", err, NULL);
        return;
    }
    report_status_locked(d);
    pthread_mutex_unlock(&d->mu);
    /* "done" = accepted and started; ICE/DTLS complete asynchronously. */
    ack(d, command_id, "done", NULL, NULL);
}

static void handle_stop(rerinku_device_t *d, const char *command_id) {
    pthread_mutex_lock(&d->mu);
    RLOG_INFO("command", "stop -> tearing down the media session");
    if (d->streaming && d->cb.on_stream_stop)
        d->cb.on_stream_stop(d->cb.user, "command");
    teardown_locked(d);
    d->streaming = false;
    report_status_locked(d);
    pthread_mutex_unlock(&d->mu);
    ack(d, command_id, "done", NULL, NULL);
}

/* The device's live state (temperature, load, memory...), straight from the
 * application: the platform asks while someone looks at the device. */
static void handle_get_runtime(rerinku_device_t *d, const char *command_id) {
    if (!d->cb.on_runtime_info) {
        ack(d, command_id, "failed", "runtime info not supported", NULL);
        return;
    }
    char buf[4096];
    size_t n = d->cb.on_runtime_info(d->cb.user, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        ack(d, command_id, "failed", "no runtime info", NULL);
        return;
    }
    buf[n] = '\0';
    cJSON *info = cJSON_Parse(buf);
    if (!cJSON_IsObject(info)) {
        RLOG_WARN("command", "on_runtime_info returned no JSON object");
        cJSON_Delete(info);
        ack(d, command_id, "failed", "invalid runtime info", NULL);
        return;
    }
    char *s = cJSON_PrintUnformatted(info);
    cJSON_Delete(info);
    ack(d, command_id, "done", NULL, s);
    cJSON_free(s);
}

static void dispatch_command(rerinku_device_t *d, const char *text) {
    cJSON *m = cJSON_Parse(text);
    if (!m)
        return;
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(m, "action");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "commandId");
    const char *action = cJSON_IsString(a) ? a->valuestring : "";
    const char *command_id = cJSON_IsString(id) ? id->valuestring : "";

    if (!strcmp(action, "push_sfu")) {
        handle_push(d, command_id, cJSON_GetObjectItemCaseSensitive(m, "params"));
    } else if (!strcmp(action, "stop")) {
        handle_stop(d, command_id);
    } else if (!strcmp(action, "get_runtime")) {
        handle_get_runtime(d, command_id);
    } else if (!strcmp(action, "get_snapshot")) {
        handle_snapshot(d, command_id);
    } else if (!strcmp(action, "push_p2p")) {
        ack(d, command_id, "failed", "p2p mode is not supported; use push_sfu", NULL);
    } else if (d->cb.on_command) {
        /* params reach the handler as raw JSON: the SDK imposes no schema. */
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(m, "params");
        char *pj = params ? cJSON_PrintUnformatted(params) : NULL;
        d->cb.on_command(d->cb.user, command_id, action, pj ? pj : "{}");
        cJSON_free(pj);
    } else {
        RLOG_INFO("command", "unhandled action '%s' (no command handler)", action);
        ack(d, command_id, "failed", "unknown action", NULL);
    }
    cJSON_Delete(m);
}

/* MQTT thread: route, then hand off to the command thread. */
static void on_mqtt_command(void *user, const char *payload, size_t len) {
    rerinku_device_t *d = user;
    cJSON *m = cJSON_ParseWithLength(payload, len);
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(m, "type");
    bool command = cJSON_IsString(type) && !strcmp(type->valuestring, "command");
    cJSON_Delete(m);
    if (!command)
        return;
    job *j = calloc(1, sizeof(*j));
    if (!j)
        return;
    j->type = JOB_COMMAND;
    j->text = rk_strndup(payload, len);
    if (!j->text) {
        free(j);
        return;
    }
    enqueue(d, j);
}

/* The rebuild job runs where push_sfu runs (same lock, same thread), so it can
 * never race a stop. Every early return is the world having moved on. */
static void handle_rebuild(rerinku_device_t *d, uint64_t gen, int attempt) {
    renew_token_if_due(d); /* an outage may have outlived the token */
    pthread_mutex_lock(&d->mu);
    rk_peer *live = peer_ref(d);
    bool connected = live && rk_peer_is_connected(live);
    if (live)
        rk_peer_release(live);
    if (!d->running || !d->streaming || gen != d->gen || connected) {
        pthread_mutex_unlock(&d->mu);
        return;
    }
    RLOG_INFO("agent", "rebuilding media session (attempt %d)", attempt + 1);
    char err[256];
    if (start_session_locked(d, err, sizeof(err))) {
        RLOG_INFO("agent", "media session rebuilt (WHIP accepted)");
        report_status_locked(d);
        pthread_mutex_unlock(&d->mu);
        return;
    }
    teardown_locked(d);
    int next = attempt + 1;
    atomic_store(&d->retry_attempt_now, next);
    if (d->reconnect_max_attempts > 0 && next >= d->reconnect_max_attempts) {
        RLOG_ERR("agent", "media session rebuild failed (%s) — giving up after %d attempts",
                 err, next);
        d->streaming = false;
        if (d->cb.on_stream_stop)
            d->cb.on_stream_stop(d->cb.user, "connection_lost");
        report_status_locked(d);
        pthread_mutex_unlock(&d->mu);
        return;
    }
    int delay = reconnect_delay_sec(d, next);
    RLOG_WARN("agent", "media session rebuild failed (%s) — retrying in %ds", err, delay);
    uint64_t current = d->gen;
    pthread_mutex_unlock(&d->mu);
    schedule_rebuild(d, current, next, delay);
}

static void *command_thread(void *arg) {
    rerinku_device_t *d = arg;
    pthread_mutex_lock(&d->job_mu);
    for (;;) {
        while (d->running && !d->job_head)
            pthread_cond_wait(&d->job_cv, &d->job_mu);
        if (!d->running)
            break;
        job *j = d->job_head;
        d->job_head = j->next;
        if (!d->job_head)
            d->job_tail = NULL;
        pthread_mutex_unlock(&d->job_mu);
        switch (j->type) {
        case JOB_COMMAND: dispatch_command(d, j->text); break;
        case JOB_REBUILD: handle_rebuild(d, j->gen, j->attempt); break;
        case JOB_UPLOAD:  upload_snapshot(d, j->text, j->data, j->len); break;
        }
        job_free(j);
        pthread_mutex_lock(&d->job_mu);
    }
    pthread_mutex_unlock(&d->job_mu);
    return NULL;
}

/* ---- write path (encoder threads) --------------------------------------------------- */

static void gop_clear(channel *c) {
    for (size_t i = 0; i < c->gop_count; ++i)
        free(c->gop[i].data);
    c->gop_count = 0;
    c->gop_bytes = 0;
}

/* Keep the current GOP (keyframe + everything since) so a new session starts
 * on a picture at once. Any limit hit drops the whole GOP: a partial one is
 * useless, and the memory goes back immediately. Caller holds c->mu. */
static void gop_update(rerinku_device_t *d, channel *c, const rk_vnorm_out *au, uint64_t pts) {
    if (!au->has_vcl)
        return;
    uint64_t now = rk_now_ms();
    if (au->has_keyframe) {
        gop_clear(c);
        c->gop_keyframe_ms = now;
    } else if (c->gop_count == 0) {
        return;
    } else if (now - c->gop_keyframe_ms > GOP_MAX_AGE_MS || c->gop_count >= GOP_MAX_FRAMES ||
               c->gop_bytes + au->len > d->gop_max_bytes) {
        gop_clear(c);
        return;
    }
    if (au->len > d->gop_max_bytes)
        return;
    if (c->gop_count == c->gop_cap) {
        size_t cap = c->gop_cap ? c->gop_cap * 2 : 32;
        gop_frame *g = realloc(c->gop, cap * sizeof(*g));
        if (!g) {
            gop_clear(c);
            return;
        }
        c->gop = g;
        c->gop_cap = cap;
    }
    uint8_t *copy = malloc(au->len);
    if (!copy) {
        gop_clear(c);
        return;
    }
    memcpy(copy, au->data, au->len);
    c->gop[c->gop_count++] = (gop_frame){copy, au->len, pts, au->has_keyframe};
    c->gop_bytes += au->len;
}

rerinku_write_result_t rerinku_device_write(rerinku_device_t *d, uint32_t channel_id,
                                            const uint8_t *data, size_t len, uint64_t pts_us) {
    if (!d || !data || len == 0)
        return RERINKU_WRITE_RESULT_INVALID_FRAME;
    channel *c = find_channel(d, channel_id);
    if (!c)
        return RERINKU_WRITE_RESULT_UNKNOWN_CHANNEL;
    uint64_t pts = us_to_clock(pts_us, c->clock_rate);

    pthread_mutex_lock(&c->mu);
    const bool wanted = atomic_load(&c->wanted);
    if (!c->video) {
        rk_peer *p = wanted ? peer_ref(d) : NULL;
        rerinku_write_result_t r = p ? rk_peer_send(p, channel_id, data, len, pts, false)
                                     : RERINKU_WRITE_RESULT_NOT_STREAMING;
        if (p)
            rk_peer_release(p);
        pthread_mutex_unlock(&c->mu);
        return r;
    }

    /* Always normalize: it also tracks parameter sets for later keyframes. */
    rk_vnorm_out au;
    if (!rk_vnorm_normalize(&c->norm, c->codec, c->format, data, len, &au)) {
        pthread_mutex_unlock(&c->mu);
        return RERINKU_WRITE_RESULT_INVALID_FRAME;
    }
    const bool cache = d->gop_max_bytes > 0;
    if (cache)
        gop_update(d, c, &au, pts);

    rerinku_write_result_t r = RERINKU_WRITE_RESULT_NOT_STREAMING;
    rk_peer *p = wanted ? peer_ref(d) : NULL; /* an unwanted stream: not sent */
    if (p) {
        /* A fresh session can't use an inter frame: replay the cached GOP so
         * the picture starts now; the current frame is the GOP's tail. */
        if (cache && au.has_vcl && !au.has_keyframe && c->gop_count > 0 &&
            c->gop[0].keyframe && rk_peer_needs_keyframe(p, channel_id) &&
            rk_now_ms() - c->gop_keyframe_ms <= GOP_MAX_AGE_MS) {
            bool sent = false;
            for (size_t i = 0; i < c->gop_count; ++i) {
                r = rk_peer_send(p, channel_id, c->gop[i].data, c->gop[i].len, c->gop[i].pts,
                                 c->gop[i].keyframe);
                sent = sent || r == RERINKU_WRITE_RESULT_SENT;
            }
            if (sent)
                r = RERINKU_WRITE_RESULT_SENT;
        } else {
            r = rk_peer_send(p, channel_id, au.data, au.len, pts, au.has_keyframe);
        }
        rk_peer_release(p);
    }
    pthread_mutex_unlock(&c->mu);
    free(au.owned);
    return r;
}

/* ---- public API ------------------------------------------------------------------------ */

const char *rerinku_version(void) { return RERINKU_VERSION_STRING; }

uint32_t rerinku_version_number(void) { return RERINKU_VERSION_NUMBER; }

const char *rerinku_strerror(rerinku_err_t err) {
    switch (err) {
    case RERINKU_OK:                   return "ok";
    case RERINKU_ERR_INVALID_ARGUMENT: return "invalid argument";
    case RERINKU_ERR_INVALID_STATE:    return "invalid state";
    case RERINKU_ERR_NO_MEMORY:        return "out of memory";
    case RERINKU_ERR_INTERNAL:         return "internal error";
    case RERINKU_ERR_REJECTED:         return "rejected by the platform";
    case RERINKU_ERR_NETWORK:          return "platform unreachable";
    case RERINKU_ERR_NOT_SUPPORTED:    return "not supported by the platform";
    default:                                  return "unknown error";
    }
}

/* ---- recording upload (caller's thread) ------------------------------------------------ */

static long read_file_chunk(void *ctx, uint8_t *buf, size_t cap) {
    size_t n = fread(buf, 1, cap, (FILE *)ctx);
    if (n == 0)
        return ferror((FILE *)ctx) ? -1 : 0;
    return (long)n;
}

/* Percent-encode a query value (stream ids are usually plain already). */
static char *query_escape(const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(s);
    char *out = malloc(n * 3 + 1);
    if (!out)
        return NULL;
    char *o = out;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            *o++ = (char)*p;
        } else {
            *o++ = '%';
            *o++ = hex[*p >> 4];
            *o++ = hex[*p & 15];
        }
    }
    *o = '\0';
    return out;
}

rerinku_err_t rerinku_device_upload_recording(rerinku_device_t *d, const char *path, const char *stream_id,
                                    uint64_t start_unix_ms) {
    if (!d || !path || !*path || !start_unix_ms)
        return RERINKU_ERR_INVALID_ARGUMENT;
    pthread_mutex_lock(&d->mu);
    char *token = d->profile.token && *d->profile.token ? rk_strdup(d->profile.token) : NULL;
    pthread_mutex_unlock(&d->mu);
    if (!token)
        return RERINKU_ERR_INVALID_STATE; /* not registered (yet) */

    FILE *f = fopen(path, "rb");
    if (!f) {
        RLOG_ERR("upload", "cannot open %s", path);
        free(token);
        return RERINKU_ERR_INVALID_ARGUMENT;
    }
    long long size = -1;
    if (fseek(f, 0, SEEK_END) == 0)
        size = (long long)ftello(f);
    if (size <= 0) {
        fclose(f);
        free(token);
        return RERINKU_ERR_INVALID_ARGUMENT;
    }
    size_t n = strlen(d->endpoint);
    while (n > 0 && d->endpoint[n - 1] == '/')
        --n;
    char *stream = query_escape(stream_id && *stream_id ? stream_id : "");
    char *url = stream ? rk_sprintf("%.*s/api/v1/devices/%s/recordings?start=%llu&stream=%s", (int)n,
                                    d->endpoint, d->sn, (unsigned long long)start_unix_ms, stream)
                       : NULL;
    free(stream);
    if (!url) {
        fclose(f);
        free(token);
        return RERINKU_ERR_NO_MEMORY;
    }

    char err[256] = "";
    int status = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        rewind(f);
        rk_http_request req = {.method = "POST", .url = url, .content_type = "video/mp4",
                               .body_len = (size_t)size, .body_read = read_file_chunk, .body_ctx = f,
                               .bearer = token, .response_timeout_ms = 300000};
        rk_http_response resp;
        bool sent = rk_http_perform(&req, &resp, err, sizeof(err));
        status = sent ? resp.status : 0;
        if (sent)
            rk_http_response_free(&resp);
        bool retry = status == 401 && attempt == 0 && renew_token(d, false, token);
        if (retry) {
            RLOG_WARN("upload", "device token refused; renewed it");
            pthread_mutex_lock(&d->mu);
            char *fresh = rk_strdup(d->profile.token);
            pthread_mutex_unlock(&d->mu);
            free(token);
            token = fresh;
        } else {
            break;
        }
    }
    fclose(f);
    free(url);
    free(token);
    if (status >= 200 && status < 300) {
        RLOG_INFO("upload", "uploaded %s (%lld bytes)", path, size);
        return RERINKU_OK;
    }
    if (status == 400 || status == 413 || status == 415 || status == 422) {
        RLOG_ERR("upload", "platform rejected %s: HTTP %d", path, status);
        return RERINKU_ERR_REJECTED;
    }
    if (status == 404 || status == 405 || status == 501) {
        RLOG_WARN("upload", "platform takes no recordings (HTTP %d)", status);
        return RERINKU_ERR_NOT_SUPPORTED;
    }
    if (status)
        snprintf(err, sizeof(err), "HTTP %d", status);
    RLOG_WARN("upload", "upload of %s failed: %s", path, err);
    return RERINKU_ERR_NETWORK;
}

const char *rerinku_device_status_name(rerinku_device_status_t status) {
    switch (status) {
    case RERINKU_DEVICE_STATUS_STOPPED:    return "stopped";
    case RERINKU_DEVICE_STATUS_CONNECTING: return "connecting";
    case RERINKU_DEVICE_STATUS_ONLINE:     return "online";
    case RERINKU_DEVICE_STATUS_OFFLINE:    return "offline";
    }
    return "unknown";
}

void rerinku_device_config_init(rerinku_device_config_t *cfg) {
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->struct_size = sizeof(*cfg);
    cfg->gop_cache_max_bytes = 1024 * 1024;
    cfg->nack_history_packets = 512;
    cfg->stream_reconnect_max_delay_sec = 30;
    cfg->stream_reconnect_max_attempts = 0;
}

rerinku_err_t rerinku_device_create(const rerinku_device_config_t *config,
                          const rerinku_device_callbacks_t *callbacks, rerinku_device_t **out) {
    if (!out)
        return RERINKU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    rerinku_device_config_t cfg;
    rerinku_device_config_init(&cfg);
    if (!copy_versioned(&cfg, sizeof(cfg), config) || !cfg.endpoint || !*cfg.endpoint ||
        !cfg.sn || !*cfg.sn)
        return RERINKU_ERR_INVALID_ARGUMENT;
    rerinku_device_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    if (callbacks && !copy_versioned(&cb, sizeof(cb), callbacks))
        return RERINKU_ERR_INVALID_ARGUMENT;
    cJSON *ext = NULL;
    if (cfg.ext_json && *cfg.ext_json) {
        ext = cJSON_Parse(cfg.ext_json);
        if (!cJSON_IsObject(ext)) {
            RLOG_ERR("agent", "ext_json must be a JSON object");
            cJSON_Delete(ext);
            return RERINKU_ERR_INVALID_ARGUMENT;
        }
    }

    rerinku_device_t *d = calloc(1, sizeof(*d));
    if (!d) {
        cJSON_Delete(ext);
        return RERINKU_ERR_NO_MEMORY;
    }
    d->ext = ext;
    d->endpoint = rk_strdup(cfg.endpoint);
    d->sn = rk_strdup(cfg.sn);
    d->secret = rk_strdup(cfg.secret);
    d->name = rk_strdup(cfg.name);
    d->device_id = rk_strdup(cfg.device_id);
    d->model = rk_strdup(cfg.model);
    d->fw_version = rk_strdup(cfg.fw_version);
    d->ca_file = rk_strdup(cfg.ca_file);
    d->tls_insecure = cfg.tls_insecure != 0;
    d->gop_max_bytes = cfg.gop_cache_max_bytes;
    d->nack_history = cfg.nack_history_packets;
    d->reconnect_max_delay_sec = (int)cfg.stream_reconnect_max_delay_sec;
    d->reconnect_max_attempts = (int)cfg.stream_reconnect_max_attempts;
    d->cb = cb;
    pthread_mutex_init(&d->life_mu, NULL);
    pthread_mutex_init(&d->timer_mu, NULL);
    rk_cond_init(&d->timer_cv);
    pthread_mutex_init(&d->job_mu, NULL);
    pthread_cond_init(&d->job_cv, NULL);
    pthread_mutex_init(&d->mu, NULL);
    pthread_mutex_init(&d->sess_mu, NULL);
    pthread_mutex_init(&d->mqtt_mu, NULL);
    pthread_mutex_init(&d->reply_mu, NULL);
    pthread_mutex_init(&d->keyframe_mu, NULL);
    atomic_init(&d->retry_attempt_now, 0);
    if (!d->endpoint || !d->sn) {
        rerinku_device_destroy(d);
        return RERINKU_ERR_NO_MEMORY;
    }
    *out = d;
    return RERINKU_OK;
}

rerinku_err_t rerinku_device_add_channel(rerinku_device_t *d, const rerinku_device_channel_t *channel_cfg) {
    if (!d)
        return RERINKU_ERR_INVALID_ARGUMENT;
    rerinku_device_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    if (!copy_versioned(&ch, sizeof(ch), channel_cfg) || !valid_kind_codec(ch.kind, ch.codec) ||
        (ch.kind == RERINKU_MEDIA_KIND_AUDIO && ch.bitstream_format != RERINKU_BITSTREAM_FORMAT_AUTO) ||
        (ch.stream_id && strlen(ch.stream_id) > 64))
        return RERINKU_ERR_INVALID_ARGUMENT;

    pthread_mutex_lock(&d->life_mu);
    int rc = RERINKU_OK;
    if (d->started) {
        rc = RERINKU_ERR_INVALID_STATE;
    } else if (find_channel(d, ch.channel_id) || d->channel_count >= 8) {
        rc = RERINKU_ERR_INVALID_ARGUMENT;
    } else {
        channel *grown = realloc(d->channels, (d->channel_count + 1) * sizeof(channel));
        if (!grown) {
            rc = RERINKU_ERR_NO_MEMORY;
        } else {
            d->channels = grown;
            channel *c = &d->channels[d->channel_count];
            memset(c, 0, sizeof(*c));
            c->id = ch.channel_id;
            c->kind = ch.kind;
            c->codec = ch.codec;
            c->format = ch.bitstream_format;
            c->label = rk_strdup(ch.label);
            c->stream_id = rk_strdup(ch.stream_id && *ch.stream_id ? ch.stream_id : "rerinku");
            c->codec_parameters = rk_strdup(ch.codec_parameters);
            c->width = ch.width;
            c->height = ch.height;
            c->fps = ch.fps;
            c->bitrate_bps = ch.bitrate_bps;
            c->sample_rate = ch.sample_rate;
            c->audio_channels = ch.audio_channels;
            c->sample_bits = ch.sample_bits;
            c->clock_rate = clock_rate_for(ch.kind, ch.codec);
            c->video = ch.kind == RERINKU_MEDIA_KIND_VIDEO;
            atomic_init(&c->wanted, true);
            pthread_mutex_init(&c->mu, NULL);
            if (!c->stream_id) {
                free(c->label);
                free(c->codec_parameters);
                rc = RERINKU_ERR_NO_MEMORY;
            } else {
                d->channel_count++;
            }
        }
    }
    pthread_mutex_unlock(&d->life_mu);
    return rc;
}

rerinku_err_t rerinku_device_start(rerinku_device_t *d) {
    if (!d)
        return RERINKU_ERR_INVALID_ARGUMENT;
    pthread_mutex_lock(&d->life_mu);
    if (d->started || d->channel_count == 0) {
        pthread_mutex_unlock(&d->life_mu);
        return RERINKU_ERR_INVALID_STATE;
    }
    rk_log_init();
    rk_tls_configure(d->ca_file, d->tls_insecure);
    if (d->tls_insecure)
        RLOG_WARN("agent", "TLS certificate verification is DISABLED");
    d->running = true;
    atomic_store(&d->retry_attempt_now, 0);
    set_status(d, RERINKU_DEVICE_STATUS_CONNECTING);
    if (pthread_create(&d->cmd_thread, NULL, command_thread, d) != 0) {
        d->running = false;
        pthread_mutex_unlock(&d->life_mu);
        return RERINKU_ERR_INTERNAL;
    }
    if (pthread_create(&d->agent_thread, NULL, agent_thread, d) != 0) {
        pthread_mutex_lock(&d->job_mu);
        d->running = false;
        pthread_cond_broadcast(&d->job_cv);
        pthread_mutex_unlock(&d->job_mu);
        pthread_join(d->cmd_thread, NULL);
        pthread_mutex_unlock(&d->life_mu);
        return RERINKU_ERR_INTERNAL;
    }
    d->started = true;
    pthread_mutex_unlock(&d->life_mu);
    return RERINKU_OK;
}

void rerinku_device_stop(rerinku_device_t *d) {
    if (!d)
        return;
    pthread_mutex_lock(&d->life_mu);
    if (!d->started) {
        pthread_mutex_unlock(&d->life_mu);
        return;
    }
    pthread_mutex_lock(&d->timer_mu);
    pthread_mutex_lock(&d->job_mu);
    d->running = false;
    pthread_cond_broadcast(&d->job_cv);
    pthread_mutex_unlock(&d->job_mu);
    pthread_cond_broadcast(&d->timer_cv);
    pthread_mutex_unlock(&d->timer_mu);
    pthread_join(d->agent_thread, NULL);
    pthread_join(d->cmd_thread, NULL);
    pthread_mutex_lock(&d->job_mu);
    while (d->job_head) {
        job *j = d->job_head;
        d->job_head = j->next;
        job_free(j);
    }
    d->job_tail = NULL;
    pthread_mutex_unlock(&d->job_mu);
    d->retry_pending = false;

    pthread_mutex_lock(&d->mu);
    if (d->streaming && d->cb.on_stream_stop)
        d->cb.on_stream_stop(d->cb.user, "shutdown");
    d->streaming = false;
    pthread_mutex_unlock(&d->mu);

    pthread_mutex_lock(&d->mqtt_mu);
    rk_mqtt *m = d->mqtt;
    d->mqtt = NULL;
    pthread_mutex_unlock(&d->mqtt_mu);
    rk_mqtt_destroy(m); /* publishes offline before disconnecting */

    pthread_mutex_lock(&d->mu);
    teardown_locked(d);
    pthread_mutex_unlock(&d->mu);
    for (size_t i = 0; i < d->channel_count; ++i) {
        pthread_mutex_lock(&d->channels[i].mu);
        gop_clear(&d->channels[i]);
        pthread_mutex_unlock(&d->channels[i].mu);
    }
    pthread_mutex_lock(&d->reply_mu);
    for (size_t i = 0; i < MAX_PENDING_SNAPSHOTS; ++i) {
        free(d->pending_snapshots[i]);
        d->pending_snapshots[i] = NULL;
    }
    pthread_mutex_unlock(&d->reply_mu);
    d->started = false;
    pthread_mutex_unlock(&d->life_mu);
    set_status(d, RERINKU_DEVICE_STATUS_STOPPED);
}

void rerinku_device_destroy(rerinku_device_t *d) {
    if (!d)
        return;
    rerinku_device_stop(d);
    for (size_t i = 0; i < d->channel_count; ++i) {
        channel *c = &d->channels[i];
        gop_clear(c);
        free(c->gop);
        rk_vnorm_free(&c->norm);
        free(c->label);
        free(c->stream_id);
        free(c->codec_parameters);
        pthread_mutex_destroy(&c->mu);
    }
    free(d->channels);
    for (size_t i = 0; i < REPLIED_RING; ++i)
        free(d->replied[i]);
    rk_profile_free(&d->profile);
    free(d->endpoint);
    free(d->sn);
    free(d->secret);
    free(d->name);
    free(d->device_id);
    free(d->model);
    free(d->fw_version);
    free(d->ca_file);
    cJSON_Delete(d->ext);
    pthread_mutex_destroy(&d->life_mu);
    pthread_mutex_destroy(&d->timer_mu);
    pthread_cond_destroy(&d->timer_cv);
    pthread_mutex_destroy(&d->job_mu);
    pthread_cond_destroy(&d->job_cv);
    pthread_mutex_destroy(&d->mu);
    pthread_mutex_destroy(&d->sess_mu);
    pthread_mutex_destroy(&d->mqtt_mu);
    pthread_mutex_destroy(&d->reply_mu);
    pthread_mutex_destroy(&d->keyframe_mu);
    free(d);
}

rerinku_err_t rerinku_device_reply_command(rerinku_device_t *d, const char *command_id,
                                 const char *result_json) {
    if (!d || !command_id || !*command_id)
        return RERINKU_ERR_INVALID_ARGUMENT;
    if (first_reply(d, command_id))
        ack(d, command_id, "done", NULL, result_json);
    return RERINKU_OK;
}

rerinku_err_t rerinku_device_fail_command(rerinku_device_t *d, const char *command_id, const char *error) {
    if (!d || !command_id || !*command_id)
        return RERINKU_ERR_INVALID_ARGUMENT;
    if (first_reply(d, command_id))
        ack(d, command_id, "failed", error && *error ? error : "failed", NULL);
    return RERINKU_OK;
}

rerinku_err_t rerinku_device_reply_snapshot(rerinku_device_t *d, const char *request_id,
                                  const uint8_t *jpeg, size_t len) {
    if (!d || !request_id || !*request_id)
        return RERINKU_ERR_INVALID_ARGUMENT;
    bool known = false;
    pthread_mutex_lock(&d->reply_mu);
    for (size_t i = 0; i < MAX_PENDING_SNAPSHOTS; ++i)
        if (d->pending_snapshots[i] && !strcmp(d->pending_snapshots[i], request_id)) {
            free(d->pending_snapshots[i]);
            memmove(d->pending_snapshots + i, d->pending_snapshots + i + 1,
                    sizeof(char *) * (MAX_PENDING_SNAPSHOTS - 1 - i));
            d->pending_snapshots[MAX_PENDING_SNAPSHOTS - 1] = NULL;
            known = true;
            break;
        }
    pthread_mutex_unlock(&d->reply_mu);
    if (!known) {
        RLOG_WARN("command", "snapshot reply for unknown request %s ignored", request_id);
        return RERINKU_OK;
    }
    if (!jpeg || len == 0) {
        ack(d, request_id, "failed", "no frame available", NULL);
        return RERINKU_OK;
    }
    /* The caller may be an encoder thread: upload on the command thread. */
    job *j = calloc(1, sizeof(*j));
    uint8_t *copy = j ? malloc(len) : NULL;
    char *id = copy ? rk_strdup(request_id) : NULL;
    if (!id) {
        free(copy);
        free(j);
        return RERINKU_ERR_NO_MEMORY;
    }
    memcpy(copy, jpeg, len);
    j->type = JOB_UPLOAD;
    j->text = id;
    j->data = copy;
    j->len = len;
    enqueue(d, j);
    return RERINKU_OK;
}

rerinku_err_t rerinku_device_send_event(rerinku_device_t *d, const char *action, const char *payload_json) {
    if (!d || !action || !*action)
        return RERINKU_ERR_INVALID_ARGUMENT;
    if (!is_running(d))
        return RERINKU_ERR_INVALID_STATE;
    cJSON *j = cJSON_CreateObject();
    if (j) {
        cJSON_AddStringToObject(j, "action", action);
        add_json_or_string(j, "data", payload_json ? payload_json : "");
    }
    char *s = j ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    if (!s)
        return RERINKU_ERR_NO_MEMORY;
    bool sent = mqtt_publish(d, PUB_DATA, s); /* not queued: offline means lost */
    cJSON_free(s);
    return sent ? RERINKU_OK : RERINKU_ERR_NETWORK;
}
