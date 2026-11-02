#include "mqtt.h"

#include "common.h"
#include "log.h"
#include "net.h"

#include <core_mqtt.h>

#include <poll.h>
#include <stdio.h>

/* coreMQTT leaves the transport's context type to the application. */
struct NetworkContext {
    rk_conn *conn;
    /* coreMQTT polls recv in a loop while waiting for CONNACK; a short wait
     * there avoids a busy spin. In the process loop it is 0: we poll first. */
    int recv_wait_ms;
};

/* Incoming publishes must fit whole: commands are small JSON. */
#define NETWORK_BUFFER_BYTES (16 * 1024)
#define INFLIGHT_RECORDS 16
#define KEEPALIVE_SEC 30
#define CONNECT_TIMEOUT_MS 10000
#define SEND_TIMEOUT_MS 10000
#define MAX_BACKOFF_SEC 30

typedef struct session {
    MQTTContext_t ctx;
    NetworkContext_t net;
    uint8_t buffer[NETWORK_BUFFER_BYTES];
    MQTTPubAckInfo_t outgoing[INFLIGHT_RECORDS];
    MQTTPubAckInfo_t incoming[INFLIGHT_RECORDS];
} session;

struct rk_mqtt {
    char *broker;
    char host[256];
    char port[16];
    bool tls;
    char *client_id, *username;
    char *cmd_topic, *status_topic, *ack_topic, *data_topic;
    void (*on_command)(void *, const char *, size_t);
    void (*on_connection)(void *, bool);
    void (*on_refused)(void *);
    void *user;

    /* mu guards s (coreMQTT is not thread-safe: every MQTT_* call happens
     * under it), the remembered status and the login token. */
    pthread_mutex_t mu;
    session *s;
    bool connected;
    char *status_json;
    char *token;

    bool started;
    volatile bool stopping;
    pthread_t thread;
    pthread_mutex_t sleep_mu;
    pthread_cond_t sleep_cv;
};

static uint32_t now_ms32(void) { return (uint32_t)rk_now_ms(); }

static int32_t transport_recv(NetworkContext_t *ctx, void *buf, size_t len) {
    int n = rk_conn_read(ctx->conn, buf, len, ctx->recv_wait_ms);
    return n < 0 ? -1 : n;
}

static int32_t transport_send(NetworkContext_t *ctx, const void *buf, size_t len) {
    return rk_conn_write_all(ctx->conn, buf, len, SEND_TIMEOUT_MS) ? (int32_t)len : -1;
}

/* The MQTT thread currently inside MQTT_ProcessLoop: incoming packets are only
 * delivered from there, synchronously, so a thread-local finds the owner from
 * coreMQTT's context-less callback. */
static __thread rk_mqtt *t_current;

static void on_event(MQTTContext_t *ctx, MQTTPacketInfo_t *packet, MQTTDeserializedInfo_t *info) {
    (void)ctx;
    rk_mqtt *m = t_current;
    if (!m || !packet)
        return;
    if ((packet->type & 0xF0U) == MQTT_PACKET_TYPE_PUBLISH) {
        const MQTTPublishInfo_t *pub = info ? info->pPublishInfo : NULL;
        if (pub && pub->pPayload && pub->payloadLength > 0 && m->on_command)
            m->on_command(m->user, pub->pPayload, pub->payloadLength);
    } else if (packet->type == MQTT_PACKET_TYPE_SUBACK) {
        uint8_t *codes = NULL;
        size_t count = 0;
        if (MQTT_GetSubAckStatusCodes(packet, &codes, &count) == MQTTSuccess)
            for (size_t i = 0; i < count; ++i)
                if (codes[i] == 0x80)
                    RLOG_ERR("mqtt", "broker rejected subscription to %s", m->cmd_topic);
    }
}

static const char k_offline[] = "{\"online\":false}";

rk_mqtt *rk_mqtt_create(const rk_mqtt_config *cfg, char *err, size_t errlen) {
    /* Accept a bare host:port as plain MQTT. */
    char *url = strstr(cfg->broker, "://") ? rk_strdup(cfg->broker)
                                           : rk_sprintf("tcp://%s", cfg->broker);
    rk_url u;
    bool ok = url && rk_url_parse(url, &u);
    free(url);
    if (!ok || !strcmp(u.scheme, "http") || !strcmp(u.scheme, "https")) {
        if (ok)
            rk_url_free(&u);
        snprintf(err, errlen, "bad MQTT broker URL: %s", cfg->broker);
        return NULL;
    }
    rk_mqtt *m = calloc(1, sizeof(*m));
    if (!m) {
        rk_url_free(&u);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    rk_strlcpy(m->host, u.host, sizeof(m->host));
    rk_strlcpy(m->port, u.port, sizeof(m->port));
    m->tls = u.tls;
    rk_url_free(&u);
    m->broker = rk_strdup(cfg->broker);
    m->client_id = rk_sprintf("rerinku-dev-%s", cfg->sn);
    m->username = rk_strdup(cfg->sn);
    m->token = cfg->token ? rk_strdup(cfg->token) : NULL;
    m->cmd_topic = rk_sprintf("rerinku/d/%s/cmd", cfg->sn);
    m->status_topic = rk_sprintf("rerinku/d/%s/status", cfg->sn);
    m->ack_topic = rk_sprintf("rerinku/d/%s/ack", cfg->sn);
    m->data_topic = rk_sprintf("rerinku/d/%s/data", cfg->sn);
    m->on_command = cfg->on_command;
    m->on_connection = cfg->on_connection;
    m->on_refused = cfg->on_refused;
    m->user = cfg->user;
    pthread_mutex_init(&m->mu, NULL);
    pthread_mutex_init(&m->sleep_mu, NULL);
    rk_cond_init(&m->sleep_cv);
    if (!m->broker || !m->client_id || !m->username || !m->cmd_topic || !m->status_topic ||
        !m->ack_topic || !m->data_topic || (cfg->token && !m->token)) {
        rk_mqtt_destroy(m);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    return m;
}

/* Caller holds mu. */
static bool publish_locked(rk_mqtt *m, const char *topic, const char *payload, bool retain) {
    if (!m->s || !m->connected)
        return false;
    MQTTPublishInfo_t pub = {0};
    pub.qos = MQTTQoS1;
    pub.retain = retain;
    pub.pTopicName = topic;
    pub.topicNameLength = (uint16_t)strlen(topic);
    pub.pPayload = payload;
    pub.payloadLength = strlen(payload);
    MQTTStatus_t st = MQTT_Publish(&m->s->ctx, &pub, MQTT_GetPacketId(&m->s->ctx));
    if (st != MQTTSuccess) {
        /* A broken connection surfaces in the loop's next MQTT_ProcessLoop. */
        RLOG_WARN("mqtt", "publish to %s failed: %s", topic, MQTT_Status_strerror(st));
        return false;
    }
    return true;
}

static void close_locked(rk_mqtt *m) {
    if (m->s) {
        rk_conn_close(m->s->net.conn);
        free(m->s);
        m->s = NULL;
    }
}

static bool connect_once(rk_mqtt *m) {
    char err[256];
    rk_conn *c = rk_conn_open(m->host, m->port, m->tls, CONNECT_TIMEOUT_MS, err, sizeof(err));
    if (!c) {
        RLOG_WARN("mqtt", "connect to %s failed: %s", m->broker, err);
        return false;
    }
    session *s = calloc(1, sizeof(*s));
    if (!s) {
        rk_conn_close(c);
        return false;
    }
    s->net.conn = c;
    s->net.recv_wait_ms = 20;
    TransportInterface_t transport = {0};
    transport.recv = transport_recv;
    transport.send = transport_send;
    transport.pNetworkContext = &s->net;
    MQTTFixedBuffer_t fixed = {s->buffer, sizeof(s->buffer)};
    MQTTStatus_t st = MQTT_Init(&s->ctx, &transport, now_ms32, on_event, &fixed);
    if (st == MQTTSuccess)
        st = MQTT_InitStatefulQoS(&s->ctx, s->outgoing, INFLIGHT_RECORDS, s->incoming,
                                  INFLIGHT_RECORDS);

    pthread_mutex_lock(&m->mu);
    char *token = m->token ? rk_strdup(m->token) : NULL;
    pthread_mutex_unlock(&m->mu);
    MQTTConnectInfo_t ci = {0};
    ci.cleanSession = true;
    ci.keepAliveSeconds = KEEPALIVE_SEC;
    ci.pClientIdentifier = m->client_id;
    ci.clientIdentifierLength = (uint16_t)strlen(m->client_id);
    if (token) {
        ci.pUserName = m->username;
        ci.userNameLength = (uint16_t)strlen(m->username);
        ci.pPassword = token;
        ci.passwordLength = (uint16_t)strlen(token);
    }
    /* Last Will: dropping without a clean disconnect makes the broker publish
     * offline for us. Retained so a late subscriber sees it. */
    MQTTPublishInfo_t will = {0};
    will.qos = MQTTQoS1;
    will.retain = true;
    will.pTopicName = m->status_topic;
    will.topicNameLength = (uint16_t)strlen(m->status_topic);
    will.pPayload = k_offline;
    will.payloadLength = sizeof(k_offline) - 1;
    bool present = false;
    if (st == MQTTSuccess)
        st = MQTT_Connect(&s->ctx, &ci, &will, CONNECT_TIMEOUT_MS, &present);
    free(token);
    if (st == MQTTServerRefused) {
        RLOG_WARN("mqtt", "broker %s refused the connection (device token not accepted?)",
                  m->broker);
        if (m->on_refused)
            m->on_refused(m->user);
    }
    if (st == MQTTSuccess) {
        MQTTSubscribeInfo_t sub = {0};
        sub.qos = MQTTQoS1;
        sub.pTopicFilter = m->cmd_topic;
        sub.topicFilterLength = (uint16_t)strlen(m->cmd_topic);
        st = MQTT_Subscribe(&s->ctx, &sub, 1, MQTT_GetPacketId(&s->ctx));
    }
    if (st != MQTTSuccess) {
        RLOG_WARN("mqtt", "MQTT session with %s failed: %s", m->broker, MQTT_Status_strerror(st));
        rk_conn_close(c);
        free(s);
        return false;
    }
    s->net.recv_wait_ms = 0;

    pthread_mutex_lock(&m->mu);
    m->s = s;
    m->connected = true;
    /* (Re)announce the current state on every (re)connect. */
    if (m->status_json)
        publish_locked(m, m->status_topic, m->status_json, true);
    pthread_mutex_unlock(&m->mu);
    return true;
}

static bool sleep_unless_stopped(rk_mqtt *m, int seconds) {
    uint64_t deadline = rk_now_ms() + (uint64_t)seconds * 1000u;
    pthread_mutex_lock(&m->sleep_mu);
    while (!m->stopping && rk_cond_wait_until(&m->sleep_cv, &m->sleep_mu, deadline)) {
    }
    bool stopped = m->stopping;
    pthread_mutex_unlock(&m->sleep_mu);
    return !stopped;
}

static void *mqtt_thread(void *arg) {
    rk_mqtt *m = arg;
    int delay = 1;
    while (!m->stopping) {
        if (!m->connected) {
            if (!connect_once(m)) {
                if (!sleep_unless_stopped(m, delay))
                    break;
                delay = RK_MIN(delay * 2, MAX_BACKOFF_SEC);
                continue;
            }
            delay = 1; /* a live session resets the backoff */
            RLOG_INFO("mqtt", "connected to %s, subscribed %s", m->broker, m->cmd_topic);
            if (m->on_connection)
                m->on_connection(m->user, true);
        }

        MQTTStatus_t st = MQTTSuccess;
        bool lost = false, buffered = false;
        int fd = -1;
        pthread_mutex_lock(&m->mu);
        if (!m->s) {
            lost = true;
        } else {
            t_current = m;
            st = MQTT_ProcessLoop(&m->s->ctx);
            t_current = NULL;
            if (st != MQTTSuccess && st != MQTTNeedMoreBytes) {
                close_locked(m);
                lost = true;
            } else {
                buffered = rk_conn_has_buffered(m->s->net.conn);
                fd = rk_conn_fd(m->s->net.conn);
            }
        }
        if (lost)
            m->connected = false;
        pthread_mutex_unlock(&m->mu);

        if (lost) {
            if (m->stopping)
                break;
            RLOG_WARN("mqtt", "connection to %s lost (%s) — reconnecting in %ds", m->broker,
                      MQTT_Status_strerror(st), delay);
            if (m->on_connection)
                m->on_connection(m->user, false);
            if (!sleep_unless_stopped(m, delay))
                break;
            delay = RK_MIN(delay * 2, MAX_BACKOFF_SEC);
            continue;
        }
        /* Wait for traffic without the lock. The 500 ms cap keeps keep-alive
         * pings on time and bounds how long stop() waits. */
        if (!buffered)
            rk_poll_fd(fd, POLLIN, 500);
    }
    return NULL;
}

bool rk_mqtt_start(rk_mqtt *m) {
    if (m->started)
        return true;
    m->stopping = false;
    if (pthread_create(&m->thread, NULL, mqtt_thread, m) != 0) {
        RLOG_ERR("mqtt", "cannot start the MQTT thread");
        return false;
    }
    m->started = true;
    RLOG_INFO("mqtt", "control connecting to %s", m->broker);
    return true;
}

void rk_mqtt_set_token(rk_mqtt *m, const char *token) {
    char *copy = token ? rk_strdup(token) : NULL;
    pthread_mutex_lock(&m->mu);
    free(m->token);
    m->token = copy;
    pthread_mutex_unlock(&m->mu);
}

void rk_mqtt_set_status(rk_mqtt *m, const char *json) {
    pthread_mutex_lock(&m->mu);
    char *copy = rk_strdup(json);
    if (copy) {
        free(m->status_json);
        m->status_json = copy;
    }
    publish_locked(m, m->status_topic, json, true);
    pthread_mutex_unlock(&m->mu);
}

void rk_mqtt_publish_ack(rk_mqtt *m, const char *json) {
    pthread_mutex_lock(&m->mu);
    publish_locked(m, m->ack_topic, json, false);
    pthread_mutex_unlock(&m->mu);
}

bool rk_mqtt_publish_data(rk_mqtt *m, const char *json) {
    pthread_mutex_lock(&m->mu);
    bool sent = publish_locked(m, m->data_topic, json, false);
    pthread_mutex_unlock(&m->mu);
    return sent;
}

void rk_mqtt_stop(rk_mqtt *m) {
    if (!m || !m->started)
        return;
    pthread_mutex_lock(&m->sleep_mu);
    m->stopping = true;
    pthread_cond_broadcast(&m->sleep_cv);
    pthread_mutex_unlock(&m->sleep_mu);
    /* The loop notices within one poll interval (or when an in-flight connect
     * returns). Join first so no connect can land after the cleanup. */
    pthread_join(m->thread, NULL);
    m->started = false;
    pthread_mutex_lock(&m->mu);
    if (m->s && m->connected) {
        /* Clean offline status before leaving (the will is for crashes). */
        publish_locked(m, m->status_topic, k_offline, true);
        MQTT_Disconnect(&m->s->ctx);
    }
    close_locked(m);
    m->connected = false;
    pthread_mutex_unlock(&m->mu);
}

void rk_mqtt_destroy(rk_mqtt *m) {
    if (!m)
        return;
    rk_mqtt_stop(m);
    pthread_mutex_destroy(&m->mu);
    pthread_mutex_destroy(&m->sleep_mu);
    pthread_cond_destroy(&m->sleep_cv);
    free(m->broker);
    free(m->client_id);
    free(m->username);
    free(m->token);
    free(m->cmd_topic);
    free(m->status_topic);
    free(m->ack_topic);
    free(m->data_topic);
    free(m->status_json);
    free(m);
}
