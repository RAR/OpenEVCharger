#include <string.h>
#include "test_harness.h"
#include "mqtt_codec.h"

int main(void)
{
    unsigned char buf[256];
    int n;

    /* --- CONNECT, no auth, keepalive 60, client id "x" --- */
    n = mqtt_encode_connect(buf, sizeof(buf), "x", NULL, NULL, 60,
                            "delta-bridge/x/availability", "offline");
    CHECK(n > 0);
    CHECK_EQ(buf[0], 0x10);                 /* CONNECT packet type */
    /* variable header protocol name "MQTT" */
    CHECK_EQ(buf[2], 0x00); CHECK_EQ(buf[3], 0x04);
    CHECK_EQ(buf[4], 'M'); CHECK_EQ(buf[5], 'Q');
    CHECK_EQ(buf[6], 'T'); CHECK_EQ(buf[7], 'T');
    CHECK_EQ(buf[8], 0x04);                 /* protocol level 4 (3.1.1) */

    /* --- remaining-length codec round-trips multi-byte values --- */
    unsigned char rl[4];
    int rln = mqtt_encode_remlen(rl, 321);
    CHECK_EQ(rln, 2);
    size_t consumed = 0;
    CHECK_EQ(mqtt_decode_remlen(rl, rln, &consumed), 321);
    CHECK_EQ(consumed, 2);
    CHECK(mqtt_encode_remlen(rl, 300000000u) < 0);   /* past MQTT max -> error */

    /* --- PUBLISH, QoS 0, retained --- */
    n = mqtt_encode_publish(buf, sizeof(buf), "delta-bridge/x/voltage",
                            "120", 1 /*retain*/);
    CHECK(n > 0);
    CHECK_EQ(buf[0], 0x31);                 /* PUBLISH | retain */

    /* --- PINGREQ --- */
    n = mqtt_encode_pingreq(buf, sizeof(buf));
    CHECK_EQ(n, 2);
    CHECK_EQ(buf[0], 0xC0);
    CHECK_EQ(buf[1], 0x00);

    /* --- DISCONNECT --- */
    n = mqtt_encode_disconnect(buf, sizeof(buf));
    CHECK_EQ(n, 2);
    CHECK_EQ(buf[0], 0xE0);

    /* --- CONNACK parse: 20 02 00 00 = accepted --- */
    unsigned char connack_ok[]  = { 0x20, 0x02, 0x00, 0x00 };
    unsigned char connack_bad[] = { 0x20, 0x02, 0x00, 0x05 };
    CHECK_EQ(mqtt_parse_connack(connack_ok, sizeof(connack_ok)), 0);
    CHECK(mqtt_parse_connack(connack_bad, sizeof(connack_bad)) != 0);

    /* truncated buffer is rejected, not over-read */
    CHECK(mqtt_encode_connect(buf, 4, "x", NULL, NULL, 60, NULL, NULL) < 0);

    /* --- SUBSCRIBE: byte-exact wire format (MQTT 3.1.1 §3.8) ---
     * topic="a/b" (3 bytes), packet_id=0x0034, QoS 0.
     * Expected bytes (10 total):
     *   buf[0] = 0x82            type=SUBSCRIBE, flags=0010
     *   buf[1] = 0x08            remaining length (2 pkt id + 2 topic len
     *                            + 3 topic + 1 qos = 8)
     *   buf[2..3] = 0x00 0x34    packet id
     *   buf[4..5] = 0x00 0x03    topic length
     *   buf[6..8] = 'a' '/' 'b'  topic
     *   buf[9]    = 0x00         requested QoS 0
     */
    n = mqtt_encode_subscribe(buf, sizeof(buf), 0x0034, "a/b");
    CHECK_EQ(n, 10);
    CHECK_EQ(buf[0], 0x82);
    CHECK_EQ(buf[1], 0x08);
    CHECK_EQ(buf[2], 0x00); CHECK_EQ(buf[3], 0x34);
    CHECK_EQ(buf[4], 0x00); CHECK_EQ(buf[5], 0x03);
    CHECK_EQ(buf[6], 'a');  CHECK_EQ(buf[7], '/'); CHECK_EQ(buf[8], 'b');
    CHECK_EQ(buf[9], 0x00);

    /* SUBSCRIBE: small-buffer rejection returns -1 (the cap-check in the
     * encoder bails before any partial write). */
    {
        unsigned char tiny[4];
        CHECK(mqtt_encode_subscribe(tiny, sizeof(tiny), 1, "a/b") < 0);
    }

    /* --- PUBLISH decode: QoS 0 ---
     * Build a PUBLISH packet using the encoder we already have, then
     * round-trip through the decoder. */
    {
        unsigned char pkt[64];
        int pn = mqtt_encode_publish(pkt, sizeof(pkt),
                                     "delta-bridge/x/set/rated_amps", "20",
                                     0 /*not retained*/);
        CHECK(pn > 0);
        CHECK_EQ(pkt[0], 0x30);                /* QoS 0, no retain */

        const char         *topic; size_t topic_len;
        const unsigned char *pl;   size_t pl_len;
        CHECK_EQ(mqtt_decode_publish(pkt, (size_t)pn,
                                     &topic, &topic_len, &pl, &pl_len), 0);
        CHECK_EQ((long)topic_len, (long)strlen("delta-bridge/x/set/rated_amps"));
        CHECK(memcmp(topic, "delta-bridge/x/set/rated_amps", topic_len) == 0);
        CHECK_EQ((long)pl_len, 2);
        CHECK(memcmp(pl, "20", 2) == 0);
    }

    /* PUBLISH decode: QoS > 0 rejected (we never subscribe at >0). */
    {
        /* Hand-crafted: type|qos1 = 0x32, remlen, topic "t", payload "p".
         * QoS-1 packets also have a 2-byte packet id between topic and
         * payload — but our decoder bails on the QoS check before that. */
        unsigned char qos1[] = {
            0x32, 0x06, 0x00, 0x01, 't', 0x00, 0x01, 'p'
        };
        const char         *t; size_t tl;
        const unsigned char *p; size_t pl;
        CHECK(mqtt_decode_publish(qos1, sizeof(qos1), &t, &tl, &p, &pl) < 0);
    }

    /* PUBLISH decode: non-PUBLISH first byte rejected. */
    {
        unsigned char other[] = { 0xC0, 0x00 };
        const char         *t; size_t tl;
        const unsigned char *p; size_t pl;
        CHECK(mqtt_decode_publish(other, sizeof(other), &t, &tl, &p, &pl) < 0);
    }

    /* PUBLISH decode: truncated buffer is rejected, no OOB read. */
    {
        /* "Says remlen 0x20 but we only give 4 bytes total." */
        unsigned char trunc[] = { 0x30, 0x20, 0x00, 0x01 };
        const char         *t; size_t tl;
        const unsigned char *p; size_t pl;
        CHECK(mqtt_decode_publish(trunc, sizeof(trunc), &t, &tl, &p, &pl) < 0);
    }
    /* PUBLISH decode: short variable header (no room for topic-length u16). */
    {
        unsigned char shorty[] = { 0x30, 0x01, 0x00 };
        const char         *t; size_t tl;
        const unsigned char *p; size_t pl;
        CHECK(mqtt_decode_publish(shorty, sizeof(shorty), &t, &tl, &p, &pl) < 0);
    }
    /* PUBLISH decode: topic length lies (claims more than remlen-2). */
    {
        /* remlen=4, topic_len=0x10 (16) > remlen-2 (2) → reject */
        unsigned char liar[] = { 0x30, 0x04, 0x00, 0x10, 't', 't' };
        const char         *t; size_t tl;
        const unsigned char *p; size_t pl;
        CHECK(mqtt_decode_publish(liar, sizeof(liar), &t, &tl, &p, &pl) < 0);
    }

    TEST_MAIN_END();
}
