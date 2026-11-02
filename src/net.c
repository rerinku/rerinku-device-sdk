#include "net.h"

#include "common.h"
#include "log.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

/* Writing to a socket the peer already closed must return EPIPE, not kill the
 * process with SIGPIPE: an embedded application rarely installs a handler. */
#if defined(MSG_NOSIGNAL)
#define RK_SEND_FLAGS MSG_NOSIGNAL
#else
#define RK_SEND_FLAGS 0
#endif

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

struct rk_conn {
    int fd;
    bool tls;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
};

static bool would_block(int err) {
    return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
}

static int remaining_ms(uint64_t deadline) {
    uint64_t now = rk_now_ms();
    return deadline > now ? (int)(deadline - now) : 0;
}

int rk_poll_fd(int fd, short events, int timeout_ms) {
    struct pollfd p = {.fd = fd, .events = events};
    for (;;) {
        int rc = poll(&p, 1, timeout_ms);
        if (rc < 0 && errno == EINTR)
            continue;
        return rc;
    }
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

/* ---- TCP ------------------------------------------------------------------- */

static int connect_one(const struct addrinfo *ai, uint64_t deadline) {
    int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
    if (fd < 0)
        return -1;
    set_nonblocking(fd);
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
        return fd;
    if (errno != EINPROGRESS || rk_poll_fd(fd, POLLOUT, remaining_ms(deadline)) <= 0) {
        close(fd);
        return -1;
    }
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err) {
        close(fd);
        return -1;
    }
    return fd;
}

static int tcp_connect(const char *host, const char *port, uint64_t deadline, char *err,
                       size_t errlen) {
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    /* getaddrinfo is not bounded by our deadline (no portable async resolver);
     * the C library's resolver timeout applies. */
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        snprintf(err, errlen, "resolve %s failed: %s", host, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next) {
        if (remaining_ms(deadline) == 0)
            break;
        fd = connect_one(ai, deadline);
    }
    freeaddrinfo(res);
    if (fd < 0)
        snprintf(err, errlen, "connect to %s:%s failed or timed out", host, port);
    return fd;
}

/* ---- TLS ------------------------------------------------------------------- */

static pthread_mutex_t g_tls_mu = PTHREAD_MUTEX_INITIALIZER;
static char *g_ca_file;     /* guarded by g_tls_mu */
static bool g_insecure;     /* guarded by g_tls_mu */
static bool g_ca_loaded;    /* guarded by g_tls_mu */
static char *g_ca_source;   /* which ca_file the loaded chain came from */
static mbedtls_x509_crt g_ca_chain; /* shared read-only by connections */

static const char *const k_system_bundles[] = {
    "/etc/ssl/certs/ca-certificates.crt", /* Debian/Ubuntu/Buildroot */
    "/etc/pki/tls/certs/ca-bundle.crt",   /* Fedora/RHEL */
    "/etc/ssl/cert.pem",                  /* Alpine, macOS */
    "/etc/ssl/certs/ca-bundle.crt",
};

void rk_tls_configure(const char *ca_file, bool insecure) {
    pthread_mutex_lock(&g_tls_mu);
    free(g_ca_file);
    g_ca_file = ca_file && *ca_file ? rk_strdup(ca_file) : NULL;
    g_insecure = insecure;
    pthread_mutex_unlock(&g_tls_mu);
}

void rk_mbedtls_error(int code, char *buf, size_t len) {
    mbedtls_strerror(code, buf, len);
    if (len > 0 && buf[0] == '\0')
        snprintf(buf, len, "mbedTLS -0x%04x", (unsigned)(code < 0 ? -code : code));
}

#define mbed_err rk_mbedtls_error

/* Caller holds g_tls_mu. */
static mbedtls_x509_crt *load_ca_locked(char *err, size_t errlen) {
    if (g_ca_loaded && rk_streq(g_ca_source, g_ca_file))
        return &g_ca_chain;
    if (g_ca_loaded)
        mbedtls_x509_crt_free(&g_ca_chain);
    mbedtls_x509_crt_init(&g_ca_chain);
    g_ca_loaded = false;

    const char *used = NULL;
    if (g_ca_file) {
        int rc = mbedtls_x509_crt_parse_file(&g_ca_chain, g_ca_file);
        if (rc < 0) {
            char why[128];
            mbed_err(rc, why, sizeof(why));
            snprintf(err, errlen, "cannot load CA file %s: %s", g_ca_file, why);
            mbedtls_x509_crt_free(&g_ca_chain);
            return NULL;
        }
        used = g_ca_file;
    } else {
        for (size_t i = 0; i < RK_ARRAY_LEN(k_system_bundles); ++i) {
            /* A positive return means some entries were skipped; fine. */
            if (mbedtls_x509_crt_parse_file(&g_ca_chain, k_system_bundles[i]) >= 0) {
                used = k_system_bundles[i];
                break;
            }
            mbedtls_x509_crt_free(&g_ca_chain);
            mbedtls_x509_crt_init(&g_ca_chain);
        }
        if (!used) {
            snprintf(err, errlen, "no CA bundle found; set ca_file (or tls_insecure for development)");
            mbedtls_x509_crt_free(&g_ca_chain);
            return NULL;
        }
    }
    g_ca_loaded = true;
    free(g_ca_source);
    g_ca_source = rk_strdup(g_ca_file);
    RLOG_INFO("tls", "trusting CA certificates from %s", used);
    return &g_ca_chain;
}

#if defined(MBEDTLS_PSA_CRYPTO_C)
static pthread_once_t g_psa_once = PTHREAD_ONCE_INIT;
static void psa_init_once(void) {
    /* TLS 1.3 in mbedTLS 3.6 runs on PSA crypto, which needs this once. */
    if (psa_crypto_init() != PSA_SUCCESS)
        RLOG_ERR("tls", "psa_crypto_init failed");
}
#endif

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    ssize_t n = send(fd, buf, len, RK_SEND_FLAGS);
    if (n >= 0)
        return (int)n;
    return would_block(errno) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    ssize_t n = recv(fd, buf, len, 0);
    if (n >= 0)
        return (int)n; /* 0 = peer closed the TCP stream */
    return would_block(errno) ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
}

static bool tls_handshake(rk_conn *c, const char *host, uint64_t deadline, char *err,
                          size_t errlen) {
#if defined(MBEDTLS_PSA_CRYPTO_C)
    pthread_once(&g_psa_once, psa_init_once);
#endif
    static const char pers[] = "rerinku-tls";
    char why[160];
    int rc = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy,
                                   (const unsigned char *)pers, sizeof(pers) - 1);
    if (rc == 0)
        rc = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        mbed_err(rc, why, sizeof(why));
        snprintf(err, errlen, "TLS setup failed: %s", why);
        return false;
    }
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);

    pthread_mutex_lock(&g_tls_mu);
    bool insecure = g_insecure;
    mbedtls_x509_crt *ca = insecure ? NULL : load_ca_locked(err, errlen);
    pthread_mutex_unlock(&g_tls_mu);
    if (!insecure && !ca)
        return false;
    if (ca)
        mbedtls_ssl_conf_ca_chain(&c->conf, ca, NULL);
    mbedtls_ssl_conf_authmode(&c->conf, insecure ? MBEDTLS_SSL_VERIFY_NONE
                                                 : MBEDTLS_SSL_VERIFY_REQUIRED);

    rc = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (rc == 0)
        rc = mbedtls_ssl_set_hostname(&c->ssl, host); /* SNI + name to verify */
    if (rc != 0) {
        mbed_err(rc, why, sizeof(why));
        snprintf(err, errlen, "TLS setup failed: %s", why);
        return false;
    }
    mbedtls_ssl_set_bio(&c->ssl, &c->fd, bio_send, bio_recv, NULL);

    for (;;) {
        rc = mbedtls_ssl_handshake(&c->ssl);
        if (rc == 0)
            return true;
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            mbed_err(rc, why, sizeof(why));
            uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
            if (flags != 0 && flags != (uint32_t)-1) {
                char info[160];
                mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
                size_t n = strlen(info);
                while (n > 0 && (info[n - 1] == '\n' || info[n - 1] == ' '))
                    info[--n] = '\0';
                snprintf(err, errlen, "TLS handshake with %s failed: %s (%s)", host, why, info);
            } else {
                snprintf(err, errlen, "TLS handshake with %s failed: %s", host, why);
            }
            return false;
        }
        short ev = rc == MBEDTLS_ERR_SSL_WANT_WRITE ? POLLOUT : POLLIN;
        if (rk_poll_fd(c->fd, ev, remaining_ms(deadline)) <= 0) {
            snprintf(err, errlen, "TLS handshake with %s timed out", host);
            return false;
        }
    }
}

/* ---- connection ------------------------------------------------------------ */

rk_conn *rk_conn_open(const char *host, const char *port, bool tls, int timeout_ms,
                      char *err, size_t errlen) {
    uint64_t deadline = rk_now_ms() + (uint64_t)timeout_ms;
    int fd = tcp_connect(host, port, deadline, err, errlen);
    if (fd < 0)
        return NULL;
    rk_conn *c = calloc(1, sizeof(*c));
    if (!c) {
        close(fd);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    c->fd = fd;
    c->tls = tls;
    if (tls) {
        mbedtls_ssl_init(&c->ssl);
        mbedtls_ssl_config_init(&c->conf);
        mbedtls_ctr_drbg_init(&c->drbg);
        mbedtls_entropy_init(&c->entropy);
        if (!tls_handshake(c, host, deadline, err, errlen)) {
            rk_conn_close(c);
            return NULL;
        }
    }
    return c;
}

void rk_conn_close(rk_conn *c) {
    if (!c)
        return;
    if (c->tls) {
        mbedtls_ssl_close_notify(&c->ssl); /* best effort, non-blocking */
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_ctr_drbg_free(&c->drbg);
        mbedtls_entropy_free(&c->entropy);
    }
    if (c->fd >= 0)
        close(c->fd);
    free(c);
}

bool rk_conn_write_all(rk_conn *c, const void *data, size_t len, int timeout_ms) {
    const unsigned char *p = data;
    while (len > 0) {
        if (c->tls) {
            int n = mbedtls_ssl_write(&c->ssl, p, len);
            if (n > 0) {
                p += n;
                len -= (size_t)n;
                continue;
            }
            if (n != MBEDTLS_ERR_SSL_WANT_WRITE && n != MBEDTLS_ERR_SSL_WANT_READ)
                return false;
            short ev = n == MBEDTLS_ERR_SSL_WANT_WRITE ? POLLOUT : POLLIN;
            if (rk_poll_fd(c->fd, ev, timeout_ms) <= 0)
                return false;
        } else {
            ssize_t n = send(c->fd, p, len, RK_SEND_FLAGS);
            if (n > 0) {
                p += n;
                len -= (size_t)n;
                continue;
            }
            if (n < 0 && !would_block(errno))
                return false;
            if (rk_poll_fd(c->fd, POLLOUT, timeout_ms) <= 0)
                return false;
        }
    }
    return true;
}

int rk_conn_read(rk_conn *c, void *buf, size_t len, int timeout_ms) {
    /* Keep reading until data or the deadline: a TLS record can arrive in
     * several TCP segments, so "readable" does not mean a record is complete. */
    const uint64_t deadline = rk_now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        short ev = POLLIN;
        if (c->tls) {
            int n = mbedtls_ssl_read(&c->ssl, buf, len);
            if (n > 0)
                return n;
            if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
                return -1;
#if defined(MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
            if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
                continue; /* TLS 1.3 post-handshake message, not data */
#endif
            if (n != MBEDTLS_ERR_SSL_WANT_READ && n != MBEDTLS_ERR_SSL_WANT_WRITE)
                return -2;
            if (n == MBEDTLS_ERR_SSL_WANT_WRITE)
                ev = POLLOUT;
        } else {
            ssize_t n = recv(c->fd, buf, len, 0);
            if (n > 0)
                return (int)n;
            if (n == 0)
                return -1;
            if (!would_block(errno))
                return -2;
        }
        int left = remaining_ms(deadline);
        if (left == 0)
            return 0;
        int rc = rk_poll_fd(c->fd, ev, left);
        if (rc == 0)
            return 0;
        if (rc < 0)
            return -2;
    }
}

bool rk_conn_has_buffered(rk_conn *c) {
    return c->tls && mbedtls_ssl_get_bytes_avail(&c->ssl) > 0;
}

int rk_conn_fd(const rk_conn *c) { return c->fd; }

/* ---- URL ------------------------------------------------------------------- */

bool rk_url_parse(const char *url, rk_url *out) {
    memset(out, 0, sizeof(*out));
    const char *sep = url ? strstr(url, "://") : NULL;
    if (!sep || sep == url || (size_t)(sep - url) >= sizeof(out->scheme))
        return false;
    for (size_t i = 0; i < (size_t)(sep - url); ++i)
        out->scheme[i] = (char)tolower((unsigned char)url[i]);

    const char *default_port;
    if (!strcmp(out->scheme, "http")) {
        default_port = "80";
    } else if (!strcmp(out->scheme, "https")) {
        default_port = "443";
        out->tls = true;
    } else if (!strcmp(out->scheme, "tcp") || !strcmp(out->scheme, "mqtt")) {
        default_port = "1883";
    } else if (!strcmp(out->scheme, "mqtts") || !strcmp(out->scheme, "ssl") ||
               !strcmp(out->scheme, "tls")) {
        default_port = "8883";
        out->tls = true;
    } else {
        return false;
    }

    const char *auth = sep + 3;
    const char *slash = strchr(auth, '/');
    size_t auth_len = slash ? (size_t)(slash - auth) : strlen(auth);
    out->path = rk_strdup(slash ? slash : "/");
    if (!out->path)
        return false;
    rk_strlcpy(out->port, default_port, sizeof(out->port));

    const char *host = auth;
    size_t host_len = auth_len;
    const char *port = NULL;
    size_t port_len = 0;
    if (auth_len > 0 && auth[0] == '[') {
        const char *rb = memchr(auth, ']', auth_len);
        if (!rb)
            goto fail;
        host = auth + 1;
        host_len = (size_t)(rb - host);
        if (rb + 1 < auth + auth_len) {
            if (rb[1] != ':')
                goto fail;
            port = rb + 2;
            port_len = (size_t)(auth + auth_len - port);
        }
    } else {
        const char *colon = NULL;
        for (const char *p = auth; p < auth + auth_len; ++p)
            if (*p == ':')
                colon = p;
        if (colon) {
            host_len = (size_t)(colon - auth);
            port = colon + 1;
            port_len = (size_t)(auth + auth_len - port);
        }
    }
    if (host_len == 0 || host_len >= sizeof(out->host) || (port && (port_len == 0 ||
        port_len >= sizeof(out->port))))
        goto fail;
    memcpy(out->host, host, host_len);
    out->host[host_len] = '\0';
    if (port) {
        memcpy(out->port, port, port_len);
        out->port[port_len] = '\0';
    }
    return true;
fail:
    rk_url_free(out);
    return false;
}

void rk_url_free(rk_url *u) {
    free(u->path);
    u->path = NULL;
}
