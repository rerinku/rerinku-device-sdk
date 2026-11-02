/* Device-token bookkeeping: HTTP Date parsing, the JWT exp claim, the token
 * lifetime taken from registration (on the server's clock, so a device with a
 * wrong clock still renews in time) and the WHIP status that signals a refused
 * token. A one-shot local HTTP server stands in for the platform. */
#include "http.h"
#include "whip.h"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* {"sub": "SN-1", "typ": "device", "scope": "publish", "exp": 1791851249} */
#define TOKEN "eyJhbGciOiJIUzI1NiJ9." \
              "eyJzdWIiOiAiU04tMSIsICJ0eXAiOiAiZGV2aWNlIiwgInNjb3BlIjogInB1Ymxpc2giLCAiZXhwIjogMTc5MTg1MTI0OX0" \
              ".c2ln"
/* exp - 86400 */
#define ISSUED "Mon, 12 Oct 2026 00:27:29 GMT"

static void test_dates(void) {
    assert(rk_http_parse_date("Sun, 06 Nov 1994 08:49:37 GMT") == 784111777);
    assert(rk_http_parse_date("Tue, 13 Oct 2026 00:27:29 GMT") == 1791851249);
    assert(rk_http_parse_date("Thu, 29 Feb 2024 23:59:59 GMT") == 1709251199);
    assert(rk_http_parse_date("13 Oct 2026") == 0);
    assert(rk_http_parse_date("Tue, 13 Foo 2026 00:27:29 GMT") == 0);
    assert(rk_http_parse_date("Tue, 13 ctN 2026 00:27:29 GMT") == 0);
}

static void test_jwt_exp(void) {
    assert(rk_jwt_exp(TOKEN) == 1791851249);
    assert(rk_jwt_exp("not-a-jwt") == 0);
    assert(rk_jwt_exp(NULL) == 0);
}

typedef struct {
    int listen_fd;
    int port;
    int requests; /* to serve */
} server;

static void serve_one(server *s) {
    int c = accept(s->listen_fd, NULL, NULL);
    assert(c >= 0);
    char req[8192];
    size_t len = 0;
    for (;;) {
        ssize_t n = recv(c, req + len, sizeof(req) - 1 - len, 0);
        assert(n > 0);
        len += (size_t)n;
        req[len] = '\0';
        char *end = strstr(req, "\r\n\r\n");
        if (!end)
            continue;
        const char *cl = strcasestr(req, "Content-Length:");
        size_t body = cl ? strtoul(cl + 15, NULL, 10) : 0;
        if (len >= (size_t)(end + 4 - req) + body)
            break;
    }
    char resp[1024];
    if (strncmp(req, "POST / ", 7) == 0) {
        assert(strstr(req, "X-Rerinku: true"));
        char body[512];
        int bl = snprintf(body, sizeof(body),
                          "{\"deviceToken\":\"%s\",\"whipUrl\":\"http://127.0.0.1:%d/whip/SN-1\","
                          "\"mqttBroker\":\"tcp://127.0.0.1:1\"}",
                          TOKEN, s->port);
        snprintf(resp, sizeof(resp),
                 "HTTP/1.1 200 OK\r\nDate: " ISSUED "\r\nContent-Type: application/json\r\n"
                 "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                 bl, body);
    } else {
        assert(strncmp(req, "POST /whip/SN-1 ", 16) == 0);
        assert(strstr(req, "Authorization: Bearer " TOKEN "\r\n"));
        snprintf(resp, sizeof(resp),
                 "HTTP/1.1 401 Unauthorized\r\nContent-Length: 21\r\nConnection: close\r\n\r\n"
                 "invalid device token\n");
    }
    assert(send(c, resp, strlen(resp), 0) == (ssize_t)strlen(resp));
    close(c);
}

static void *server_main(void *arg) {
    server *s = arg;
    for (int i = 0; i < s->requests; ++i)
        serve_one(s);
    return NULL;
}

static void test_register_and_refused_token(void) {
    server s = {.requests = 2};
    s.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(s.listen_fd, (struct sockaddr *)&a, sizeof(a)) == 0);
    socklen_t al = sizeof(a);
    assert(getsockname(s.listen_fd, (struct sockaddr *)&a, &al) == 0);
    s.port = ntohs(a.sin_port);
    assert(listen(s.listen_fd, 4) == 0);
    pthread_t t;
    assert(pthread_create(&t, NULL, server_main, &s) == 0);

    char endpoint[64], err[256];
    snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%d", s.port);
    rk_identity id = {.endpoint = endpoint, .sn = "SN-1"};
    rk_profile p;
    assert(rk_register(&id, &p, err, sizeof(err)));
    assert(strcmp(p.token, TOKEN) == 0);
    /* exp minus the server's Date, whatever this machine's clock says. */
    assert(p.token_lifetime_sec == 86400);

    char *answer, *resource;
    int status = -1;
    assert(!rk_whip_post(p.whip_url, p.token, "v=0\r\n", &answer, &resource, &status, err,
                         sizeof(err)));
    assert(status == 401);
    assert(strstr(err, "invalid device token"));
    rk_profile_free(&p);

    pthread_join(t, NULL);
    close(s.listen_fd);
}

int main(void) {
    test_dates();
    test_jwt_exp();
    test_register_and_refused_token();
    puts("token_test: ok");
    return 0;
}
