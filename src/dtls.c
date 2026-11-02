#include "dtls.h"

#include "common.h"
#include "log.h"
#include "net.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <ctype.h>
#include <stdio.h>

/* Path MTU minus IPv6 + UDP headers, like libdatachannel. */
#define DTLS_MTU (1280 - 40 - 8)

/* ---- process-wide certificate ----------------------------------------------
 * One self-signed ECDSA P-256 certificate per process: generating a key costs
 * tens of ms on a Cortex-A7, and WebRTC authenticates by fingerprint, so there
 * is nothing to gain from a fresh one per session. */

static pthread_once_t g_cert_once = PTHREAD_ONCE_INIT;
static bool g_cert_ok;
static mbedtls_x509_crt g_cert;
static mbedtls_pk_context g_key;
static char g_fingerprint[8 + 32 * 3]; /* "sha-256 " + hex pairs with colons */

static void format_fingerprint(const uint8_t *der, size_t len, char *out, size_t cap) {
    uint8_t hash[32];
    mbedtls_sha256(der, len, hash, 0);
    size_t o = (size_t)snprintf(out, cap, "sha-256 ");
    for (int i = 0; i < 32 && o + 3 < cap; ++i)
        o += (size_t)snprintf(out + o, cap - o, i ? ":%02X" : "%02X", hash[i]);
}

static void cert_init_once(void) {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509write_cert w;
    unsigned char der[1024];
    int rc;

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509write_crt_init(&w);
    mbedtls_x509_crt_init(&g_cert);
    mbedtls_pk_init(&g_key);

    rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, NULL, 0);
    if (rc == 0)
        rc = mbedtls_pk_setup(&g_key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (rc == 0)
        rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(g_key),
                                 mbedtls_ctr_drbg_random, &drbg);
    if (rc == 0) {
        uint8_t sn[8];
        rk_random(sn, sizeof(sn));
        sn[0] = (uint8_t)((sn[0] & 0x7f) | 0x01); /* positive, no leading zero */
        rc = mbedtls_x509write_crt_set_serial_raw(&w, sn, sizeof(sn));
    }
    if (rc == 0)
        rc = mbedtls_x509write_crt_set_subject_name(&w, "CN=rerinku");
    if (rc == 0)
        rc = mbedtls_x509write_crt_set_issuer_name(&w, "CN=rerinku");
    /* A wide window: peers check the fingerprint, not dates, and a device clock
     * may be far off before NTP. */
    if (rc == 0)
        rc = mbedtls_x509write_crt_set_validity(&w, "20200101000000", "20491231235959");
    if (rc == 0) {
        mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_subject_key(&w, &g_key);
        mbedtls_x509write_crt_set_issuer_key(&w, &g_key);
        mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
        /* Writes at the END of the buffer; returns the length. */
        rc = mbedtls_x509write_crt_der(&w, der, sizeof(der), mbedtls_ctr_drbg_random, &drbg);
    }
    if (rc > 0) {
        const unsigned char *start = der + sizeof(der) - (size_t)rc;
        size_t len = (size_t)rc;
        rc = mbedtls_x509_crt_parse_der(&g_cert, start, len);
        if (rc == 0) {
            format_fingerprint(start, len, g_fingerprint, sizeof(g_fingerprint));
            g_cert_ok = true;
        }
    }
    if (!g_cert_ok) {
        char why[128];
        rk_mbedtls_error(rc, why, sizeof(why));
        RLOG_ERR("dtls", "certificate generation failed: %s", why);
    }
    mbedtls_x509write_crt_free(&w);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
}

const char *rk_dtls_local_fingerprint(void) {
    pthread_once(&g_cert_once, cert_init_once);
    return g_cert_ok ? g_fingerprint : NULL;
}

/* ---- session ----------------------------------------------------------------- */

static const mbedtls_ssl_srtp_profile k_srtp_profiles[] = {
    MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80,
    MBEDTLS_TLS_SRTP_UNSET,
};

struct rk_dtls {
    bool server;
    char remote_fp[100];
    rk_dtls_send_fn send;
    void *send_ctx;

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;

    /* The one datagram waiting to be read by the handshake. */
    uint8_t in[2048];
    size_t in_len;

    /* Retransmission timer (mbedTLS's intermediate/final delays). */
    uint64_t timer_start;
    uint32_t timer_int_ms;
    uint32_t timer_fin_ms;

    /* Captured by the export-keys callback. */
    unsigned char master[48];
    unsigned char randoms[64]; /* client_random || server_random */
    mbedtls_tls_prf_types prf;
    bool fingerprint_ok;
};

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    rk_dtls *d = ctx;
    return d->send(d->send_ctx, buf, len) ? (int)len : MBEDTLS_ERR_SSL_WANT_WRITE;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    rk_dtls *d = ctx;
    if (d->in_len == 0)
        return MBEDTLS_ERR_SSL_WANT_READ;
    /* Datagram semantics: hand over the whole record batch at once. */
    size_t n = RK_MIN(len, d->in_len);
    memcpy(buf, d->in, n);
    d->in_len = 0;
    return (int)n;
}

static void timer_set(void *ctx, uint32_t int_ms, uint32_t fin_ms) {
    rk_dtls *d = ctx;
    d->timer_start = rk_now_ms();
    d->timer_int_ms = int_ms;
    d->timer_fin_ms = fin_ms;
}

static int timer_get(void *ctx) {
    rk_dtls *d = ctx;
    if (d->timer_fin_ms == 0)
        return -1; /* cancelled */
    uint64_t elapsed = rk_now_ms() - d->timer_start;
    if (elapsed >= d->timer_fin_ms)
        return 2;
    if (elapsed >= d->timer_int_ms)
        return 1;
    return 0;
}

static void export_keys(void *ctx, mbedtls_ssl_key_export_type type, const unsigned char *secret,
                        size_t secret_len, const unsigned char client_random[32],
                        const unsigned char server_random[32], mbedtls_tls_prf_types prf) {
    (void)type;
    rk_dtls *d = ctx;
    memcpy(d->master, secret, RK_MIN(secret_len, sizeof(d->master)));
    memcpy(d->randoms, client_random, 32);
    memcpy(d->randoms + 32, server_random, 32);
    d->prf = prf;
}

static void normalize_fp(const char *in, char *out, size_t cap) {
    /* Drop the "sha-256 " algorithm prefix, upper-case the hex. */
    const char *sp = strchr(in, ' ');
    const char *hex = sp ? sp + 1 : in;
    size_t o = 0;
    for (; *hex && o + 1 < cap; ++hex)
        if (!isspace((unsigned char)*hex))
            out[o++] = (char)toupper((unsigned char)*hex);
    out[o] = '\0';
}

/* The peer's certificate is self-signed: it is authenticated by matching the
 * fingerprint its SDP announced, nothing else. */
static int verify_cert(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    rk_dtls *d = ctx;
    if (depth != 0)
        return 0;
    char fp[sizeof(g_fingerprint)];
    char got[sizeof(g_fingerprint)];
    format_fingerprint(crt->raw.p, crt->raw.len, fp, sizeof(fp));
    normalize_fp(fp, got, sizeof(got));
    d->fingerprint_ok = strcmp(got, d->remote_fp) == 0;
    if (d->fingerprint_ok)
        *flags = 0;
    return d->fingerprint_ok ? 0 : 1;
}

rk_dtls *rk_dtls_create(bool server, const char *remote_fingerprint, rk_dtls_send_fn send,
                        void *ctx, char *err, size_t errlen) {
    if (!rk_dtls_local_fingerprint()) {
        snprintf(err, errlen, "no DTLS certificate");
        return NULL;
    }
    rk_dtls *d = calloc(1, sizeof(*d));
    if (!d) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    d->server = server;
    d->send = send;
    d->send_ctx = ctx;
    normalize_fp(remote_fingerprint, d->remote_fp, sizeof(d->remote_fp));
    mbedtls_ssl_init(&d->ssl);
    mbedtls_ssl_config_init(&d->conf);
    mbedtls_ctr_drbg_init(&d->drbg);
    mbedtls_entropy_init(&d->entropy);

    int rc = mbedtls_ctr_drbg_seed(&d->drbg, mbedtls_entropy_func, &d->entropy, NULL, 0);
    if (rc == 0)
        rc = mbedtls_ssl_config_defaults(&d->conf,
                                         server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_DATAGRAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc == 0) {
        mbedtls_ssl_conf_max_tls_version(&d->conf, MBEDTLS_SSL_VERSION_TLS1_2);
        /* Request/accept the peer certificate but judge it by fingerprint. */
        mbedtls_ssl_conf_authmode(&d->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
        mbedtls_ssl_conf_verify(&d->conf, verify_cert, d);
        mbedtls_ssl_conf_rng(&d->conf, mbedtls_ctr_drbg_random, &d->drbg);
        rc = mbedtls_ssl_conf_own_cert(&d->conf, &g_cert, &g_key);
    }
    if (rc == 0) {
        /* The ICE layer already proved the peer owns its address. */
        mbedtls_ssl_conf_dtls_cookies(&d->conf, NULL, NULL, NULL);
        rc = mbedtls_ssl_conf_dtls_srtp_protection_profiles(&d->conf, k_srtp_profiles);
    }
    if (rc == 0)
        rc = mbedtls_ssl_setup(&d->ssl, &d->conf);
    if (rc != 0) {
        char why[128];
        rk_mbedtls_error(rc, why, sizeof(why));
        snprintf(err, errlen, "DTLS setup failed: %s", why);
        rk_dtls_destroy(d);
        return NULL;
    }
    mbedtls_ssl_set_export_keys_cb(&d->ssl, export_keys, d);
    mbedtls_ssl_set_bio(&d->ssl, d, bio_send, bio_recv, NULL);
    mbedtls_ssl_set_timer_cb(&d->ssl, d, timer_set, timer_get);
    mbedtls_ssl_set_mtu(&d->ssl, DTLS_MTU);
    return d;
}

void rk_dtls_destroy(rk_dtls *d) {
    if (!d)
        return;
    mbedtls_ssl_free(&d->ssl);
    mbedtls_ssl_config_free(&d->conf);
    mbedtls_ctr_drbg_free(&d->drbg);
    mbedtls_entropy_free(&d->entropy);
    free(d);
}

void rk_dtls_input(rk_dtls *d, const uint8_t *data, size_t len) {
    if (len > sizeof(d->in))
        return; /* not a handshake record we can use */
    memcpy(d->in, data, len);
    d->in_len = len;
}

int rk_dtls_step(rk_dtls *d, char *err, size_t errlen) {
    int rc = mbedtls_ssl_handshake(&d->ssl);
    if (rc == 0)
        return 1;
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE ||
        rc == MBEDTLS_ERR_SSL_TIMEOUT)
        return 0;
    char why[128];
    rk_mbedtls_error(rc, why, sizeof(why));
    snprintf(err, errlen, "DTLS handshake failed: %s%s", why,
             d->fingerprint_ok ? "" : " (certificate fingerprint mismatch?)");
    return -1;
}

uint64_t rk_dtls_timer_deadline(const rk_dtls *d) {
    return d->timer_fin_ms ? d->timer_start + d->timer_fin_ms : 0;
}

bool rk_dtls_srtp_keys(rk_dtls *d, uint8_t local[RK_SRTP_KEY_SALT_LEN],
                       uint8_t remote[RK_SRTP_KEY_SALT_LEN]) {
    mbedtls_dtls_srtp_info info;
    mbedtls_ssl_get_dtls_srtp_negotiation_result(&d->ssl, &info);
    if (info.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile) != MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80)
        return false;
    if (!d->fingerprint_ok)
        return false;
    /* RFC 5764 4.2: client key, server key, client salt, server salt. */
    unsigned char m[2 * RK_SRTP_KEY_SALT_LEN];
    if (mbedtls_ssl_tls_prf(d->prf, d->master, sizeof(d->master), "EXTRACTOR-dtls_srtp",
                            d->randoms, sizeof(d->randoms), m, sizeof(m)) != 0)
        return false;
    const unsigned char *client_key = m, *server_key = m + 16;
    const unsigned char *client_salt = m + 32, *server_salt = m + 46;
    const unsigned char *lk = d->server ? server_key : client_key;
    const unsigned char *ls = d->server ? server_salt : client_salt;
    const unsigned char *rk = d->server ? client_key : server_key;
    const unsigned char *rs = d->server ? client_salt : server_salt;
    memcpy(local, lk, 16);
    memcpy(local + 16, ls, 14);
    memcpy(remote, rk, 16);
    memcpy(remote + 16, rs, 14);
    mbedtls_platform_zeroize(m, sizeof(m));
    return true;
}

bool rk_dtls_peer_closed(rk_dtls *d) {
    unsigned char buf[256];
    int rc = mbedtls_ssl_read(&d->ssl, buf, sizeof(buf));
    return rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE;
}

void rk_dtls_close(rk_dtls *d) {
    if (d)
        mbedtls_ssl_close_notify(&d->ssl);
}
