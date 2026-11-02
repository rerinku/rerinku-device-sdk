/* H.264/H.265 access-unit handling: the compatibility boundary between
 * whatever the encoder emits and the RTP packetizer.
 *
 * rk_vnorm_normalize() accepts Annex-B (3/4-byte or mixed start codes, zero
 * padding), 1/2/4-byte length-prefixed samples, or one raw NAL, validates the
 * NAL headers and produces canonical four-byte Annex-B. It remembers the
 * parameter sets (H.264 SPS/PPS, H.265 VPS/SPS/PPS) and restores them before a
 * keyframe that omits them, so a fresh receiver always gets a decodable random
 * access point. Canonical input without missing parameter sets passes through
 * without a copy. */
#ifndef RK_BITSTREAM_H
#define RK_BITSTREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct rk_vnorm {
    uint8_t *param_sets[3]; /* H.264: [1]=SPS [2]=PPS; H.265: VPS, SPS, PPS */
    size_t param_len[3];
} rk_vnorm;

typedef struct rk_vnorm_out {
    const uint8_t *data; /* canonical Annex-B: input (passthrough) or owned */
    size_t len;
    uint8_t *owned;      /* free() after use; NULL on passthrough */
    bool has_vcl;
    bool has_keyframe;
} rk_vnorm_out;

/* codec: rerinku_codec_t (H264/H265). format: rerinku_bitstream_format_t.
 * false = invalid input (nothing allocated). */
bool rk_vnorm_normalize(rk_vnorm *n, int codec, int format, const uint8_t *in, size_t len,
                        rk_vnorm_out *out);
void rk_vnorm_free(rk_vnorm *n);

/* Iterate the NAL units of canonical (or any) Annex-B: returns the next NAL
 * payload (without start code) at or after *pos, advancing *pos; NULL at end. */
const uint8_t *rk_annexb_next_nal(const uint8_t *data, size_t len, size_t *pos,
                                  size_t *nal_len);

/* Cheap scan of an encoder access unit (any supported layout). */
void rk_scan_access_unit(int codec, const uint8_t *data, size_t len, bool *has_vcl,
                         bool *has_keyframe);

#endif /* RK_BITSTREAM_H */
