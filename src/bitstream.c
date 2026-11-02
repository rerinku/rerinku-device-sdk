#include "bitstream.h"

#include "rerinku/rerinku_device.h"

#include <stdlib.h>
#include <string.h>

#define MAX_NALS_PER_AU 4096

typedef struct nal_span {
    size_t offset;
    size_t size;
    uint8_t type;
} nal_span;

typedef struct parsed_au {
    nal_span *nals;
    size_t count;
    size_t cap;
    bool canonical_annexb;
} parsed_au;

static void parsed_reset(parsed_au *p) {
    p->count = 0;
    p->canonical_annexb = false;
}

static bool find_start_code(const uint8_t *d, size_t len, size_t from, size_t *pos,
                            size_t *prefix) {
    for (size_t i = from; i + 3 <= len; ++i) {
        if (d[i] != 0 || d[i + 1] != 0)
            continue;
        if (d[i + 2] == 1) {
            *pos = i;
            *prefix = 3;
            return true;
        }
        if (i + 4 <= len && d[i + 2] == 0 && d[i + 3] == 1) {
            *pos = i;
            *prefix = 4;
            return true;
        }
    }
    return false;
}

static bool nal_type(int codec, const uint8_t *d, size_t len, uint8_t *type) {
    if (codec == RERINKU_CODEC_H264) {
        if (len < 1)
            return false;
        *type = d[0] & 0x1f;
        /* Types 24+ are RTP aggregation/fragmentation payloads, not raw
         * elementary-stream NAL units. Type zero is unspecified. */
        return (d[0] & 0x80) == 0 && *type >= 1 && *type <= 23;
    }
    if (len < 2)
        return false;
    *type = (d[0] >> 1) & 0x3f;
    /* TID+1 must not be zero. Types 48+ are RTP payload structures. */
    return (d[0] & 0x80) == 0 && (d[1] & 0x07) != 0 && *type <= 47;
}

static bool append_nal(int codec, const uint8_t *d, size_t offset, size_t size, parsed_au *p) {
    if (size == 0)
        return true; /* tolerate duplicate/trailing separators */
    if (p->count >= MAX_NALS_PER_AU)
        return false;
    uint8_t type = 0;
    if (!nal_type(codec, d + offset, size, &type))
        return false;
    if (p->count == p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 16;
        nal_span *n = realloc(p->nals, cap * sizeof(*n));
        if (!n)
            return false;
        p->nals = n;
        p->cap = cap;
    }
    p->nals[p->count++] = (nal_span){offset, size, type};
    return true;
}

static bool parse_annexb(int codec, const uint8_t *d, size_t len, parsed_au *p) {
    size_t start = 0, prefix = 0;
    if (!find_start_code(d, len, 0, &start, &prefix))
        return false;
    /* Leading zero padding is legal; other bytes are not silently discarded
     * because that would hide a mismatched format. */
    for (size_t i = 0; i < start; ++i)
        if (d[i] != 0)
            return false;
    p->canonical_annexb = start == 0 && prefix == 4;
    for (;;) {
        size_t nal_start = start + prefix;
        size_t next = 0, next_prefix = 0;
        bool has_next = find_start_code(d, len, nal_start, &next, &next_prefix);
        size_t nal_end = has_next ? next : len;
        if (nal_end == nal_start)
            p->canonical_annexb = false;
        if (!append_nal(codec, d, nal_start, nal_end - nal_start, p))
            return false;
        if (!has_next)
            break;
        if (next_prefix != 4)
            p->canonical_annexb = false;
        start = next;
        prefix = next_prefix;
    }
    return p->count > 0;
}

static bool parse_length_prefixed(int codec, const uint8_t *d, size_t len, size_t width,
                                  parsed_au *p) {
    size_t offset = 0;
    while (offset < len) {
        if (len - offset < width)
            return false;
        uint32_t size = 0;
        for (size_t i = 0; i < width; ++i)
            size = (size << 8) | d[offset + i];
        offset += width;
        if (size == 0 || size > len - offset)
            return false;
        if (!append_nal(codec, d, offset, size, p))
            return false;
        offset += size;
    }
    return p->count > 0;
}

static bool parse_raw_nal(int codec, const uint8_t *d, size_t len, parsed_au *p) {
    return append_nal(codec, d, 0, len, p) && p->count == 1;
}

static bool parse_any_length(int codec, const uint8_t *d, size_t len, parsed_au *p) {
    static const size_t widths[] = {4, 2, 1};
    for (size_t i = 0; i < 3; ++i) {
        parsed_reset(p);
        if (parse_length_prefixed(codec, d, len, widths[i], p))
            return true;
    }
    return false;
}

static bool parse_access_unit(int codec, int format, const uint8_t *d, size_t len,
                              parsed_au *p) {
    if (!d || len == 0 || (codec != RERINKU_CODEC_H264 && codec != RERINKU_CODEC_H265))
        return false;
    switch (format) {
    case RERINKU_BITSTREAM_FORMAT_ANNEX_B:
        return parse_annexb(codec, d, len, p);
    case RERINKU_BITSTREAM_FORMAT_RAW_NAL:
        return parse_raw_nal(codec, d, len, p);
    case RERINKU_BITSTREAM_FORMAT_LENGTH_PREFIXED:
        return parse_any_length(codec, d, len, p);
    case RERINKU_BITSTREAM_FORMAT_AUTO:
        /* Detection runs per access unit, so a source may change layout after
         * a reconnect without rebuilding the publisher. */
        if (parse_annexb(codec, d, len, p))
            return true;
        if (parse_any_length(codec, d, len, p))
            return true;
        parsed_reset(p);
        return parse_raw_nal(codec, d, len, p);
    default:
        return false;
    }
}

static bool is_vcl(int codec, uint8_t type) {
    return codec == RERINKU_CODEC_H264 ? type >= 1 && type <= 5 : type <= 31;
}

static bool is_keyframe(int codec, uint8_t type) {
    return codec == RERINKU_CODEC_H264 ? type == 5 : type >= 16 && type <= 21;
}

static int param_set_index(int codec, uint8_t type) {
    if (codec == RERINKU_CODEC_H264)
        return type == 7 ? 1 : type == 8 ? 2 : -1;
    return type >= 32 && type <= 34 ? (int)type - 32 : -1;
}

static uint8_t *put_canonical(uint8_t *dst, const uint8_t *nal, size_t len) {
    dst[0] = 0;
    dst[1] = 0;
    dst[2] = 0;
    dst[3] = 1;
    memcpy(dst + 4, nal, len);
    return dst + 4 + len;
}

bool rk_vnorm_normalize(rk_vnorm *n, int codec, int format, const uint8_t *in, size_t len,
                        rk_vnorm_out *out) {
    memset(out, 0, sizeof(*out));
    parsed_au p = {0};
    if (!parse_access_unit(codec, format, in, len, &p)) {
        free(p.nals);
        return false;
    }

    bool present[3] = {false, false, false};
    for (size_t i = 0; i < p.count; ++i) {
        const nal_span *nal = &p.nals[i];
        out->has_vcl = out->has_vcl || is_vcl(codec, nal->type);
        out->has_keyframe = out->has_keyframe || is_keyframe(codec, nal->type);
        int idx = param_set_index(codec, nal->type);
        if (idx >= 0) {
            present[idx] = true;
            uint8_t *copy = malloc(nal->size);
            if (copy) {
                memcpy(copy, in + nal->offset, nal->size);
                free(n->param_sets[idx]);
                n->param_sets[idx] = copy;
                n->param_len[idx] = nal->size;
            }
        }
    }

    const int first = codec == RERINKU_CODEC_H264 ? 1 : 0;
    bool complete_cache = true, complete_in_sample = true;
    for (int i = first; i < 3; ++i) {
        complete_cache = complete_cache && n->param_len[i] > 0;
        complete_in_sample = complete_in_sample && present[i];
    }
    const bool inject = out->has_keyframe && complete_cache && !complete_in_sample;

    if (p.canonical_annexb && !inject) {
        out->data = in;
        out->len = len;
        free(p.nals);
        return true;
    }

    size_t need = 0;
    if (inject)
        for (int i = first; i < 3; ++i)
            need += 4 + n->param_len[i];
    for (size_t i = 0; i < p.count; ++i)
        need += 4 + p.nals[i].size;
    uint8_t *buf = malloc(need);
    if (!buf) {
        free(p.nals);
        return false;
    }
    uint8_t *w = buf;
    if (inject)
        for (int i = first; i < 3; ++i)
            w = put_canonical(w, n->param_sets[i], n->param_len[i]);
    for (size_t i = 0; i < p.count; ++i)
        w = put_canonical(w, in + p.nals[i].offset, p.nals[i].size);
    free(p.nals);
    out->owned = buf;
    out->data = buf;
    out->len = (size_t)(w - buf);
    return out->len > 0;
}

void rk_vnorm_free(rk_vnorm *n) {
    for (int i = 0; i < 3; ++i) {
        free(n->param_sets[i]);
        n->param_sets[i] = NULL;
        n->param_len[i] = 0;
    }
}

const uint8_t *rk_annexb_next_nal(const uint8_t *data, size_t len, size_t *pos,
                                  size_t *nal_len) {
    size_t sc = 0, prefix = 0;
    while (*pos < len) {
        if (!find_start_code(data, len, *pos, &sc, &prefix))
            return NULL;
        size_t start = sc + prefix;
        size_t next = 0, next_prefix = 0;
        size_t end = find_start_code(data, len, start, &next, &next_prefix) ? next : len;
        *pos = end;
        if (end > start) {
            *nal_len = end - start;
            return data + start;
        }
    }
    return NULL;
}

void rk_scan_access_unit(int codec, const uint8_t *data, size_t len, bool *has_vcl,
                         bool *has_keyframe) {
    *has_vcl = false;
    *has_keyframe = false;
    if (!data || len == 0)
        return;
    size_t sc = 0, prefix = 0;
    if (!find_start_code(data, len, 0, &sc, &prefix)) {
        /* Not Annex-B: judge by the first NAL header (raw NAL / length
         * prefixed samples are normalized later anyway). */
        uint8_t type;
        if (nal_type(codec, data, len, &type)) {
            *has_vcl = is_vcl(codec, type);
            *has_keyframe = is_keyframe(codec, type);
        }
        return;
    }
    size_t pos = 0, nal_len = 0;
    const uint8_t *nal;
    while ((nal = rk_annexb_next_nal(data, len, &pos, &nal_len)) != NULL) {
        uint8_t type;
        if (!nal_type(codec, nal, nal_len, &type))
            continue;
        if (is_vcl(codec, type))
            *has_vcl = true;
        if (is_keyframe(codec, type))
            *has_keyframe = true;
    }
}
