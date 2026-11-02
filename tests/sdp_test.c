/* Offer generation and answer parsing against a pion-style answer, plus the
 * URL parser used by HTTP and MQTT. */
#include "net.h"
#include "rerinku/rerinku_device.h"
#include "sdp.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char k_ice[] = "a=ice-ufrag:abcd\r\na=ice-pwd:0123456789abcdef012345\r\n"
                            "a=candidate:1 1 UDP 2122317823 192.168.1.100 50000 typ host\r\n"
                            "a=end-of-candidates\r\na=ice-options:ice2\r\n";

static const char k_answer[] =
    "v=0\r\no=- 1 2 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
    "a=fingerprint:sha-256 0A:1B:2C\r\na=group:BUNDLE 0 1\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 102\r\nc=IN IP4 0.0.0.0\r\n"
    "a=setup:active\r\na=mid:0\r\na=ice-ufrag:srvu\r\na=ice-pwd:srvpwdsrvpwdsrvpwdsrvpw\r\n"
    "a=rtcp-mux\r\na=rtpmap:102 H264/90000\r\n"
    "a=candidate:2 1 udp 2130706431 192.168.1.139 40000 typ host\r\na=end-of-candidates\r\n"
    "a=recvonly\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 8\r\nc=IN IP4 0.0.0.0\r\n"
    "a=setup:active\r\na=mid:1\r\na=ice-ufrag:srvu\r\na=ice-pwd:srvpwdsrvpwdsrvpwdsrvpw\r\n"
    "a=rtpmap:8 PCMA/8000\r\n"
    "a=candidate:2 1 udp 2130706431 192.168.1.139 40000 typ host\r\na=recvonly\r\n";

static void test_offer_and_answer(void) {
    rk_sdp_track tracks[2] = {
        {.mid = "0", .kind = RERINKU_MEDIA_KIND_VIDEO, .codec = RERINKU_CODEC_H264, .payload_type = 96,
         .ssrc = 1001, .cname = "rerinku-0", .stream_id = "main", .track_id = "0"},
        {.mid = "1", .kind = RERINKU_MEDIA_KIND_AUDIO, .codec = RERINKU_CODEC_G711A, .payload_type = 8,
         .ssrc = 1002, .cname = "rerinku-1", .stream_id = "main", .track_id = "1"},
    };
    char *offer = rk_sdp_build_offer(tracks, 2, k_ice, "sha-256 AA:BB");
    assert(offer);
    assert(strstr(offer, "a=group:BUNDLE 0 1\r\n"));
    assert(strstr(offer, "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"));
    assert(strstr(offer, "a=rtpmap:96 H264/90000\r\n"));
    assert(strstr(offer, "packetization-mode=1"));
    assert(strstr(offer, "a=rtcp-fb:96 nack pli\r\n"));
    assert(strstr(offer, "a=rtpmap:8 PCMA/8000\r\n"));
    assert(strstr(offer, "a=msid:main 0\r\n") && strstr(offer, "a=ssrc:1002 msid:main 1\r\n"));
    assert(strstr(offer, "a=setup:actpass\r\n") && strstr(offer, "a=fingerprint:sha-256 AA:BB"));
    /* Credentials in every section, candidates once (BUNDLE). */
    char *first = strstr(offer, "a=candidate:");
    assert(first && !strstr(first + 1, "a=candidate:"));
    char *audio = strstr(offer, "m=audio");
    assert(audio && strstr(audio, "a=ice-ufrag:abcd"));
    free(offer);

    rk_sdp_answer ans;
    char err[128];
    assert(rk_sdp_parse_answer(k_answer, tracks, 2, &ans, err, sizeof(err)));
    assert(!strcmp(ans.fingerprint, "sha-256 0A:1B:2C"));
    assert(!strcmp(ans.setup, "active"));
    assert(ans.media_count == 2);
    assert(!strcmp(ans.media[0].mid, "0") && ans.media[0].payload_type == 102);
    assert(!strcmp(ans.media[1].mid, "1") && ans.media[1].payload_type == 8);
    assert(!ans.media[0].rejected);
    /* ICE for the bundle: one set of credentials, the first section's candidates. */
    assert(strstr(ans.ice, "a=ice-ufrag:srvu\r\n") &&
           !strstr(strstr(ans.ice, "a=ice-ufrag:") + 1, "a=ice-ufrag:"));
    assert(strstr(ans.ice, "a=candidate:2 1 udp"));
    assert(!strstr(strstr(ans.ice, "a=candidate:") + 1, "a=candidate:"));
    rk_sdp_answer_free(&ans);

    /* No fingerprint: rejected. */
    assert(!rk_sdp_parse_answer("v=0\r\na=ice-ufrag:x\r\na=ice-pwd:y\r\n", tracks, 2, &ans, err,
                                sizeof(err)));
}

static void test_url(void) {
    rk_url u;
    assert(rk_url_parse("http://192.168.1.10:8080", &u));
    assert(!strcmp(u.scheme, "http") && !strcmp(u.host, "192.168.1.10") &&
           !strcmp(u.port, "8080") && !strcmp(u.path, "/") && !u.tls);
    rk_url_free(&u);
    assert(rk_url_parse("HTTPS://example.com/whip/cam?token=1", &u));
    assert(!strcmp(u.port, "443") && !strcmp(u.path, "/whip/cam?token=1") && u.tls);
    rk_url_free(&u);
    assert(rk_url_parse("mqtts://broker", &u) && !strcmp(u.port, "8883") && u.tls);
    rk_url_free(&u);
    assert(rk_url_parse("tcp://[fd00::1]:1883", &u));
    assert(!strcmp(u.host, "fd00::1") && !strcmp(u.port, "1883"));
    rk_url_free(&u);
    assert(!rk_url_parse("ftp://host", &u));
    assert(!rk_url_parse("no-scheme", &u));
    assert(!rk_url_parse("http://", &u));
    assert(!rk_url_parse("tcp://[::1", &u));
}

int main(void) {
    test_offer_and_answer();
    test_url();
    printf("sdp ok\n");
    return 0;
}
