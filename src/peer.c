#include "peer.h"

#include "common.h"
#include "dtls.h"
#include "log.h"
#include "rtp.h"
#include "sdp.h"

#include <juice/juice.h>
#include <srtp.h>

#include <stdatomic.h>
#include <stdio.h>

/* Video pacing: spread a large keyframe over a few milliseconds instead of
 * dumping ~100 packets into the socket at once (the receiver's NIC/driver and
 * Wi-Fi drop the burst and the keyframe never reassembles). The rate is a
 * ceiling far above any camera bitrate; it only shapes bursts. */
#define PACE_BYTES_PER_SEC (48000000 / 8)
#define PACE_BURST_BYTES (24 * 1024)
/* Encoder output waiting to be sent. Beyond this the link cannot keep up:
 * drop the frame and restart from a keyframe rather than queue seconds. */
#define MAX_QUEUED_BYTES (2u * 1024 * 1024)
#define MAX_INCOMING 256
#define CONNECT_TIMEOUT_MS 15000
#define SR_INTERVAL_MS 1000
#define MAX_EVENTS 16

typedef struct rk_pkt {
    struct rk_pkt *next;
    uint16_t seq;
    uint16_t len;
    uint8_t track;
    uint8_t data[];
} rk_pkt;

typedef struct pkt_queue {
    rk_pkt *head, *tail;
    size_t count, bytes;
} pkt_queue;

static void q_push(pkt_queue *q, rk_pkt *pkt) {
    pkt->next = NULL;
    if (q->tail)
        q->tail->next = pkt;
    else
        q->head = pkt;
    q->tail = pkt;
    q->count++;
    q->bytes += pkt->len;
}

static rk_pkt *q_pop(pkt_queue *q) {
    rk_pkt *pkt = q->head;
    if (!pkt)
        return NULL;
    q->head = pkt->next;
    if (!q->head)
        q->tail = NULL;
    q->count--;
    q->bytes -= pkt->len;
    pkt->next = NULL;
    return pkt;
}

static void q_free(pkt_queue *q) {
    rk_pkt *pkt;
    while ((pkt = q_pop(q)) != NULL)
        free(pkt);
}

typedef struct track {
    uint32_t channel_id;
    int kind, codec;
    bool video;
    char *fmtp;
    char *stream_id;
    char mid[12];
    char cname[32];
    rk_rtp_stream rtp; /* seq advanced under mu (packetizer) */

    /* Send side, under mu. */
    bool have_base;
    uint64_t base_pts;
    bool need_keyframe;
    bool keyframe_wanted; /* to report from the session thread */

    /* Session thread only. */
    uint32_t packets, octets;
    uint32_t last_rtp_ts;
    uint64_t last_send_us;
    rk_pkt **history; /* sent packets by seq % history_size, for NACKs */
    size_t history_size;
} track;

typedef struct event {
    enum { EV_STATE, EV_KEYFRAME } type;
    uint32_t value;
} event;

struct rk_peer {
    atomic_int refs;
    char *label;
    void (*on_state)(void *, rerinku_stream_state_t);
    void (*on_keyframe)(void *, uint32_t);
    void *user;

    track *tracks;
    size_t track_count;
    juice_agent_t *agent;
    char *stun_host;
    char *turn_host, *turn_user, *turn_pass;

    pthread_mutex_t mu;
    pthread_cond_t cv;
    /* Under mu. */
    bool gathered;
    pkt_queue incoming;
    juice_state_t ice_state;
    bool ice_changed;
    pkt_queue audio_q, video_q;
    bool ready;   /* SRTP keys in place: media may flow */
    bool failed;
    bool closing;
    bool thread_started;
    pthread_t thread;

    /* Session thread only. */
    rk_dtls *dtls;
    bool dtls_server;
    bool dtls_started;
    bool dtls_done;
    srtp_t srtp_out, srtp_in;
    uint64_t connect_deadline;
    double tokens;
    uint64_t last_refill_us;
    uint64_t next_sr_ms;
    event events[MAX_EVENTS];
    size_t event_count;
};

/* ---- helpers ------------------------------------------------------------------ */

static pthread_once_t g_srtp_once = PTHREAD_ONCE_INIT;
static bool g_srtp_ok;
static void srtp_init_once(void) { g_srtp_ok = srtp_init() == srtp_err_status_ok; }

static uint8_t payload_type_for(int codec) {
    switch (codec) {
    case RERINKU_CODEC_H265:  return 98;
    case RERINKU_CODEC_OPUS:  return 111;
    case RERINKU_CODEC_G711U: return 0;
    case RERINKU_CODEC_G711A: return 8;
    default:                  return 96;
    }
}

static track *track_by_channel(rk_peer *p, uint32_t channel_id) {
    for (size_t i = 0; i < p->track_count; ++i)
        if (p->tracks[i].channel_id == channel_id)
            return &p->tracks[i];
    return NULL;
}

static track *track_by_ssrc(rk_peer *p, uint32_t ssrc) {
    for (size_t i = 0; i < p->track_count; ++i)
        if (p->tracks[i].rtp.ssrc == ssrc)
            return &p->tracks[i];
    return NULL;
}

/* Queue a callback for delivery outside the lock (session thread only). */
static void push_event(rk_peer *p, int type, uint32_t value) {
    if (p->event_count < MAX_EVENTS)
        p->events[p->event_count++] = (event){type, value};
}

/* "stun:host[:port]" / "turn:user:pass@host[:port][?transport=udp]". */
static bool parse_ice_url(const char *url, const char *scheme, char **host, uint16_t *port,
                          char **user, char **pass) {
    size_t sl = strlen(scheme);
    if (!url || strncmp(url, scheme, sl) != 0)
        return false;
    const char *s = url + sl;
    const char *at = strrchr(s, '@');
    if (at && user && pass) {
        const char *colon = memchr(s, ':', (size_t)(at - s));
        if (!colon)
            return false;
        *user = rk_strndup(s, (size_t)(colon - s));
        *pass = rk_strndup(colon + 1, (size_t)(at - colon - 1));
        s = at + 1;
    }
    size_t end = strcspn(s, "?");
    const char *colon = NULL;
    for (const char *c = s; c < s + end; ++c)
        if (*c == ':')
            colon = c;
    *port = 3478;
    if (colon) {
        *host = rk_strndup(s, (size_t)(colon - s));
        *port = (uint16_t)atoi(colon + 1);
    } else {
        *host = rk_strndup(s, end);
    }
    return *host && **host;
}

/* ---- libjuice callbacks (juice thread): queue and wake, nothing else -------- */

static void on_ice_state(juice_agent_t *agent, juice_state_t state, void *user) {
    (void)agent;
    rk_peer *p = user;
    pthread_mutex_lock(&p->mu);
    p->ice_state = state;
    p->ice_changed = true;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
}

static void on_ice_candidate(juice_agent_t *agent, const char *sdp, void *user) {
    (void)agent;
    (void)sdp;
    (void)user; /* non-trickle: candidates are read once gathering is done */
}

static void on_ice_gathering_done(juice_agent_t *agent, void *user) {
    (void)agent;
    rk_peer *p = user;
    pthread_mutex_lock(&p->mu);
    p->gathered = true;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
}

static void on_ice_recv(juice_agent_t *agent, const char *data, size_t size, void *user) {
    (void)agent;
    rk_peer *p = user;
    if (size == 0 || size > 2048)
        return;
    /* Only DTLS and RTCP matter to a send-only peer. */
    if (!rk_is_dtls((const uint8_t *)data, size) && !rk_is_rtcp((const uint8_t *)data, size))
        return;
    rk_pkt *pkt = malloc(sizeof(rk_pkt) + size);
    if (!pkt)
        return;
    memcpy(pkt->data, data, size);
    pkt->len = (uint16_t)size;
    pthread_mutex_lock(&p->mu);
    if (p->incoming.count < MAX_INCOMING && !p->closing) {
        q_push(&p->incoming, pkt);
        pkt = NULL;
        pthread_cond_broadcast(&p->cv);
    }
    pthread_mutex_unlock(&p->mu);
    free(pkt);
}

/* ---- creation ---------------------------------------------------------------- */

rk_peer *rk_peer_create(const rk_peer_config *cfg, char *err, size_t errlen) {
    if (!cfg->tracks || cfg->track_count == 0 || cfg->track_count > RK_SDP_MAX_MEDIA) {
        snprintf(err, errlen, "a session needs 1..%d tracks", RK_SDP_MAX_MEDIA);
        return NULL;
    }
    pthread_once(&g_srtp_once, srtp_init_once);
    if (!g_srtp_ok) {
        snprintf(err, errlen, "SRTP initialization failed");
        return NULL;
    }
    rk_log_init();

    rk_peer *p = calloc(1, sizeof(*p));
    if (!p) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    atomic_init(&p->refs, 1);
    pthread_mutex_init(&p->mu, NULL);
    rk_cond_init(&p->cv);
    p->label = rk_strdup(cfg->label ? cfg->label : "session");
    p->on_state = cfg->on_state;
    p->on_keyframe = cfg->on_keyframe;
    p->user = cfg->user;
    p->tracks = calloc(cfg->track_count, sizeof(track));
    if (!p->tracks)
        goto oom;
    p->track_count = cfg->track_count;

    for (size_t i = 0; i < cfg->track_count; ++i) {
        const rk_peer_track *in = &cfg->tracks[i];
        track *t = &p->tracks[i];
        t->channel_id = in->channel_id;
        t->kind = in->kind;
        t->codec = in->codec;
        t->video = in->kind == RERINKU_MEDIA_KIND_VIDEO;
        t->fmtp = rk_strdup(in->codec_parameters);
        t->stream_id = rk_strdup(in->stream_id && *in->stream_id ? in->stream_id : "rerinku");
        snprintf(t->mid, sizeof(t->mid), "%u", in->channel_id);
        snprintf(t->cname, sizeof(t->cname), "rerinku-%u", in->channel_id);
        do
            t->rtp.ssrc = rk_random_u32();
        while (t->rtp.ssrc == 0 || (i > 0 && track_by_ssrc(p, t->rtp.ssrc) != t));
        t->rtp.payload_type = payload_type_for(in->codec);
        t->rtp.clock_rate = rk_sdp_clock_rate(in->codec);
        t->rtp.seq = (uint16_t)rk_random_u32();
        t->rtp.ts_offset = rk_random_u32();
        t->need_keyframe = t->video;
        if (!t->stream_id)
            goto oom;
        if (t->video && cfg->nack_history > 0) {
            t->history_size = cfg->nack_history;
            t->history = calloc(t->history_size, sizeof(rk_pkt *));
            if (!t->history)
                goto oom;
        }
    }

    juice_config_t jc = {0};
    jc.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
    uint16_t stun_port = 0;
    if (cfg->stun_server && *cfg->stun_server &&
        parse_ice_url(cfg->stun_server, "stun:", &p->stun_host, &stun_port, NULL, NULL)) {
        jc.stun_server_host = p->stun_host;
        jc.stun_server_port = stun_port;
    }
    juice_turn_server_t turn = {0};
    if (cfg->turn_server && *cfg->turn_server &&
        parse_ice_url(cfg->turn_server, "turn:", &p->turn_host, &turn.port, &p->turn_user,
                      &p->turn_pass)) {
        turn.host = p->turn_host;
        turn.username = p->turn_user;
        turn.password = p->turn_pass;
        jc.turn_servers = &turn;
        jc.turn_servers_count = 1;
    }
    jc.cb_state_changed = on_ice_state;
    jc.cb_candidate = on_ice_candidate;
    jc.cb_gathering_done = on_ice_gathering_done;
    jc.cb_recv = on_ice_recv;
    jc.user_ptr = p;
    p->agent = juice_create(&jc);
    if (!p->agent) {
        snprintf(err, errlen, "ICE agent creation failed");
        rk_peer_release(p);
        return NULL;
    }
    return p;

oom:
    snprintf(err, errlen, "out of memory");
    rk_peer_release(p);
    return NULL;
}

/* ---- offer / answer -------------------------------------------------------------- */

static void fill_sdp_tracks(rk_peer *p, rk_sdp_track *out) {
    for (size_t i = 0; i < p->track_count; ++i) {
        track *t = &p->tracks[i];
        memset(&out[i], 0, sizeof(out[i]));
        rk_strlcpy(out[i].mid, t->mid, sizeof(out[i].mid));
        out[i].kind = t->kind;
        out[i].codec = t->codec;
        out[i].payload_type = t->rtp.payload_type;
        out[i].fmtp = t->fmtp;
        out[i].ssrc = t->rtp.ssrc;
        out[i].cname = t->cname;
        out[i].stream_id = t->stream_id;
        out[i].track_id = t->mid; /* track id = channel id, as before */
    }
}

char *rk_peer_create_offer(rk_peer *p, int timeout_ms, char *err, size_t errlen) {
    const char *fp = rk_dtls_local_fingerprint();
    if (!fp) {
        snprintf(err, errlen, "no DTLS certificate");
        return NULL;
    }
    if (juice_gather_candidates(p->agent) != JUICE_ERR_SUCCESS) {
        snprintf(err, errlen, "ICE gathering failed to start");
        return NULL;
    }
    /* Non-trickle: WHIP carries every candidate in the one POST. Bounded: a
     * broken network must not hold the caller forever; whatever was gathered
     * by then goes out. */
    uint64_t deadline = rk_now_ms() + (uint64_t)timeout_ms;
    pthread_mutex_lock(&p->mu);
    while (!p->gathered && !p->closing)
        if (!rk_cond_wait_until(&p->cv, &p->mu, deadline))
            break;
    bool gathered = p->gathered;
    pthread_mutex_unlock(&p->mu);
    if (!gathered)
        RLOG_WARN("webrtc", "%s: ICE gathering incomplete after %d ms, offering what we have",
                  p->label, timeout_ms);

    char ice[JUICE_MAX_SDP_STRING_LEN];
    if (juice_get_local_description(p->agent, ice, sizeof(ice)) < 0) {
        snprintf(err, errlen, "ICE description unavailable");
        return NULL;
    }
    rk_sdp_track tracks[RK_SDP_MAX_MEDIA];
    fill_sdp_tracks(p, tracks);
    char *sdp = rk_sdp_build_offer(tracks, p->track_count, ice, fp);
    if (!sdp)
        snprintf(err, errlen, "out of memory");
    return sdp;
}

static void *peer_thread(void *arg);
static bool dtls_send(void *ctx, const uint8_t *data, size_t len);

bool rk_peer_set_answer(rk_peer *p, const char *sdp, char *err, size_t errlen) {
    rk_sdp_track tracks[RK_SDP_MAX_MEDIA];
    fill_sdp_tracks(p, tracks);
    rk_sdp_answer ans;
    if (!rk_sdp_parse_answer(sdp, tracks, p->track_count, &ans, err, errlen))
        return false;

    bool ok = true;
    for (size_t i = 0; i < p->track_count && ok; ++i) {
        track *t = &p->tracks[i];
        const rk_sdp_answer_media *m = NULL;
        for (size_t j = 0; j < ans.media_count; ++j)
            if (!strcmp(ans.media[j].mid, t->mid))
                m = &ans.media[j];
        if (!m || m->rejected || m->payload_type < 0) {
            snprintf(err, errlen, "the server did not accept %s channel %u (%s)",
                     t->video ? "video" : "audio", t->channel_id, rk_sdp_codec_name(t->codec));
            ok = false;
            break;
        }
        t->rtp.payload_type = (uint8_t)m->payload_type;
    }
    /* Answer "active": the server opens DTLS and we answer it. */
    p->dtls_server = strcmp(ans.setup, "passive") != 0;
    if (ok) {
        p->dtls = rk_dtls_create(p->dtls_server, ans.fingerprint, dtls_send, p, err, errlen);
        ok = p->dtls != NULL;
    }
    if (ok && juice_set_remote_description(p->agent, ans.ice) != JUICE_ERR_SUCCESS) {
        snprintf(err, errlen, "invalid ICE attributes in the answer");
        ok = false;
    }
    rk_sdp_answer_free(&ans);
    if (!ok)
        return false;
    juice_set_remote_gathering_done(p->agent);

    p->connect_deadline = rk_now_ms() + CONNECT_TIMEOUT_MS;
    pthread_mutex_lock(&p->mu);
    rk_peer_retain(p); /* the thread's reference */
    if (pthread_create(&p->thread, NULL, peer_thread, p) != 0) {
        pthread_mutex_unlock(&p->mu);
        rk_peer_release(p);
        snprintf(err, errlen, "cannot start the session thread");
        return false;
    }
    p->thread_started = true;
    pthread_mutex_unlock(&p->mu);
    return true;
}

/* ---- session thread ------------------------------------------------------------ */

static bool dtls_send(void *ctx, const uint8_t *data, size_t len) {
    rk_peer *p = ctx;
    return juice_send(p->agent, (const char *)data, len) == JUICE_ERR_SUCCESS;
}

static void transmit(rk_peer *p, const uint8_t *pkt, size_t len, bool rtcp) {
    uint8_t buf[RK_RTP_MAX_PACKET + SRTP_MAX_TRAILER_LEN + 64];
    if (len > RK_RTP_MAX_PACKET + 64)
        return;
    memcpy(buf, pkt, len);
    int n = (int)len;
    srtp_err_status_t st = rtcp ? srtp_protect_rtcp(p->srtp_out, buf, &n)
                                : srtp_protect(p->srtp_out, buf, &n);
    if (st != srtp_err_status_ok) {
        RLOG_DEBUG("webrtc", "%s: SRTP protect failed (%d)", p->label, (int)st);
        return;
    }
    juice_send(p->agent, (const char *)buf, (size_t)n);
}

static void send_rtp(rk_peer *p, rk_pkt *pkt) {
    track *t = &p->tracks[pkt->track];
    transmit(p, pkt->data, pkt->len, false);
    t->packets++;
    t->octets += pkt->len - RK_RTP_HEADER;
    t->last_rtp_ts = rk_rtp_ts(pkt->data);
    t->last_send_us = rk_now_us();
    if (t->history_size) {
        size_t idx = pkt->seq % t->history_size;
        free(t->history[idx]);
        t->history[idx] = pkt;
    } else {
        free(pkt);
    }
}

static void on_nack(void *ctx, uint32_t ssrc, uint16_t seq) {
    rk_peer *p = ctx;
    track *t = track_by_ssrc(p, ssrc);
    if (!t || !t->history_size)
        return;
    rk_pkt *pkt = t->history[seq % t->history_size];
    if (pkt && pkt->seq == seq)
        transmit(p, pkt->data, pkt->len, false); /* same seq: plain retransmission */
}

static void on_rtcp_keyframe(void *ctx, uint32_t ssrc) {
    rk_peer *p = ctx;
    track *t = track_by_ssrc(p, ssrc);
    if (t && t->video)
        push_event(p, EV_KEYFRAME, t->channel_id);
}

static bool setup_srtp(rk_peer *p) {
    uint8_t local[RK_SRTP_KEY_SALT_LEN], remote[RK_SRTP_KEY_SALT_LEN];
    if (!rk_dtls_srtp_keys(p->dtls, local, remote))
        return false;
    srtp_policy_t out, in;
    memset(&out, 0, sizeof(out));
    memset(&in, 0, sizeof(in));
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&out.rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&out.rtcp);
    out.ssrc.type = ssrc_any_outbound;
    out.key = local;
    out.window_size = 1024;
    out.allow_repeat_tx = 1; /* NACK retransmissions reuse the sequence number */
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&in.rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&in.rtcp);
    in.ssrc.type = ssrc_any_inbound;
    in.key = remote;
    in.window_size = 1024;
    bool ok = srtp_create(&p->srtp_out, &out) == srtp_err_status_ok &&
              srtp_create(&p->srtp_in, &in) == srtp_err_status_ok;
    memset(local, 0, sizeof(local));
    memset(remote, 0, sizeof(remote));
    return ok;
}

/* The session is over (FAILED, or CLOSED by the peer): stop sending and
 * report it once; the owner decides whether to rebuild. */
static void end_session(rk_peer *p, rerinku_stream_state_t state, const char *why) {
    if (p->failed)
        return;
    RLOG_WARN("webrtc", "%s: %s", p->label, why);
    pthread_mutex_lock(&p->mu);
    p->failed = true;
    p->ready = false;
    pthread_mutex_unlock(&p->mu);
    push_event(p, EV_STATE, state);
}

static void fail(rk_peer *p, const char *why) { end_session(p, RERINKU_STREAM_STATE_FAILED, why); }

/* Drive the DTLS handshake one step; on completion bring up SRTP. */
static void step_dtls(rk_peer *p) {
    char err[200];
    int rc = rk_dtls_step(p->dtls, err, sizeof(err));
    if (rc == 0)
        return;
    if (rc < 0) {
        fail(p, err);
        return;
    }
    p->dtls_done = true;
    if (!setup_srtp(p)) {
        fail(p, "SRTP keying failed");
        return;
    }
    RLOG_INFO("webrtc", "%s: connected (DTLS %s)", p->label, p->dtls_server ? "server" : "client");
    pthread_mutex_lock(&p->mu);
    p->ready = true;
    pthread_mutex_unlock(&p->mu);
    push_event(p, EV_STATE, RERINKU_STREAM_STATE_CONNECTED);
    /* Nothing decodable flows until a keyframe: ask for one right away. */
    for (size_t i = 0; i < p->track_count; ++i)
        if (p->tracks[i].video)
            push_event(p, EV_KEYFRAME, p->tracks[i].channel_id);
}

static void handle_datagram(rk_peer *p, rk_pkt *pkt) {
    if (rk_is_dtls(pkt->data, pkt->len)) {
        if (!p->dtls)
            return;
        rk_dtls_input(p->dtls, pkt->data, pkt->len);
        if (!p->dtls_done) {
            p->dtls_started = true; /* a server learns the peer started */
            step_dtls(p);
        } else if (rk_dtls_peer_closed(p->dtls)) {
            /* The server ended the session (restart, WHIP DELETE): rebuild
             * now instead of waiting for ICE consent to expire. */
            end_session(p, RERINKU_STREAM_STATE_CLOSED, "closed by the server");
        }
        return;
    }
    if (rk_is_rtcp(pkt->data, pkt->len) && p->srtp_in) {
        int n = pkt->len;
        if (srtp_unprotect_rtcp(p->srtp_in, pkt->data, &n) != srtp_err_status_ok)
            return;
        rk_rtcp_handlers h = {on_nack, on_rtcp_keyframe, p};
        rk_rtcp_parse(pkt->data, (size_t)n, &h);
    }
}

static void send_sender_reports(rk_peer *p, uint64_t now_ms) {
    if (now_ms < p->next_sr_ms)
        return;
    p->next_sr_ms = now_ms + SR_INTERVAL_MS;
    uint64_t now_us = rk_now_us();
    uint64_t ntp = rk_ntp_from_unix_us(rk_wall_us());
    for (size_t i = 0; i < p->track_count; ++i) {
        track *t = &p->tracks[i];
        if (!t->packets)
            continue;
        /* The RTP time "now": the last packet's timestamp moved forward by
         * the wall time since it was sent. */
        uint32_t ts = t->last_rtp_ts +
                      (uint32_t)((now_us - t->last_send_us) * t->rtp.clock_rate / 1000000u);
        uint8_t buf[128];
        size_t n = rk_rtcp_sender_report(buf, sizeof(buf), t->rtp.ssrc, ntp, ts, t->packets,
                                         t->octets, t->cname);
        if (n)
            transmit(p, buf, n, true);
    }
}

/* Send queued media. Audio goes out at once (one small packet per frame and
 * latency-sensitive); video through the token bucket. Returns the time the
 * next video packet may go, or 0 when the queue is empty. Called with mu held;
 * drops it around the actual sends. */
static uint64_t send_media(rk_peer *p) {
    uint64_t now_us = rk_now_us();
    if (p->last_refill_us == 0)
        p->last_refill_us = now_us;
    p->tokens += (double)(now_us - p->last_refill_us) * PACE_BYTES_PER_SEC / 1e6;
    if (p->tokens > PACE_BURST_BYTES)
        p->tokens = PACE_BURST_BYTES;
    p->last_refill_us = now_us;

    for (;;) {
        rk_pkt *pkt = q_pop(&p->audio_q);
        if (!pkt) {
            if (!p->video_q.head || p->tokens < p->video_q.head->len)
                break;
            pkt = q_pop(&p->video_q);
            p->tokens -= pkt->len;
        }
        pthread_mutex_unlock(&p->mu);
        send_rtp(p, pkt);
        pthread_mutex_lock(&p->mu);
    }
    if (!p->video_q.head)
        return 0;
    double missing = p->video_q.head->len - p->tokens;
    uint64_t wait_us = (uint64_t)(missing * 1e6 / PACE_BYTES_PER_SEC) + 1;
    return rk_now_ms() + (wait_us + 999) / 1000;
}

static void deliver_events(rk_peer *p) {
    for (size_t i = 0; i < p->event_count; ++i) {
        pthread_mutex_lock(&p->mu);
        bool closing = p->closing;
        pthread_mutex_unlock(&p->mu);
        if (closing)
            break; /* a torn-down session reports nothing */
        const event *e = &p->events[i];
        if (e->type == EV_STATE && p->on_state)
            p->on_state(p->user, (rerinku_stream_state_t)e->value);
        else if (e->type == EV_KEYFRAME && p->on_keyframe)
            p->on_keyframe(p->user, e->value);
    }
    p->event_count = 0;
}

static void *peer_thread(void *arg) {
    rk_peer *p = arg;
    push_event(p, EV_STATE, RERINKU_STREAM_STATE_CONNECTING);

    pthread_mutex_lock(&p->mu);
    while (!p->closing) {
        /* ICE progress. */
        if (p->ice_changed) {
            p->ice_changed = false;
            juice_state_t s = p->ice_state;
            pthread_mutex_unlock(&p->mu);
            if (s == JUICE_STATE_FAILED) {
                fail(p, p->ready ? "connection lost (ICE)" : "ICE connectivity failed");
            } else if ((s == JUICE_STATE_CONNECTED || s == JUICE_STATE_COMPLETED) &&
                       !p->dtls_started && !p->dtls_server && !p->failed) {
                p->dtls_started = true; /* we are the DTLS client: say hello */
                step_dtls(p);
            }
            pthread_mutex_lock(&p->mu);
        }

        /* Incoming DTLS and RTCP. */
        rk_pkt *in;
        while ((in = q_pop(&p->incoming)) != NULL && !p->closing) {
            pthread_mutex_unlock(&p->mu);
            if (!p->failed)
                handle_datagram(p, in);
            free(in);
            pthread_mutex_lock(&p->mu);
        }

        uint64_t now = rk_now_ms();
        uint64_t wake = now + 1000;
        bool failed = p->failed;
        if (!failed && !p->ready) {
            uint64_t t = p->dtls && p->dtls_started ? rk_dtls_timer_deadline(p->dtls) : 0;
            if (t && now >= t) {
                pthread_mutex_unlock(&p->mu);
                step_dtls(p); /* retransmission */
                pthread_mutex_lock(&p->mu);
                t = rk_dtls_timer_deadline(p->dtls);
            }
            if (t)
                wake = RK_MIN(wake, t);
            if (now >= p->connect_deadline && !p->failed) {
                pthread_mutex_unlock(&p->mu);
                fail(p, "connection timed out");
                pthread_mutex_lock(&p->mu);
            }
            wake = RK_MIN(wake, p->connect_deadline);
        }
        if (p->ready && !p->failed) {
            uint64_t next = send_media(p);
            if (next)
                wake = RK_MIN(wake, next);
            pthread_mutex_unlock(&p->mu);
            send_sender_reports(p, rk_now_ms());
            pthread_mutex_lock(&p->mu);
            wake = RK_MIN(wake, p->next_sr_ms);
        }
        /* Keyframes the encoder must produce (queue overflow on send). */
        for (size_t i = 0; i < p->track_count; ++i)
            if (p->tracks[i].keyframe_wanted) {
                p->tracks[i].keyframe_wanted = false;
                push_event(p, EV_KEYFRAME, p->tracks[i].channel_id);
            }

        if (p->event_count) {
            pthread_mutex_unlock(&p->mu);
            deliver_events(p);
            pthread_mutex_lock(&p->mu);
            continue; /* state may have moved while unlocked */
        }
        if (p->closing || p->incoming.head || p->ice_changed)
            continue;
        if (p->ready && !p->failed && p->audio_q.head)
            continue;
        rk_cond_wait_until(&p->cv, &p->mu, wake);
    }
    bool ready = p->ready;
    pthread_mutex_unlock(&p->mu);
    if (ready && p->dtls)
        rk_dtls_close(p->dtls); /* tell the server we are leaving */
    rk_peer_release(p);
    return NULL;
}

/* ---- media path (encoder threads) ------------------------------------------------ */

typedef struct emit_ctx {
    pkt_queue local;
    uint8_t track;
    bool oom;
} emit_ctx;

static bool emit_to_queue(void *ctx, const uint8_t *packet, size_t len) {
    emit_ctx *e = ctx;
    rk_pkt *pkt = malloc(sizeof(rk_pkt) + len);
    if (!pkt) {
        e->oom = true;
        return false;
    }
    memcpy(pkt->data, packet, len);
    pkt->len = (uint16_t)len;
    pkt->seq = rk_rtp_seq(packet);
    pkt->track = e->track;
    q_push(&e->local, pkt);
    return true;
}

rerinku_write_result_t rk_peer_send(rk_peer *p, uint32_t channel_id, const uint8_t *data,
                                    size_t len, uint64_t pts_clock, bool keyframe) {
    track *t = track_by_channel(p, channel_id);
    if (!t)
        return RERINKU_WRITE_RESULT_UNKNOWN_CHANNEL;
    pthread_mutex_lock(&p->mu);
    if (p->closing || p->failed) {
        pthread_mutex_unlock(&p->mu);
        return RERINKU_WRITE_RESULT_NOT_STREAMING;
    }
    if (!p->ready) {
        pthread_mutex_unlock(&p->mu);
        return RERINKU_WRITE_RESULT_CONNECTING;
    }
    if (t->video && t->need_keyframe && !keyframe) {
        pthread_mutex_unlock(&p->mu);
        return RERINKU_WRITE_RESULT_WAITING_FOR_KEYFRAME;
    }
    pkt_queue *q = t->video ? &p->video_q : &p->audio_q;
    if (p->video_q.bytes + p->audio_q.bytes > MAX_QUEUED_BYTES) {
        /* The link cannot keep up. Skip ahead to the next keyframe instead of
         * building seconds of latency (a partial picture would decode as
         * garbage anyway). */
        if (t->video) {
            t->need_keyframe = true;
            t->keyframe_wanted = true;
            pthread_cond_broadcast(&p->cv);
        }
        pthread_mutex_unlock(&p->mu);
        return RERINKU_WRITE_RESULT_DROPPED;
    }
    if (!t->have_base) {
        t->base_pts = pts_clock;
        t->have_base = true;
    }
    /* First frame anchors the timeline; uint32 truncation is RTP's mod-2^32
     * timestamp arithmetic. */
    uint32_t ts = t->rtp.ts_offset + (uint32_t)(pts_clock - t->base_pts);
    emit_ctx e = {.track = (uint8_t)(t - p->tracks)};
    uint16_t seq_before = t->rtp.seq;
    bool ok = rk_rtp_packetize(&t->rtp, t->codec, data, len, ts, emit_to_queue, &e);
    if (!ok) {
        t->rtp.seq = seq_before; /* nothing went out: no gap in the numbering */
        pthread_mutex_unlock(&p->mu);
        q_free(&e.local);
        return e.oom ? RERINKU_WRITE_RESULT_DROPPED : RERINKU_WRITE_RESULT_INVALID_FRAME;
    }
    rk_pkt *pkt;
    while ((pkt = q_pop(&e.local)) != NULL)
        q_push(q, pkt);
    if (keyframe)
        t->need_keyframe = false;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    return RERINKU_WRITE_RESULT_SENT;
}

void rk_peer_restart_at_keyframe(rk_peer *p, uint32_t channel_id) {
    track *t = track_by_channel(p, channel_id);
    if (!t || !t->video)
        return;
    pthread_mutex_lock(&p->mu);
    t->need_keyframe = true;
    t->keyframe_wanted = true;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
}

bool rk_peer_needs_keyframe(rk_peer *p, uint32_t channel_id) {
    track *t = track_by_channel(p, channel_id);
    if (!t)
        return false;
    pthread_mutex_lock(&p->mu);
    bool needs = t->video && t->need_keyframe && p->ready && !p->failed;
    pthread_mutex_unlock(&p->mu);
    return needs;
}

bool rk_peer_is_connected(rk_peer *p) {
    pthread_mutex_lock(&p->mu);
    bool c = p->ready && !p->failed && !p->closing;
    pthread_mutex_unlock(&p->mu);
    return c;
}

/* ---- teardown ---------------------------------------------------------------------- */

void rk_peer_close(rk_peer *p) {
    pthread_mutex_lock(&p->mu);
    if (p->closing) {
        pthread_mutex_unlock(&p->mu);
        return;
    }
    p->closing = true;
    bool started = p->thread_started;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    if (started)
        pthread_join(p->thread, NULL);
    /* No more ICE callbacks after this returns. */
    if (p->agent) {
        juice_destroy(p->agent);
        p->agent = NULL;
    }
}

void rk_peer_retain(rk_peer *p) { atomic_fetch_add(&p->refs, 1); }

void rk_peer_release(rk_peer *p) {
    if (!p || atomic_fetch_sub(&p->refs, 1) != 1)
        return;
    /* Last reference: the thread (which holds one) has exited. */
    if (p->agent)
        juice_destroy(p->agent);
    rk_dtls_destroy(p->dtls);
    if (p->srtp_out)
        srtp_dealloc(p->srtp_out);
    if (p->srtp_in)
        srtp_dealloc(p->srtp_in);
    q_free(&p->incoming);
    q_free(&p->audio_q);
    q_free(&p->video_q);
    for (size_t i = 0; p->tracks && i < p->track_count; ++i) {
        track *t = &p->tracks[i];
        for (size_t j = 0; j < t->history_size; ++j)
            free(t->history[j]);
        free(t->history);
        free(t->fmtp);
        free(t->stream_id);
    }
    free(p->tracks);
    free(p->stun_host);
    free(p->turn_host);
    free(p->turn_user);
    free(p->turn_pass);
    free(p->label);
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv);
    free(p);
}
