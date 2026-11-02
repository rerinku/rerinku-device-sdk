/* RTP packetization (single NAL, FU-A, H.265 FU, audio) and RTCP (sender
 * report layout, NACK/PLI/FIR parsing). */
#include "rerinku/rerinku_device.h"
#include "rtp.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PKTS 64

typedef struct capture {
    uint8_t pkt[MAX_PKTS][RK_RTP_MAX_PACKET];
    size_t len[MAX_PKTS];
    size_t count;
} capture;

static bool collect(void *ctx, const uint8_t *packet, size_t len) {
    capture *c = ctx;
    assert(c->count < MAX_PKTS);
    assert(len <= RK_RTP_MAX_PACKET);
    memcpy(c->pkt[c->count], packet, len);
    c->len[c->count++] = len;
    return true;
}

static bool marker(const capture *c, size_t i) { return (c->pkt[i][1] & 0x80) != 0; }

static void test_h264_single_nals(void) {
    /* Mixed start codes; the AUD is dropped, every other NAL its own packet,
     * the marker only on the access unit's last packet. */
    const uint8_t au[] = {0, 0, 0, 1, 0x09, 0xf0, 0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1f,
                          0, 0, 0, 1, 0x68, 0xce, 0x06, 0xe2, 0, 0, 1, 0x06, 0x05,
                          0, 0, 1, 0x65, 0x88, 0x84};
    rk_rtp_stream s = {.ssrc = 0x12345678, .payload_type = 96, .clock_rate = 90000, .seq = 65534};
    static capture c;
    memset(&c, 0, sizeof(c));
    assert(rk_rtp_packetize(&s, RERINKU_CODEC_H264, au, sizeof(au), 3000, collect, &c));
    const uint8_t types[] = {7, 8, 6, 5};
    assert(c.count == 4);
    for (size_t i = 0; i < c.count; ++i) {
        assert(c.pkt[i][0] == 0x80);
        assert((c.pkt[i][1] & 0x7f) == 96);
        assert(rk_rtp_ts(c.pkt[i]) == 3000);
        assert((c.pkt[i][RK_RTP_HEADER] & 0x1f) == types[i]);
        assert(marker(&c, i) == (i == 3));
    }
    /* Sequence numbers wrap. */
    assert(rk_rtp_seq(c.pkt[0]) == 65534 && rk_rtp_seq(c.pkt[1]) == 65535 &&
           rk_rtp_seq(c.pkt[2]) == 0 && s.seq == 2);
}

static void test_fragmentation(int codec) {
    /* One big NAL: FU fragments, S on the first, E on the last (by position),
     * and the payloads reassemble to the original NAL body. */
    const size_t body = 3000;
    const size_t nh = codec == RERINKU_CODEC_H264 ? 1 : 2;
    uint8_t *au = malloc(4 + nh + body);
    memset(au, 0, 4);
    au[3] = 1;
    if (codec == RERINKU_CODEC_H264) {
        au[4] = 0x65; /* NRI 3, IDR */
    } else {
        au[4] = 0x26; /* IDR_W_RADL */
        au[5] = 0x01;
    }
    for (size_t i = 0; i < body; ++i)
        au[4 + nh + i] = (uint8_t)(i * 7 + 1);
    rk_rtp_stream s = {.ssrc = 1, .payload_type = 98, .clock_rate = 90000};
    static capture c;
    memset(&c, 0, sizeof(c));
    assert(rk_rtp_packetize(&s, codec, au, 4 + nh + body, 0, collect, &c));
    assert(c.count == 3);
    const size_t hdr = codec == RERINKU_CODEC_H264 ? 2 : 3;
    uint8_t *joined = malloc(body);
    size_t pos = 0;
    for (size_t i = 0; i < c.count; ++i) {
        const uint8_t *p = c.pkt[i] + RK_RTP_HEADER;
        uint8_t fu = p[hdr - 1];
        if (codec == RERINKU_CODEC_H264) {
            assert((p[0] & 0x1f) == 28 && (p[0] & 0x60) == 0x60);
            assert((fu & 0x1f) == 5);
        } else {
            assert(((p[0] >> 1) & 0x3f) == 49 && p[1] == 0x01);
            assert((fu & 0x3f) == 19);
        }
        assert(((fu & 0x80) != 0) == (i == 0));
        assert(((fu & 0x40) != 0) == (i == c.count - 1));
        assert(marker(&c, i) == (i == c.count - 1));
        size_t n = c.len[i] - RK_RTP_HEADER - hdr;
        memcpy(joined + pos, p + hdr, n);
        pos += n;
    }
    assert(pos == body && memcmp(joined, au + 4 + nh, body) == 0);
    free(joined);
    free(au);
}

static void test_audio(void) {
    uint8_t frame[160];
    memset(frame, 0xd5, sizeof(frame));
    rk_rtp_stream s = {.ssrc = 7, .payload_type = 8, .clock_rate = 8000};
    static capture c;
    memset(&c, 0, sizeof(c));
    assert(rk_rtp_packetize(&s, RERINKU_CODEC_G711A, frame, sizeof(frame), 160, collect, &c));
    assert(c.count == 1 && c.len[0] == RK_RTP_HEADER + 160 && !marker(&c, 0));
    assert((c.pkt[0][1] & 0x7f) == 8);
}

typedef struct feedback {
    uint16_t nacks[64];
    size_t nack_count;
    uint32_t keyframe_ssrc[4];
    size_t keyframe_count;
} feedback;

static void on_nack(void *ctx, uint32_t ssrc, uint16_t seq) {
    feedback *f = ctx;
    assert(ssrc == 0xAABBCCDD);
    f->nacks[f->nack_count++] = seq;
}

static void on_keyframe(void *ctx, uint32_t ssrc) {
    feedback *f = ctx;
    f->keyframe_ssrc[f->keyframe_count++] = ssrc;
}

static void test_rtcp(void) {
    uint8_t sr[128];
    size_t n = rk_rtcp_sender_report(sr, sizeof(sr), 0x01020304, 0x1122334455667788ull, 9000,
                                     10, 1000, "rerinku-0");
    assert(n == 28 + 4 + 4 + 2 + 9 + 1 + 0); /* SR + SDES header/SSRC/item/end */
    assert(n % 4 == 0);
    assert(sr[0] == 0x80 && sr[1] == 200 && sr[3] == 6);
    assert(sr[28] == 0x81 && sr[29] == 202);
    assert(!memcmp(sr + 38, "rerinku-0", 9));
    assert(rk_rtcp_sender_report(sr, 20, 1, 0, 0, 0, 0, "x") == 0);

    /* NACK (PID 100, BLP bits 0 and 2 → 101, 103) + PLI + FIR in one compound. */
    const uint8_t fb[] = {
        0x81, 205, 0, 3, 0, 0, 0, 1, 0xAA, 0xBB, 0xCC, 0xDD, 0, 100, 0x00, 0x05,
        0x81, 206, 0, 2, 0, 0, 0, 1, 0xAA, 0xBB, 0xCC, 0xDD,
        0x84, 206, 0, 4, 0, 0, 0, 1, 0, 0, 0, 0, 0x0A, 0x0B, 0x0C, 0x0D, 1, 0, 0, 0,
    };
    feedback f = {0};
    rk_rtcp_handlers h = {on_nack, on_keyframe, &f};
    rk_rtcp_parse(fb, sizeof(fb), &h);
    assert(f.nack_count == 3 && f.nacks[0] == 100 && f.nacks[1] == 101 && f.nacks[2] == 103);
    assert(f.keyframe_count == 2 && f.keyframe_ssrc[0] == 0xAABBCCDD &&
           f.keyframe_ssrc[1] == 0x0A0B0C0D);
    /* Truncated input is ignored, not overread. */
    feedback g = {0};
    rk_rtcp_handlers h2 = {on_nack, on_keyframe, &g};
    rk_rtcp_parse(fb, 10, &h2);
    assert(g.nack_count == 0 && g.keyframe_count == 0);

    const uint8_t dtls[] = {22, 0xfe, 0xfd};
    const uint8_t rtcp[] = {0x81, 201};
    const uint8_t rtp[] = {0x80, 96};
    assert(rk_is_dtls(dtls, 3) && !rk_is_rtcp(dtls, 3));
    assert(rk_is_rtcp(rtcp, 2) && !rk_is_rtcp(rtp, 2));
    assert(rk_ntp_from_unix_us(0) >> 32 == 2208988800ull);
}

int main(void) {
    test_h264_single_nals();
    test_fragmentation(RERINKU_CODEC_H264);
    test_fragmentation(RERINKU_CODEC_H265);
    test_audio();
    test_rtcp();
    printf("rtp ok\n");
    return 0;
}
