#include "rtp.h"

#include "bitstream.h"
#include "rerinku/rerinku_device.h"

#include <string.h>

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

uint16_t rk_rtp_seq(const uint8_t *packet) { return get16(packet + 2); }
uint32_t rk_rtp_ts(const uint8_t *packet) { return get32(packet + 4); }

static void write_header(rk_rtp_stream *s, uint8_t *p, uint32_t ts, bool marker) {
    p[0] = 0x80; /* V=2, no padding, no extension, no CSRC */
    p[1] = (uint8_t)((marker ? 0x80 : 0) | (s->payload_type & 0x7f));
    put16(p + 2, s->seq++);
    put32(p + 4, ts);
    put32(p + 8, s->ssrc);
}

/* One packet: header + `hdr` bytes of payload header + `len` bytes of data. */
static bool emit_packet(rk_rtp_stream *s, uint32_t ts, bool marker, const uint8_t *hdr,
                        size_t hdr_len, const uint8_t *data, size_t len, rk_rtp_emit_fn emit,
                        void *ctx) {
    uint8_t pkt[RK_RTP_MAX_PACKET];
    write_header(s, pkt, ts, marker);
    memcpy(pkt + RK_RTP_HEADER, hdr, hdr_len);
    memcpy(pkt + RK_RTP_HEADER + hdr_len, data, len);
    return emit(ctx, pkt, RK_RTP_HEADER + hdr_len + len);
}

/* Fragment one NAL (header bytes nal[0..nh-1], payload after them). The first
 * fragment carries S, the last E — decided by position so a short tail chunk
 * can never lose its End flag (a receiver would wait for it forever). */
static bool fragment_nal(rk_rtp_stream *s, int codec, const uint8_t *nal, size_t nal_len,
                         uint32_t ts, bool last_nal, rk_rtp_emit_fn emit, void *ctx) {
    uint8_t hdr[3];
    size_t hdr_len, nh;
    uint8_t fu_type;
    if (codec == RERINKU_CODEC_H264) {
        nh = 1;
        hdr[0] = (uint8_t)((nal[0] & 0xe0) | 28); /* FU indicator: NRI + type 28 */
        fu_type = nal[0] & 0x1f;
        hdr_len = 2;
    } else {
        nh = 2;
        hdr[0] = (uint8_t)((nal[0] & 0x81) | (49 << 1)); /* F + LayerId hi + type 49 */
        hdr[1] = nal[1];                                 /* LayerId lo + TID */
        fu_type = (nal[0] >> 1) & 0x3f;
        hdr_len = 3;
    }
    const uint8_t *payload = nal + nh;
    size_t remaining = nal_len - nh;
    const size_t chunk = RK_RTP_MAX_PACKET - RK_RTP_HEADER - hdr_len;
    bool first = true;
    while (remaining > 0) {
        size_t n = remaining > chunk ? chunk : remaining;
        bool last = n == remaining;
        uint8_t fu = fu_type;
        if (first)
            fu |= 0x80;
        if (last)
            fu |= 0x40;
        hdr[hdr_len - 1] = fu;
        if (!emit_packet(s, ts, last && last_nal, hdr, hdr_len, payload, n, emit, ctx))
            return false;
        payload += n;
        remaining -= n;
        first = false;
    }
    return true;
}

static bool skip_nal(int codec, const uint8_t *nal) {
    /* Access-unit delimiters and filler carry nothing a receiver needs. */
    if (codec == RERINKU_CODEC_H264) {
        uint8_t t = nal[0] & 0x1f;
        return t == 9 || t == 12;
    }
    uint8_t t = (nal[0] >> 1) & 0x3f;
    return t == 35 || t == 38;
}

static bool packetize_video(rk_rtp_stream *s, int codec, const uint8_t *data, size_t len,
                            uint32_t ts, rk_rtp_emit_fn emit, void *ctx) {
    const size_t min_header = codec == RERINKU_CODEC_H264 ? 1 : 2;
    /* Collect the NALs first so the last one can carry the marker bit. */
    size_t pos = 0, nal_len = 0;
    const uint8_t *nal, *prev = NULL;
    size_t prev_len = 0;
    while ((nal = rk_annexb_next_nal(data, len, &pos, &nal_len)) != NULL) {
        if (nal_len <= min_header || skip_nal(codec, nal))
            continue;
        if (prev) {
            bool ok = prev_len + RK_RTP_HEADER <= RK_RTP_MAX_PACKET
                          ? emit_packet(s, ts, false, NULL, 0, prev, prev_len, emit, ctx)
                          : fragment_nal(s, codec, prev, prev_len, ts, false, emit, ctx);
            if (!ok)
                return false;
        }
        prev = nal;
        prev_len = nal_len;
    }
    if (!prev)
        return false;
    return prev_len + RK_RTP_HEADER <= RK_RTP_MAX_PACKET
               ? emit_packet(s, ts, true, NULL, 0, prev, prev_len, emit, ctx)
               : fragment_nal(s, codec, prev, prev_len, ts, true, emit, ctx);
}

bool rk_rtp_packetize(rk_rtp_stream *s, int codec, const uint8_t *data, size_t len,
                      uint32_t rtp_ts, rk_rtp_emit_fn emit, void *ctx) {
    if (!data || len == 0)
        return false;
    if (codec == RERINKU_CODEC_H264 || codec == RERINKU_CODEC_H265)
        return packetize_video(s, codec, data, len, rtp_ts, emit, ctx);
    /* Audio: one frame per packet (G.711 frames are small; Opus packets are
     * at most a few hundred bytes). */
    if (len + RK_RTP_HEADER > RK_RTP_MAX_PACKET)
        return false;
    return emit_packet(s, rtp_ts, false, NULL, 0, data, len, emit, ctx);
}

uint64_t rk_ntp_from_unix_us(uint64_t unix_us) {
    const uint64_t epoch_offset = 2208988800ull; /* 1900 -> 1970 */
    uint64_t sec = unix_us / 1000000u + epoch_offset;
    uint64_t frac = ((unix_us % 1000000u) << 32) / 1000000u;
    return sec << 32 | frac;
}

size_t rk_rtcp_sender_report(uint8_t *buf, size_t cap, uint32_t ssrc, uint64_t ntp,
                             uint32_t rtp_ts, uint32_t packets, uint32_t octets,
                             const char *cname) {
    size_t cname_len = strlen(cname);
    if (cname_len > 255)
        cname_len = 255;
    /* SDES chunk: SSRC + item(type, len, text) + END, padded to 32 bits. */
    size_t sdes_body = 4 + 2 + cname_len + 1;
    size_t sdes_len = 4 + ((sdes_body + 3) & ~(size_t)3);
    size_t total = 28 + sdes_len;
    if (cap < total)
        return 0;
    memset(buf, 0, total);
    /* SR: V=2, RC=0, PT=200, length in 32-bit words minus one. */
    buf[0] = 0x80;
    buf[1] = 200;
    put16(buf + 2, 6);
    put32(buf + 4, ssrc);
    put32(buf + 8, (uint32_t)(ntp >> 32));
    put32(buf + 12, (uint32_t)ntp);
    put32(buf + 16, rtp_ts);
    put32(buf + 20, packets);
    put32(buf + 24, octets);
    /* SDES: V=2, SC=1, PT=202. */
    uint8_t *sd = buf + 28;
    sd[0] = 0x81;
    sd[1] = 202;
    put16(sd + 2, (uint16_t)(sdes_len / 4 - 1));
    put32(sd + 4, ssrc);
    sd[8] = 1; /* CNAME */
    sd[9] = (uint8_t)cname_len;
    memcpy(sd + 10, cname, cname_len);
    /* The END item and padding are already zero. */
    return total;
}

void rk_rtcp_parse(const uint8_t *buf, size_t len, const rk_rtcp_handlers *h) {
    size_t off = 0;
    while (off + 4 <= len) {
        const uint8_t *p = buf + off;
        if ((p[0] >> 6) != 2)
            return;
        uint8_t fmt = p[0] & 0x1f;
        uint8_t pt = p[1];
        size_t plen = ((size_t)get16(p + 2) + 1) * 4;
        if (off + plen > len)
            return;
        if (plen >= 12) {
            uint32_t media_ssrc = get32(p + 8);
            if (pt == 205 && fmt == 1 && h->on_nack) {
                /* Generic NACK: (PID, BLP) pairs after the two SSRCs. */
                for (size_t i = 12; i + 4 <= plen; i += 4) {
                    uint16_t pid = get16(p + i);
                    uint16_t blp = get16(p + i + 2);
                    h->on_nack(h->ctx, media_ssrc, pid);
                    for (int b = 0; b < 16; ++b)
                        if (blp & (1u << b))
                            h->on_nack(h->ctx, media_ssrc, (uint16_t)(pid + b + 1));
                }
            } else if (pt == 206 && fmt == 1 && h->on_keyframe) {
                h->on_keyframe(h->ctx, media_ssrc); /* PLI */
            } else if (pt == 206 && fmt == 4 && h->on_keyframe && plen >= 20) {
                /* FIR: the target SSRC is in the FCI entry, not the header. */
                h->on_keyframe(h->ctx, get32(p + 12));
            }
        }
        off += plen;
    }
}
