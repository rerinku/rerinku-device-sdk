/* The device's control plane over MQTT 3.1.1 (coreMQTT):
 *   subscribe  rerinku/d/<sn>/cmd     commands in
 *   publish    rerinku/d/<sn>/status  retained status; Last Will = offline
 *   publish    rerinku/d/<sn>/ack     command results
 *   publish    rerinku/d/<sn>/data    device-initiated events
 *
 * One thread owns the connection and runs the MQTT loop; it never holds the
 * lock while waiting on the network, so publishing from other threads stays
 * cheap. Any loss of the broker (TCP drop, broker restart, a server-sent
 * DISCONNECT, keep-alive timeout) ends in backoff + reconnect until stop. */
#ifndef RK_MQTT_H
#define RK_MQTT_H

#include <stdbool.h>
#include <stddef.h>

typedef struct rk_mqtt rk_mqtt;

typedef struct rk_mqtt_config {
    const char *broker; /* tcp://host:1883, mqtts://host:8883, or host:port */
    /* Logs in as username = sn, password = the device token, client id
     * rerinku-dev-<sn>: the platform's broker admits a device only that way
     * and then lets it use its own four topics alone. */
    const char *sn;
    const char *token; /* replaced later with rk_mqtt_set_token() */
    /* All run on the MQTT thread: hand work off, do not block. */
    void (*on_command)(void *user, const char *payload, size_t len);
    void (*on_connection)(void *user, bool up);
    /* The broker turned the connection down (CONNACK refused): typically a
     * device token it no longer accepts. Reconnecting continues with backoff. */
    void (*on_refused)(void *user);
    void *user;
} rk_mqtt_config;

rk_mqtt *rk_mqtt_create(const rk_mqtt_config *cfg, char *err, size_t errlen);
bool rk_mqtt_start(rk_mqtt *m);

/* The token the next (re)connect logs in with; the live connection, if any,
 * stays up — the broker checks credentials only when a client connects. */
void rk_mqtt_set_token(rk_mqtt *m, const char *token);

/* Retained status JSON; remembered and re-published after every reconnect. */
void rk_mqtt_set_status(rk_mqtt *m, const char *json);
void rk_mqtt_publish_ack(rk_mqtt *m, const char *json);
/* False when not connected or the send failed: the message is not queued. */
bool rk_mqtt_publish_data(rk_mqtt *m, const char *json);

/* Publish offline, disconnect, join the thread. Idempotent. */
void rk_mqtt_stop(rk_mqtt *m);
void rk_mqtt_destroy(rk_mqtt *m);

#endif /* RK_MQTT_H */
