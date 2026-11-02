#include "sdp.h"

#include "common.h"
#include "rerinku/rerinku_device.h"

#include <ctype.h>
#include <stdio.h>
#include <strings.h>

const char *rk_sdp_codec_name(int codec) {
    switch (codec) {
    case RERINKU_CODEC_H264:  return "H264";
    case RERINKU_CODEC_H265:  return "H265";
    case RERINKU_CODEC_OPUS:  return "opus";
    case RERINKU_CODEC_G711U: return "PCMU";
    case RERINKU_CODEC_G711A: return "PCMA";
    default:                  return "?";
    }
}

uint32_t rk_sdp_clock_rate(int codec) {
    switch (codec) {
    case RERINKU_CODEC_H264:
    case RERINKU_CODEC_H265:  return 90000;
    case RERINKU_CODEC_OPUS:  return 48000;
    default:                  return 8000;
    }
}

static const char *default_fmtp(int codec) {
    switch (codec) {
    /* Constrained Baseline 3.1 is what every browser decodes; level asymmetry
     * lets the device send a higher level than it would receive. */
    case RERINKU_CODEC_H264:
        return "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f";
    /* Safari will not negotiate H.265 without parameters: Main profile, level
     * 3.1 ceiling, single RTP stream. */
    case RERINKU_CODEC_H265:
        return "profile-id=1;level-id=93;tx-mode=SRST";
    case RERINKU_CODEC_OPUS:
        return "minptime=10;useinbandfec=1;stereo=0";
    default:
        return NULL;
    }
}

/* Split libjuice's ICE description into per-section pieces. */
static void append_ice_lines(rk_buf *out, const char *ice, bool credentials, bool candidates) {
    const char *p = ice;
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        bool is_cred = !strncmp(p, "a=ice-ufrag:", 12) || !strncmp(p, "a=ice-pwd:", 10);
        bool is_cand = !strncmp(p, "a=candidate:", 12) || !strncmp(p, "a=end-of-candidates", 19);
        if ((credentials && is_cred) || (candidates && is_cand)) {
            rk_buf_append(out, p, len);
            rk_buf_append_str(out, "\r\n");
        }
        if (!eol)
            break;
        p = eol + 2;
    }
}

char *rk_sdp_build_offer(const rk_sdp_track *tracks, size_t count, const char *ice,
                         const char *fingerprint) {
    rk_buf b = {0};
    uint64_t session_id = ((uint64_t)rk_random_u32() << 31) | rk_random_u32();
    rk_buf_appendf(&b, "v=0\r\no=rerinku %llu 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n",
                   (unsigned long long)session_id);
    rk_buf_append_str(&b, "a=group:BUNDLE");
    for (size_t i = 0; i < count; ++i)
        rk_buf_appendf(&b, " %s", tracks[i].mid);
    rk_buf_append_str(&b, "\r\na=msid-semantic: WMS *\r\n");
    rk_buf_append_str(&b, "a=ice-options:ice2\r\n");

    for (size_t i = 0; i < count; ++i) {
        const rk_sdp_track *t = &tracks[i];
        const bool video = t->kind == RERINKU_MEDIA_KIND_VIDEO;
        rk_buf_appendf(&b, "m=%s 9 UDP/TLS/RTP/SAVPF %u\r\nc=IN IP4 0.0.0.0\r\n",
                       video ? "video" : "audio", t->payload_type);
        rk_buf_appendf(&b, "a=mid:%s\r\na=sendonly\r\na=rtcp-mux\r\na=rtcp-rsize\r\n", t->mid);
        append_ice_lines(&b, ice, true, false);
        /* One transport for the bundle: candidates go with the first section. */
        if (i == 0)
            append_ice_lines(&b, ice, false, true);
        rk_buf_appendf(&b, "a=fingerprint:%s\r\na=setup:actpass\r\n", fingerprint);

        const unsigned pt = t->payload_type;
        if (t->codec == RERINKU_CODEC_OPUS)
            rk_buf_appendf(&b, "a=rtpmap:%u opus/48000/2\r\n", pt);
        else
            rk_buf_appendf(&b, "a=rtpmap:%u %s/%u\r\n", pt, rk_sdp_codec_name(t->codec),
                           rk_sdp_clock_rate(t->codec));
        const char *fmtp = t->fmtp && *t->fmtp ? t->fmtp : default_fmtp(t->codec);
        if (fmtp)
            rk_buf_appendf(&b, "a=fmtp:%u %s\r\n", pt, fmtp);
        if (video)
            rk_buf_appendf(&b, "a=rtcp-fb:%u nack\r\na=rtcp-fb:%u nack pli\r\n"
                               "a=rtcp-fb:%u ccm fir\r\n", pt, pt, pt);
        rk_buf_appendf(&b, "a=msid:%s %s\r\n", t->stream_id, t->track_id);
        rk_buf_appendf(&b, "a=ssrc:%u cname:%s\r\na=ssrc:%u msid:%s %s\r\n", t->ssrc, t->cname,
                       t->ssrc, t->stream_id, t->track_id);
    }
    return rk_buf_steal_str(&b);
}

/* ---- answer ------------------------------------------------------------------ */

static void copy_value(char *dst, size_t cap, const char *v, size_t len) {
    while (len > 0 && isspace((unsigned char)v[len - 1]))
        --len;
    rk_strlcpy(dst, "", cap);
    if (len >= cap)
        len = cap - 1;
    memcpy(dst, v, len);
    dst[len] = '\0';
}

static const rk_sdp_track *track_for_mid(const rk_sdp_track *tracks, size_t count,
                                         const char *mid) {
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(tracks[i].mid, mid))
            return &tracks[i];
    return NULL;
}

static bool has_prefix(const char *line, size_t len, const char *prefix) {
    size_t n = strlen(prefix);
    return len >= n && !strncmp(line, prefix, n);
}

typedef struct answer_parser {
    rk_sdp_answer *out;
    const rk_sdp_track *tracks;
    size_t count;
    rk_buf ice;
    bool have_ufrag, have_pwd;
    int section; /* -1 = session level */
    /* a=rtpmap may come before a=mid within a section; remember the PTs. */
    struct {
        int pt;
        char name[16];
    } maps[32];
    size_t map_count;
} answer_parser;

static void add_ice_line(answer_parser *ap, const char *line, size_t len) {
    rk_buf_append(&ap->ice, line, len);
    rk_buf_append_str(&ap->ice, "\r\n");
}

/* Once a section knows its mid, find the PT the answer kept for our codec. */
static void resolve_payload_type(answer_parser *ap) {
    if (ap->section < 0 || ap->section >= RK_SDP_MAX_MEDIA)
        return;
    rk_sdp_answer_media *m = &ap->out->media[ap->section];
    const rk_sdp_track *t = m->mid[0] ? track_for_mid(ap->tracks, ap->count, m->mid) : NULL;
    if (!t || m->payload_type >= 0)
        return;
    for (size_t i = 0; i < ap->map_count; ++i)
        if (!strcasecmp(ap->maps[i].name, rk_sdp_codec_name(t->codec))) {
            m->payload_type = ap->maps[i].pt;
            return;
        }
}

static void parse_line(answer_parser *ap, const char *p, size_t len) {
    rk_sdp_answer *out = ap->out;
    if (len < 3 || p[1] != '=')
        return;
    if (p[0] == 'm') {
        ap->section++;
        ap->map_count = 0;
        if (ap->section < RK_SDP_MAX_MEDIA) {
            rk_sdp_answer_media *m = &out->media[ap->section];
            m->payload_type = -1;
            const char *port = memchr(p, ' ', len);
            m->rejected = port && atoi(port + 1) == 0;
            out->media_count = (size_t)ap->section + 1;
        }
        return;
    }
    if (has_prefix(p, len, "a=fingerprint:")) {
        if (!out->fingerprint[0])
            copy_value(out->fingerprint, sizeof(out->fingerprint), p + 14, len - 14);
    } else if (has_prefix(p, len, "a=setup:")) {
        if (!out->setup[0])
            copy_value(out->setup, sizeof(out->setup), p + 8, len - 8);
    } else if (has_prefix(p, len, "a=mid:")) {
        if (ap->section >= 0 && ap->section < RK_SDP_MAX_MEDIA)
            copy_value(out->media[ap->section].mid, sizeof(out->media[0].mid), p + 6, len - 6);
    } else if (has_prefix(p, len, "a=rtpmap:")) {
        const char *sp = memchr(p, ' ', len);
        if (sp && ap->section >= 0 && ap->map_count < RK_ARRAY_LEN(ap->maps)) {
            size_t nl = 0;
            while (sp + 1 + nl < p + len && sp[1 + nl] != '/' && nl + 1 < sizeof(ap->maps[0].name))
                ++nl;
            ap->maps[ap->map_count].pt = atoi(p + 9);
            memcpy(ap->maps[ap->map_count].name, sp + 1, nl);
            ap->maps[ap->map_count].name[nl] = '\0';
            ap->map_count++;
        }
    } else if (has_prefix(p, len, "a=ice-ufrag:")) {
        /* Repeated per section, identical under BUNDLE: keep the first. */
        if (!ap->have_ufrag)
            add_ice_line(ap, p, len);
        ap->have_ufrag = true;
    } else if (has_prefix(p, len, "a=ice-pwd:")) {
        if (!ap->have_pwd)
            add_ice_line(ap, p, len);
        ap->have_pwd = true;
    } else if (has_prefix(p, len, "a=candidate:") || has_prefix(p, len, "a=end-of-candidates")) {
        /* The bundle's transport: session level or the first section. */
        if (ap->section <= 0)
            add_ice_line(ap, p, len);
    } else if (has_prefix(p, len, "a=ice-options:")) {
        if (ap->section < 0)
            add_ice_line(ap, p, len);
    }
    resolve_payload_type(ap);
}

bool rk_sdp_parse_answer(const char *sdp, const rk_sdp_track *tracks, size_t count,
                         rk_sdp_answer *out, char *err, size_t errlen) {
    memset(out, 0, sizeof(*out));
    answer_parser ap = {.out = out, .tracks = tracks, .count = count, .section = -1};
    for (const char *p = sdp; *p;) {
        size_t len = strcspn(p, "\r\n");
        parse_line(&ap, p, len);
        p += len;
        while (*p == '\r' || *p == '\n')
            ++p;
    }
    if (!out->fingerprint[0] || !ap.have_ufrag || !ap.have_pwd) {
        snprintf(err, errlen, "SDP answer lacks %s",
                 out->fingerprint[0] ? "ICE credentials" : "a DTLS fingerprint");
        rk_buf_free(&ap.ice);
        return false;
    }
    out->ice = rk_buf_steal_str(&ap.ice);
    return out->ice != NULL;
}

void rk_sdp_answer_free(rk_sdp_answer *a) {
    free(a->ice);
    a->ice = NULL;
}
