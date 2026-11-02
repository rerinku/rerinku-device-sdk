/* Access-unit normalization: every accepted layout becomes canonical Annex-B,
 * parameter sets are repaired in front of keyframes that lack them, and
 * malformed input is rejected before it can reach RTP/SRTP. */
#include "bitstream.h"
#include "rerinku/rerinku_device.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* NAL types of canonical Annex-B (4-byte start codes only). */
static size_t nal_types(int codec, const uint8_t *d, size_t len, uint8_t *types, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < len;) {
        assert(i + 5 <= len);
        assert(d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1);
        size_t nal = i + 4;
        assert(n < cap);
        types[n++] = codec == RERINKU_CODEC_H264 ? d[nal] & 0x1f : (d[nal] >> 1) & 0x3f;
        size_t next = nal + 1;
        while (next + 4 <= len && !(d[next] == 0 && d[next + 1] == 0 && d[next + 2] == 0 &&
                                    d[next + 3] == 1))
            ++next;
        i = next + 4 <= len ? next : len;
    }
    return n;
}

static void expect(int codec, const rk_vnorm_out *out, const uint8_t *expected, size_t count) {
    uint8_t types[16];
    size_t n = nal_types(codec, out->data, out->len, types, 16);
    assert(n == count);
    assert(memcmp(types, expected, count) == 0);
}

#define EXPECT(codec, out, ...)                                                  \
    do {                                                                         \
        const uint8_t e[] = {__VA_ARGS__};                                       \
        expect(codec, out, e, sizeof(e));                                        \
    } while (0)

int main(void) {
    rk_vnorm h264 = {0};
    rk_vnorm_out out;

    /* FFmpeg-style mixed Annex-B: long AUD/SPS/PPS and short SEI/IDR. */
    const uint8_t mixed[] = {0, 0, 0, 1, 0x09, 0xf0, 0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1f,
                             0, 0, 0, 1, 0x68, 0xce, 0x06, 0, 0, 1, 0x06, 0x05,
                             0, 0, 1, 0x65, 0x88};
    assert(rk_vnorm_normalize(&h264, RERINKU_CODEC_H264, RERINKU_BITSTREAM_FORMAT_AUTO, mixed,
                              sizeof(mixed), &out));
    assert(out.owned && out.has_vcl && out.has_keyframe);
    EXPECT(RERINKU_CODEC_H264, &out, 9, 7, 8, 6, 5);
    free(out.owned);

    /* UVC devices seen in the wild prepend zero padding. */
    const uint8_t padded[] = {0, 0, 0, 0, 0, 0, 1, 0x41, 0x77, 0, 0, 0, 1};
    assert(rk_vnorm_normalize(&h264, RERINKU_CODEC_H264, RERINKU_BITSTREAM_FORMAT_AUTO, padded,
                              sizeof(padded), &out));
    EXPECT(RERINKU_CODEC_H264, &out, 1);
    assert(out.has_vcl && !out.has_keyframe);
    free(out.owned);

    /* Four-byte AVCC, two-byte lengths, then a raw NAL: Auto is evaluated per
     * sample, not locked to the first layout seen. */
    const uint8_t avcc[] = {0, 0, 0, 2, 0x41, 0x11, 0, 0, 0, 2, 0x41, 0x22};
    assert(rk_vnorm_normalize(&h264, RERINKU_CODEC_H264, RERINKU_BITSTREAM_FORMAT_AUTO, avcc,
                              sizeof(avcc), &out));
    EXPECT(RERINKU_CODEC_H264, &out, 1, 1);
    free(out.owned);
    const uint8_t two_byte[] = {0, 2, 0x41, 0x33, 0, 2, 0x41, 0x44};
    assert(rk_vnorm_normalize(&h264, RERINKU_CODEC_H264, RERINKU_BITSTREAM_FORMAT_AUTO, two_byte,
                              sizeof(two_byte), &out));
    EXPECT(RERINKU_CODEC_H264, &out, 1, 1);
    free(out.owned);

    /* SPS/PPS cached from the mixed sample are restored in front of a later
     * keyframe that omits them. */
    const uint8_t raw_idr[] = {0x65, 0x99};
    assert(rk_vnorm_normalize(&h264, RERINKU_CODEC_H264, RERINKU_BITSTREAM_FORMAT_AUTO, raw_idr,
                              sizeof(raw_idr), &out));
    EXPECT(RERINKU_CODEC_H264, &out, 7, 8, 5);
    free(out.owned);

    /* Canonical Annex-B passes through without a copy. */
    const uint8_t canonical[] = {0, 0, 0, 1, 0x41, 0x55};
    assert(rk_vnorm_normalize(&h264, RERINKU_CODEC_H264, RERINKU_BITSTREAM_FORMAT_ANNEX_B, canonical,
                              sizeof(canonical), &out));
    assert(!out.owned && out.data == canonical && out.len == sizeof(canonical));
    const uint8_t trailing[] = {0, 0, 0, 1, 0x41, 0x55, 0, 0, 0, 1};
    assert(rk_vnorm_normalize(&h264, RERINKU_CODEC_H264, RERINKU_BITSTREAM_FORMAT_AUTO, trailing,
                              sizeof(trailing), &out));
    assert(out.owned);
    EXPECT(RERINKU_CODEC_H264, &out, 1);
    free(out.owned);

    /* HEVC caches VPS/SPS/PPS and repairs an IDR_W_RADL raw NAL. */
    rk_vnorm h265 = {0};
    const uint8_t hevc_params[] = {0, 0, 0, 1, 0x40, 0x01, 0x0c, 0, 0, 1, 0x42, 0x01, 0x01,
                                   0, 0, 0, 1, 0x44, 0x01, 0xc0};
    assert(rk_vnorm_normalize(&h265, RERINKU_CODEC_H265, RERINKU_BITSTREAM_FORMAT_AUTO, hevc_params,
                              sizeof(hevc_params), &out));
    free(out.owned);
    const uint8_t hevc_idr[] = {0x26, 0x01, 0xaa};
    assert(rk_vnorm_normalize(&h265, RERINKU_CODEC_H265, RERINKU_BITSTREAM_FORMAT_RAW_NAL, hevc_idr,
                              sizeof(hevc_idr), &out));
    assert(out.has_keyframe);
    EXPECT(RERINKU_CODEC_H265, &out, 32, 33, 34, 19);
    free(out.owned);

    /* Explicit layouts reject truncation and RTP payload structures. */
    const uint8_t truncated[] = {0, 0, 0, 8, 0x65};
    assert(!rk_vnorm_normalize(&h264, RERINKU_CODEC_H264, RERINKU_BITSTREAM_FORMAT_LENGTH_PREFIXED,
                               truncated, sizeof(truncated), &out));
    const uint8_t rtp_fu_a[] = {0x7c, 0x85, 0x01};
    assert(!rk_vnorm_normalize(&h264, RERINKU_CODEC_H264, RERINKU_BITSTREAM_FORMAT_RAW_NAL, rtp_fu_a,
                               sizeof(rtp_fu_a), &out));
    assert(!rk_vnorm_normalize(&h264, RERINKU_CODEC_H264, RERINKU_BITSTREAM_FORMAT_AUTO, NULL, 0, &out));

    rk_vnorm_free(&h264);
    rk_vnorm_free(&h265);
    printf("bitstream ok\n");
    return 0;
}
