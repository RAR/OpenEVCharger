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

    TEST_MAIN_END();
}
