/* TCP connections with optional TLS (mbedTLS), shared by the HTTP client
 * (registration, WHIP, snapshot upload) and the MQTT control transport.
 *
 * Sockets are non-blocking; every wait is a poll() with an explicit timeout,
 * so no network step can hang a caller forever. TLS verifies the server
 * certificate and host name against a CA bundle unless explicitly disabled. */
#ifndef RK_NET_H
#define RK_NET_H

#include <stdbool.h>
#include <stddef.h>

typedef struct rk_conn rk_conn;

/* Process-wide TLS settings for connections opened afterwards. ca_file NULL
 * or "": the first system bundle found. insecure skips verification. */
void rk_tls_configure(const char *ca_file, bool insecure);

/* Connect host:port (and handshake when tls). Returns NULL on failure with a
 * reason in err. */
rk_conn *rk_conn_open(const char *host, const char *port, bool tls, int timeout_ms,
                      char *err, size_t errlen);
void rk_conn_close(rk_conn *c);

/* Write everything; false on error or when timeout_ms elapses waiting for room. */
bool rk_conn_write_all(rk_conn *c, const void *data, size_t len, int timeout_ms);

/* Read up to len bytes, waiting at most timeout_ms (0 = don't wait).
 * >0 bytes read, 0 nothing arrived in time, -1 peer closed, -2 error. */
int rk_conn_read(rk_conn *c, void *buf, size_t len, int timeout_ms);

/* Decrypted bytes waiting inside TLS (false for plain TCP); a caller that
 * polls the fd itself must drain these first. */
bool rk_conn_has_buffered(rk_conn *c);
int rk_conn_fd(const rk_conn *c);

/* mbedTLS error text; the trimmed build has no string table, so this falls
 * back to the code ("mbedTLS -0x7780"), as listed in the mbedtls headers. */
void rk_mbedtls_error(int code, char *buf, size_t len);

/* poll() one fd, retrying on EINTR. >0 ready, 0 timeout, <0 error. */
int rk_poll_fd(int fd, short events, int timeout_ms);

typedef struct rk_url {
    char scheme[16]; /* lower-case */
    char host[256];
    char port[16];   /* defaulted from the scheme when absent */
    char *path;      /* "/..." with query; owned, free with rk_url_free() */
    bool tls;        /* https, mqtts, ssl, tls */
} rk_url;

/* Accepts http, https, tcp, mqtt (plain) and mqtts, ssl, tls. IPv6 literals in
 * brackets. */
bool rk_url_parse(const char *url, rk_url *out);
void rk_url_free(rk_url *u);

#endif /* RK_NET_H */
