/* DTLS 1.2 handshake for DTLS-SRTP (RFC 5764) on top of the ICE transport,
 * with mbedTLS. The peer is authenticated by the certificate fingerprint from
 * its SDP; the handshake's only product is the SRTP keying material.
 *
 * Not thread-safe: one thread (the session's) drives everything. */
#ifndef RK_DTLS_H
#define RK_DTLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RK_SRTP_KEY_SALT_LEN 30 /* AES-128 key (16) + salt (14) */

typedef struct rk_dtls rk_dtls;

/* Sends one datagram on the ICE transport. */
typedef bool (*rk_dtls_send_fn)(void *ctx, const uint8_t *data, size_t len);

/* "sha-256 AB:CD:..." of this process's certificate (generated on first use). */
const char *rk_dtls_local_fingerprint(void);

/* server: we answer the peer's ClientHello (our SDP setup was passive or the
 * answer said active). remote_fingerprint: hex with colons, case-insensitive. */
rk_dtls *rk_dtls_create(bool server, const char *remote_fingerprint, rk_dtls_send_fn send,
                        void *ctx, char *err, size_t errlen);
void rk_dtls_destroy(rk_dtls *d);

/* Hand one received DTLS datagram to the handshake. */
void rk_dtls_input(rk_dtls *d, const uint8_t *data, size_t len);

/* Advance the handshake (call after input and when the retransmission timer
 * fires). 1 = done, 0 = in progress, -1 = failed (reason in err). */
int rk_dtls_step(rk_dtls *d, char *err, size_t errlen);

/* Monotonic ms at which rk_dtls_step must run again for a retransmission, or
 * 0 when no timer is armed. */
uint64_t rk_dtls_timer_deadline(const rk_dtls *d);

/* After completion: SRTP master key+salt for what we send and what we receive. */
bool rk_dtls_srtp_keys(rk_dtls *d, uint8_t local[RK_SRTP_KEY_SALT_LEN],
                       uint8_t remote[RK_SRTP_KEY_SALT_LEN]);

/* After the handshake: consume a received record (fed with rk_dtls_input).
 * True when the peer ended the association (close_notify or a fatal alert). */
bool rk_dtls_peer_closed(rk_dtls *d);

/* Tell the peer we are leaving (close_notify), best effort. */
void rk_dtls_close(rk_dtls *d);

#endif /* RK_DTLS_H */
