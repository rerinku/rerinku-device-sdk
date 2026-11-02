#include "http.h"

#include "common.h"
#include "net.h"

#include <ctype.h>
#include <stdio.h>
#include <strings.h>

/* Every request is bounded end to end: a device must never hang on a
 * half-dead server, since the caller may be holding a lock. */
#define HTTP_TIMEOUT_MS 15000
/* Responses here are small (JSON, SDP). Refuse anything absurd instead of
 * growing a buffer without bound on a device with a few MB to spare. */
#define HTTP_MAX_RESPONSE (1u << 20)

static bool build_request(rk_buf *out, const rk_http_request *req, const rk_url *u) {
    const bool v6 = strchr(u->host, ':') != NULL;
    const char *default_port = u->tls ? "443" : "80";
    bool ok = rk_buf_appendf(out, "%s %s HTTP/1.1\r\nHost: %s%s%s", req->method, u->path,
                             v6 ? "[" : "", u->host, v6 ? "]" : "");
    /* Host carries the port unless it is the scheme default (RFC 9110 7.2);
     * reverse proxies route on it. */
    if (strcmp(u->port, default_port) != 0)
        ok = ok && rk_buf_appendf(out, ":%s", u->port);
    ok = ok && rk_buf_append_str(out, "\r\nConnection: close\r\nUser-Agent: rerinku-device\r\n");
    if (req->bearer && *req->bearer)
        ok = ok && rk_buf_appendf(out, "Authorization: Bearer %s\r\n", req->bearer);
    if (req->extra_header)
        ok = ok && rk_buf_appendf(out, "%s\r\n", req->extra_header);
    if (req->body_len > 0 && req->content_type)
        ok = ok && rk_buf_appendf(out, "Content-Type: %s\r\n", req->content_type);
    if (req->body_len > 0 || !strcmp(req->method, "POST"))
        ok = ok && rk_buf_appendf(out, "Content-Length: %zu\r\n", req->body_len);
    ok = ok && rk_buf_append_str(out, "\r\n");
    return ok;
}

/* Find "\r\n\r\n"; returns the offset of the body or 0. */
static size_t header_end(const char *raw, size_t len) {
    for (size_t i = 0; i + 3 < len; ++i)
        if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n')
            return i + 4;
    return 0;
}

/* Value of a header within the header block, or NULL (allocated copy). */
static char *header_value(const char *headers, size_t len, const char *name) {
    size_t nlen = strlen(name);
    const char *p = headers;
    const char *end = headers + len;
    while (p < end) {
        const char *eol = p;
        while (eol + 1 < end && !(eol[0] == '\r' && eol[1] == '\n'))
            ++eol;
        if ((size_t)(eol - p) > nlen && p[nlen] == ':' && strncasecmp(p, name, nlen) == 0) {
            const char *v = p + nlen + 1;
            while (v < eol && (*v == ' ' || *v == '\t'))
                ++v;
            const char *ve = eol;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t'))
                --ve;
            return rk_strndup(v, (size_t)(ve - v));
        }
        p = eol + 2;
    }
    return NULL;
}

static bool decode_chunked(const char *src, size_t len, rk_buf *out) {
    size_t pos = 0;
    while (pos < len) {
        size_t line_end = pos;
        while (line_end + 1 < len && !(src[line_end] == '\r' && src[line_end + 1] == '\n'))
            ++line_end;
        if (line_end + 1 >= len)
            return false;
        char hex[24];
        size_t hl = RK_MIN(line_end - pos, sizeof(hex) - 1);
        memcpy(hex, src + pos, hl);
        hex[hl] = '\0';
        char *semi = strchr(hex, ';'); /* chunk extensions */
        if (semi)
            *semi = '\0';
        unsigned long size = strtoul(hex, NULL, 16);
        pos = line_end + 2;
        if (size == 0)
            return true;
        if (size > len - pos)
            return false;
        if (!rk_buf_append(out, src + pos, size))
            return false;
        pos += size + 2; /* trailing CRLF */
    }
    return true;
}

static bool parse_response(const char *raw, size_t len, rk_http_response *resp, char *err,
                           size_t errlen) {
    size_t body = header_end(raw, len);
    if (!body) {
        snprintf(err, errlen, "http: malformed response (no header end)");
        return false;
    }
    const char *sp = memchr(raw, ' ', body);
    if (!sp) {
        snprintf(err, errlen, "http: bad status line");
        return false;
    }
    resp->status = atoi(sp + 1);
    resp->location = header_value(raw, body, "Location");
    char *date = header_value(raw, body, "Date");
    resp->date = date ? rk_http_parse_date(date) : 0;
    free(date);

    rk_buf out = {0};
    char *te = header_value(raw, body, "Transfer-Encoding");
    bool chunked = te && strstr(te, "chunked");
    free(te);
    bool ok;
    if (chunked) {
        ok = decode_chunked(raw + body, len - body, &out);
    } else {
        size_t n = len - body;
        char *cl = header_value(raw, body, "Content-Length");
        if (cl) {
            unsigned long declared = strtoul(cl, NULL, 10);
            if (declared < n)
                n = declared;
            free(cl);
        }
        ok = rk_buf_append(&out, raw + body, n);
    }
    if (!ok || !rk_buf_terminate(&out)) {
        rk_buf_free(&out);
        snprintf(err, errlen, "http: malformed body");
        return false;
    }
    resp->body_len = out.len;
    resp->body = rk_buf_steal_str(&out);
    return resp->body != NULL;
}

/* Stream body_len bytes from body_read in chunks: each chunk gets the usual
 * write timeout, so a large file is bounded by progress, not by its size. */
static bool send_streamed_body(rk_conn *c, const rk_http_request *req, char *err, size_t errlen) {
    enum { CHUNK = 64 * 1024 };
    uint8_t *buf = malloc(CHUNK);
    if (!buf) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    size_t left = req->body_len;
    bool ok = true;
    while (ok && left > 0) {
        long n = req->body_read(req->body_ctx, buf, left < CHUNK ? left : CHUNK);
        if (n <= 0) {
            snprintf(err, errlen, "http: body source ended %zu bytes early", left);
            ok = false;
            break;
        }
        ok = rk_conn_write_all(c, buf, (size_t)n, HTTP_TIMEOUT_MS);
        left -= (size_t)n;
    }
    free(buf);
    return ok;
}

bool rk_http_perform(const rk_http_request *req, rk_http_response *resp, char *err,
                     size_t errlen) {
    memset(resp, 0, sizeof(*resp));
    rk_url u;
    if (!rk_url_parse(req->url, &u) || (strcmp(u.scheme, "http") && strcmp(u.scheme, "https"))) {
        if (u.path)
            rk_url_free(&u);
        snprintf(err, errlen, "http: unsupported URL: %s", req->url);
        return false;
    }

    rk_buf head = {0};
    rk_buf raw = {0};
    bool ok = false;
    rk_conn *c = NULL;
    if (!build_request(&head, req, &u)) {
        snprintf(err, errlen, "out of memory");
        goto done;
    }
    c = rk_conn_open(u.host, u.port, u.tls, HTTP_TIMEOUT_MS, err, errlen);
    if (!c)
        goto done;
    /* A server may reject a request from its headers alone (401 on an upload)
     * and close while the body is still going out. Its answer is then already
     * on the wire: read it rather than report a bare send failure. */
    bool sent = rk_conn_write_all(c, head.data, head.len, HTTP_TIMEOUT_MS) &&
                (req->body_read ? send_streamed_body(c, req, err, errlen)
                                : (!req->body_len ||
                                   rk_conn_write_all(c, req->body, req->body_len, HTTP_TIMEOUT_MS)));
    const int answer_ms = req->response_timeout_ms > 0 ? req->response_timeout_ms : HTTP_TIMEOUT_MS;
    for (;;) {
        char buf[4096];
        int n = rk_conn_read(c, buf, sizeof(buf), sent ? answer_ms : 2000);
        if (n > 0) {
            if (raw.len + (size_t)n > HTTP_MAX_RESPONSE || !rk_buf_append(&raw, buf, (size_t)n)) {
                snprintf(err, errlen, "http: response too large");
                goto done;
            }
            continue;
        }
        if (!sent && !header_end((const char *)raw.data, raw.len)) {
            snprintf(err, errlen, "http: send to %s failed", u.host);
            goto done;
        }
        if (n == -1 || !sent)
            break; /* server closed: end of response (Connection: close) */
        if (n == 0) {
            snprintf(err, errlen, "http: response from %s timed out", u.host);
            goto done;
        }
        snprintf(err, errlen, "http: receive from %s failed", u.host);
        goto done;
    }
    ok = parse_response((const char *)raw.data, raw.len, resp, err, errlen);
done:
    rk_conn_close(c);
    rk_buf_free(&head);
    rk_buf_free(&raw);
    rk_url_free(&u);
    if (!ok)
        rk_http_response_free(resp);
    return ok;
}

void rk_http_response_free(rk_http_response *resp) {
    free(resp->body);
    free(resp->location);
    memset(resp, 0, sizeof(*resp));
}

/* Days from 1970-01-01 to y-m-d (proleptic Gregorian; H. Hinnant's
 * days_from_civil), so no timegm()/TZ dependence. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

int64_t rk_http_parse_date(const char *s) {
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4];
    int day, year, hh, mm, ss;
    /* "Sun, 06 Nov 1994 08:49:37 GMT" */
    const char *comma = strchr(s, ',');
    if (!comma || sscanf(comma + 1, " %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6)
        return 0;
    const char *m = strstr(months, mon);
    if (!m || strlen(mon) != 3 || (m - months) % 3 || day < 1 || day > 31 || year < 1970 ||
        hh > 23 || mm > 59 || ss > 60)
        return 0;
    unsigned month = (unsigned)((m - months) / 3) + 1;
    return days_from_civil(year, month, (unsigned)day) * 86400 + hh * 3600 + mm * 60 + ss;
}
