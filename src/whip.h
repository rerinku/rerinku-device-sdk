/* WHIP (RFC 9725): one HTTP POST carries the SDP offer and returns the answer;
 * DELETE on the returned resource ends the session. Plus the device
 * registration (bootstrap) request. */
#ifndef RK_WHIP_H
#define RK_WHIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* POST the offer. On success *answer and *resource (absolute URL, may be
 * NULL) are allocated for the caller. *status: the HTTP status, 0 when no
 * answer came (401 means the device token was refused). */
bool rk_whip_post(const char *url, const char *token, const char *offer, char **answer,
                  char **resource, int *status, char *err, size_t errlen);

/* Best-effort DELETE of a WHIP resource. */
void rk_whip_delete(const char *resource, const char *token);

/* What the device tells the platform about itself. */
typedef struct rk_identity {
    const char *endpoint, *sn, *secret, *name, *device_id, *model, *fw_version;
} rk_identity;

/* Runtime settings the platform hands back at registration. */
typedef struct rk_profile {
    char *token;
    /* How long the token is valid from the moment it arrived, measured on the
     * server's own clock (its exp claim against the response's Date header),
     * so a device whose clock is wrong still renews in time. 0 = unknown. */
    int64_t token_lifetime_sec;
    char *whip_url;
    char *mqtt_broker;
    char *stun_server;
    char *turn_server;
} rk_profile;

/* POST <endpoint>/ with X-Rerinku: true and the identity; parse the answer. */
bool rk_register(const rk_identity *id, rk_profile *out, char *err, size_t errlen);
void rk_profile_free(rk_profile *p);

/* exp claim of a JWT, 0 if absent or unreadable. */
int64_t rk_jwt_exp(const char *token);

#endif /* RK_WHIP_H */
