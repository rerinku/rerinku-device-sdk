/* One send-only WebRTC session: ICE (libjuice) + DTLS-SRTP (mbedTLS, libsrtp)
 * + RTP/RTCP for N media tracks, negotiated as a single WHIP offer/answer.
 *
 * Threads: libjuice's poll thread only queues incoming datagrams; the
 * session's own thread owns DTLS and SRTP and does all sending (paced video,
 * immediate audio, NACK retransmissions, sender reports). rk_peer_send() runs
 * on the caller's (encoder) thread and only packetizes and queues: it never
 * touches the network and never blocks on it.
 *
 * Lifetime: reference counted. The creator holds one reference; anyone who
 * calls into the session from another thread takes their own. */
#ifndef RK_PEER_H
#define RK_PEER_H

#include "rerinku/rerinku_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct rk_peer rk_peer;

typedef struct rk_peer_track {
    uint32_t channel_id;
    rerinku_media_kind_t kind;
    rerinku_codec_t codec;
    const char *codec_parameters; /* fmtp override, NULL = default */
    const char *stream_id;        /* msid stream id */
} rk_peer_track;

typedef struct rk_peer_config {
    const rk_peer_track *tracks;
    size_t track_count;
    const char *stun_server;    /* "stun:host[:port]" or NULL */
    const char *turn_server;    /* "turn:user:pass@host[:port]" or NULL */
    size_t nack_history;        /* RTP packets kept per video track */
    const char *label;          /* for logs */

    /* Both run on the session thread without its lock held. */
    void (*on_state)(void *user, rerinku_stream_state_t state);
    void (*on_keyframe)(void *user, uint32_t channel_id);
    void *user;
} rk_peer_config;

rk_peer *rk_peer_create(const rk_peer_config *cfg, char *err, size_t errlen);
void rk_peer_retain(rk_peer *p);
void rk_peer_release(rk_peer *p);

/* Gather candidates (bounded by timeout_ms) and build the complete offer. */
char *rk_peer_create_offer(rk_peer *p, int timeout_ms, char *err, size_t errlen);

/* Apply the answer and start connecting (state goes CONNECTING, then
 * CONNECTED or FAILED). */
bool rk_peer_set_answer(rk_peer *p, const char *sdp, char *err, size_t errlen);

/* Queue one access unit (canonical Annex-B video, or an audio frame) stamped
 * on the codec clock. */
rerinku_write_result_t rk_peer_send(rk_peer *p, uint32_t channel_id, const uint8_t *data,
                                    size_t len, uint64_t pts_clock, bool keyframe);

/* A video channel whose session has not sent a keyframe yet. */
bool rk_peer_needs_keyframe(rk_peer *p, uint32_t channel_id);
/* The channel resumes after frames were withheld from it (its stream was not
 * wanted): drop inter frames until its next keyframe and report a keyframe
 * request from the session thread, as for a fresh session. */
void rk_peer_restart_at_keyframe(rk_peer *p, uint32_t channel_id);
bool rk_peer_is_connected(rk_peer *p);

/* Stop the session: no more callbacks, DTLS close_notify, thread joined. Safe
 * to call more than once; other references stay valid (sends are dropped). */
void rk_peer_close(rk_peer *p);

#endif /* RK_PEER_H */
