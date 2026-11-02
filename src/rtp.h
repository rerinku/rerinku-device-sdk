/* RTP packetization (RFC 3550) and the RTCP the sender needs: sender reports,
 * and NACK / PLI / FIR feedback parsing. */
#ifndef RK_RTP_H
#define RK_RTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Largest RTP packet we build (header + payload, before the SRTP tag). Keeps
 * IPv4/IPv6 + UDP + SRTP well under a 1280-byte path MTU. */
#define RK_RTP_MAX_PACKET 1200
#define RK_RTP_HEADER 12

typedef struct rk_rtp_stream {
    uint32_t ssrc;
    uint8_t payload_type;
    uint32_t clock_rate;
    uint16_t seq;
    uint32_t ts_offset; /* random RTP timestamp origin */
} rk_rtp_stream;

/* Receives each packet built; return false to abort the access unit. */
typedef bool (*rk_rtp_emit_fn)(void *ctx, const uint8_t *packet, size_t len);

/* Packetize one access unit (canonical Annex-B for H.264/H.265, one frame for
 * audio) stamped with rtp_ts. H.264 per RFC 6184 (single NAL / FU-A), H.265
 * per RFC 7798 (single NAL / FU); the marker bit ends a video access unit. */
bool rk_rtp_packetize(rk_rtp_stream *s, int codec, const uint8_t *data, size_t len,
                      uint32_t rtp_ts, rk_rtp_emit_fn emit, void *ctx);

uint16_t rk_rtp_seq(const uint8_t *packet);
uint32_t rk_rtp_ts(const uint8_t *packet);

/* RTCP sender report + SDES CNAME (one compound packet). Returns its length,
 * 0 if cap is too small. ntp is NTP-format time (32.32 fixed point). */
size_t rk_rtcp_sender_report(uint8_t *buf, size_t cap, uint32_t ssrc, uint64_t ntp,
                             uint32_t rtp_ts, uint32_t packets, uint32_t octets,
                             const char *cname);

/* Wall clock (µs since the Unix epoch) to NTP 32.32. */
uint64_t rk_ntp_from_unix_us(uint64_t unix_us);

typedef struct rk_rtcp_handlers {
    void (*on_nack)(void *ctx, uint32_t media_ssrc, uint16_t seq);
    void (*on_keyframe)(void *ctx, uint32_t media_ssrc); /* PLI or FIR */
    void *ctx;
} rk_rtcp_handlers;

/* Walk a (decrypted) compound RTCP packet and report feedback. */
void rk_rtcp_parse(const uint8_t *buf, size_t len, const rk_rtcp_handlers *h);

/* RFC 7983 demultiplexing of the first byte of a datagram. */
static inline bool rk_is_dtls(const uint8_t *p, size_t len) {
    return len > 0 && p[0] >= 20 && p[0] <= 63;
}
static inline bool rk_is_rtcp(const uint8_t *p, size_t len) {
    return len >= 2 && p[0] >= 128 && p[0] <= 191 && p[1] >= 192 && p[1] <= 223;
}

#endif /* RK_RTP_H */
