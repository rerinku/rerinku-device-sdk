#include "whip.h"

#include "common.h"
#include "http.h"

#include <cJSON.h>

#include <stdio.h>

/* Resolve a (possibly relative) Location against the request URL. */
static char *resolve_location(const char *url, const char *location) {
    if (!location || !*location)
        return NULL;
    if (strstr(location, "://"))
        return rk_strdup(location);
    const char *scheme_end = strstr(url, "://");
    if (location[0] == '/' && scheme_end) {
        const char *host_end = strchr(scheme_end + 3, '/');
        size_t base = host_end ? (size_t)(host_end - url) : strlen(url);
        return rk_sprintf("%.*s%s", (int)base, url, location);
    }
    const char *slash = strrchr(url, '/');
    size_t base = slash ? (size_t)(slash - url) + 1 : strlen(url);
    return rk_sprintf("%.*s%s", (int)base, url, location);
}

bool rk_whip_post(const char *url, const char *token, const char *offer, char **answer,
                  char **resource, int *status, char *err, size_t errlen) {
    *answer = NULL;
    *resource = NULL;
    *status = 0;
    rk_http_request req = {.method = "POST",
                           .url = url,
                           .content_type = "application/sdp",
                           .body = offer,
                           .body_len = strlen(offer),
                           .bearer = token};
    rk_http_response resp;
    if (!rk_http_perform(&req, &resp, err, errlen))
        return false;
    *status = resp.status;
    if (resp.status != 201) {
        snprintf(err, errlen, "WHIP POST expected 201, got %d: %.200s", resp.status,
                 resp.body ? resp.body : "");
        rk_http_response_free(&resp);
        return false;
    }
    *resource = resolve_location(url, resp.location);
    *answer = resp.body;
    resp.body = NULL;
    rk_http_response_free(&resp);
    return true;
}

void rk_whip_delete(const char *resource, const char *token) {
    if (!resource)
        return;
    rk_http_request req = {.method = "DELETE", .url = resource, .bearer = token};
    rk_http_response resp;
    char err[160];
    if (rk_http_perform(&req, &resp, err, sizeof(err)))
        rk_http_response_free(&resp);
}

static char *json_string(const cJSON *obj, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) && item->valuestring && *item->valuestring
               ? rk_strdup(item->valuestring)
               : NULL;
}

bool rk_register(const rk_identity *id, rk_profile *out, char *err, size_t errlen) {
    memset(out, 0, sizeof(*out));
    cJSON *body = cJSON_CreateObject();
    if (!body) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    cJSON_AddStringToObject(body, "sn", id->sn ? id->sn : "");
    cJSON_AddStringToObject(body, "name", id->name ? id->name : "");
    cJSON_AddStringToObject(body, "deviceId", id->device_id ? id->device_id : "");
    cJSON_AddStringToObject(body, "model", id->model ? id->model : "");
    cJSON_AddStringToObject(body, "fwVersion", id->fw_version ? id->fw_version : "");
    cJSON_AddStringToObject(body, "secret", id->secret ? id->secret : "");
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    /* One endpoint, no routing: the platform recognises a registration by the
     * X-Rerinku header on its base URL. */
    size_t n = strlen(id->endpoint);
    while (n > 0 && id->endpoint[n - 1] == '/')
        --n;
    char *url = rk_sprintf("%.*s/", (int)n, id->endpoint);
    if (!json || !url) {
        cJSON_free(json);
        free(url);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    rk_http_request req = {.method = "POST",
                           .url = url,
                           .content_type = "application/json",
                           .body = json,
                           .body_len = strlen(json),
                           .extra_header = "X-Rerinku: true"};
    rk_http_response resp;
    bool ok = rk_http_perform(&req, &resp, err, errlen);
    cJSON_free(json);
    free(url);
    if (!ok)
        return false;
    if (resp.status < 200 || resp.status >= 300) {
        snprintf(err, errlen, "registration: HTTP %d: %.200s", resp.status,
                 resp.body ? resp.body : "");
        rk_http_response_free(&resp);
        return false;
    }
    cJSON *doc = cJSON_ParseWithLength(resp.body, resp.body_len);
    int64_t server_now = resp.date;
    rk_http_response_free(&resp);
    if (!cJSON_IsObject(doc)) {
        cJSON_Delete(doc);
        snprintf(err, errlen, "registration: malformed response");
        return false;
    }
    out->token = json_string(doc, "deviceToken");
    out->whip_url = json_string(doc, "whipUrl");
    out->mqtt_broker = json_string(doc, "mqttBroker");
    out->stun_server = json_string(doc, "stunServer");
    out->turn_server = json_string(doc, "turnServer");
    cJSON_Delete(doc);
    /* Without a Date header fall back to our own clock; a lifetime that comes
     * out absurd (clock years off) is treated as unknown. */
    int64_t exp = rk_jwt_exp(out->token);
    if (!server_now)
        server_now = (int64_t)(rk_wall_us() / 1000000u);
    if (exp > server_now && exp - server_now <= 366 * 86400)
        out->token_lifetime_sec = exp - server_now;
    if (!out->whip_url || !out->mqtt_broker) {
        snprintf(err, errlen, "registration: response lacks %s",
                 out->whip_url ? "mqttBroker" : "whipUrl");
        rk_profile_free(out);
        return false;
    }
    return true;
}

int64_t rk_jwt_exp(const char *token) {
    const char *dot1 = token ? strchr(token, '.') : NULL;
    const char *dot2 = dot1 ? strchr(dot1 + 1, '.') : NULL;
    if (!dot2)
        return 0;
    char *payload = rk_base64url_decode(dot1 + 1, (size_t)(dot2 - dot1 - 1));
    cJSON *j = payload ? cJSON_Parse(payload) : NULL;
    free(payload);
    const cJSON *exp = cJSON_GetObjectItemCaseSensitive(j, "exp");
    int64_t v = cJSON_IsNumber(exp) ? (int64_t)exp->valuedouble : 0;
    cJSON_Delete(j);
    return v;
}

void rk_profile_free(rk_profile *p) {
    free(p->token);
    free(p->whip_url);
    free(p->mqtt_broker);
    free(p->stun_server);
    free(p->turn_server);
    memset(p, 0, sizeof(*p));
}
