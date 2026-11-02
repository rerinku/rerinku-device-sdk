/* SDP for a send-only WHIP publisher: build the offer, read the answer.
 * Only what a BUNDLEd, rtcp-muxed, non-trickle offer needs. */
#ifndef RK_SDP_H
#define RK_SDP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RK_SDP_MAX_MEDIA 8

typedef struct rk_sdp_track {
    char mid[12];
    int kind;  /* rerinku_media_kind_t */
    int codec; /* rerinku_codec_t */
    uint8_t payload_type;
    const char *fmtp; /* NULL = codec default */
    uint32_t ssrc;
    const char *cname;
    const char *stream_id; /* msid stream: groups tracks into one viewable stream */
    const char *track_id;  /* msid track */
} rk_sdp_track;

/* ice: the ICE attribute lines from libjuice (ufrag, pwd, candidates,
 * end-of-candidates, ice-options). Returns an allocated SDP or NULL. */
char *rk_sdp_build_offer(const rk_sdp_track *tracks, size_t count, const char *ice,
                         const char *fingerprint);

typedef struct rk_sdp_answer_media {
    char mid[12];
    bool rejected;        /* port 0 */
    int payload_type;     /* PT the answer kept for our codec, -1 if none */
} rk_sdp_answer_media;

typedef struct rk_sdp_answer {
    char fingerprint[128]; /* "sha-256 AB:..." */
    char setup[16];        /* active | passive | actpass */
    char *ice;             /* ICE lines for libjuice (owned) */
    rk_sdp_answer_media media[RK_SDP_MAX_MEDIA];
    size_t media_count;
} rk_sdp_answer;

/* tracks: what we offered, to find each codec's PT in the answer. */
bool rk_sdp_parse_answer(const char *sdp, const rk_sdp_track *tracks, size_t count,
                         rk_sdp_answer *out, char *err, size_t errlen);
void rk_sdp_answer_free(rk_sdp_answer *a);

/* Codec name and clock as they appear in a=rtpmap. */
const char *rk_sdp_codec_name(int codec);
uint32_t rk_sdp_clock_rate(int codec);

#endif /* RK_SDP_H */
