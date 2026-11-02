/* Minimal HTTP/1.1 client for the SDK's few one-shot requests: registration,
 * WHIP POST/DELETE and snapshot upload.
 *
 *   - http:// and https:// (mbedTLS, certificate verified; see net.h)
 *   - one request per connection (Connection: close), bounded by a timeout
 *   - optional Bearer token and one extra header
 *   - status, body and the Location and Date headers; chunked bodies decoded
 *   - a server that answers before reading the whole body (e.g. 401 on an
 *     upload) and drops the connection still yields its status */
#ifndef RK_HTTP_H
#define RK_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct rk_http_request {
    const char *method;       /* "GET", "POST", "DELETE" */
    const char *url;          /* http[s]://host[:port]/path */
    const char *content_type; /* NULL = none */
    const void *body;
    size_t body_len;
    const char *bearer;       /* NULL = no Authorization header */
    const char *extra_header; /* one "Name: value" line without CRLF, or NULL */
    /* Streamed body (large uploads) instead of `body`: body_len is the total;
     * body_read fills up to cap bytes and returns how many, 0 at the end,
     * -1 on error. */
    long (*body_read)(void *ctx, uint8_t *buf, size_t cap);
    void *body_ctx;
    /* How long to wait for the answer once the body is out (0 = 15 s). A
     * platform that processes an upload before answering needs more. */
    int response_timeout_ms;
} rk_http_request;

typedef struct rk_http_response {
    int status;
    char *body; /* NUL-terminated */
    size_t body_len;
    char *location; /* Location header, or NULL */
    int64_t date;   /* Date header as Unix seconds (the server's clock), 0 if absent */
} rk_http_response;

/* RFC 9110 IMF-fixdate ("Sun, 06 Nov 1994 08:49:37 GMT") to Unix seconds, 0 if
 * malformed. */
int64_t rk_http_parse_date(const char *s);

/* false on transport failure (reason in err); any HTTP status is a success
 * here, the caller checks it. Free the response with rk_http_response_free(). */
bool rk_http_perform(const rk_http_request *req, rk_http_response *resp, char *err,
                     size_t errlen);
void rk_http_response_free(rk_http_response *resp);

#endif /* RK_HTTP_H */
