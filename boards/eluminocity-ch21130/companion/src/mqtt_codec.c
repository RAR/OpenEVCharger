#include "mqtt_codec.h"
#include <string.h>

int mqtt_encode_remlen(unsigned char *out, size_t value)
{
    if (value > 268435455u)        /* MQTT 3.1.1 max remaining length */
        return -1;
    int n = 0;
    do {
        unsigned char b = value & 0x7F;
        value >>= 7;
        if (value)
            b |= 0x80;
        out[n++] = b;
    } while (value && n < 4);
    return n;
}

size_t mqtt_decode_remlen(const unsigned char *in, size_t len, size_t *consumed)
{
    size_t value = 0;
    int    shift = 0;
    size_t i = 0;
    for (; i < len && i < 4; i++) {
        value |= (size_t)(in[i] & 0x7F) << shift;
        shift += 7;
        if (!(in[i] & 0x80)) {
            *consumed = i + 1;
            return value;
        }
    }
    *consumed = 0;
    return (size_t)-1;
}

/* Append a 2-byte-length-prefixed string. Returns bytes written or -1. */
static int put_str(unsigned char *p, size_t room, const char *s)
{
    size_t l = strlen(s);
    if (l > 0xFFFF || room < l + 2)
        return -1;
    p[0] = (unsigned char)(l >> 8);
    p[1] = (unsigned char)(l & 0xFF);
    memcpy(p + 2, s, l);
    return (int)(l + 2);
}

/* Finalize: write fixed header byte + remaining-length in front of a payload
 * already laid out at buf+5. Returns total packet length or -1. */
static int finalize(unsigned char *buf, size_t cap,
                    unsigned char type_flags, size_t payload_len)
{
    unsigned char rl[4];
    int rln = mqtt_encode_remlen(rl, payload_len);
    if (rln < 0)
        return -1;
    size_t total = 1 + rln + payload_len;
    if (total > cap)
        return -1;
    /* shift payload to sit right after the header we are about to write */
    memmove(buf + 1 + rln, buf + 5, payload_len);
    buf[0] = type_flags;
    memcpy(buf + 1, rl, rln);
    return (int)total;
}

int mqtt_encode_connect(unsigned char *buf, size_t cap,
                        const char *client_id,
                        const char *user, const char *pass,
                        int keepalive_s,
                        const char *will_topic, const char *will_msg)
{
    if (cap < 16)
        return -1;
    unsigned char *p = buf + 5;            /* leave room for header */
    size_t room = cap - 5;
    size_t n = 0;
    int r;

    /* variable header: protocol name + level + flags + keepalive */
    static const unsigned char vh_name[] = { 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04 };
    if (room < sizeof(vh_name) + 3)
        return -1;
    memcpy(p, vh_name, sizeof(vh_name));
    n += sizeof(vh_name);

    unsigned char flags = 0x02;            /* clean session */
    int have_will = will_topic && will_msg;
    if (have_will) flags |= 0x04;          /* will flag, QoS 0, not retained */
    if (user)      flags |= 0x80;
    if (pass && user) flags |= 0x40; /* MQTT 3.1.1 §3.1.2.9: password requires username */
    p[n++] = flags;
    p[n++] = (unsigned char)((unsigned)keepalive_s >> 8);
    p[n++] = (unsigned char)((unsigned)keepalive_s & 0xFF);

    /* payload: client id, [will topic, will msg], [user], [pass] */
    r = put_str(p + n, room - n, client_id); if (r < 0) return -1; n += r;
    if (have_will) {
        r = put_str(p + n, room - n, will_topic); if (r < 0) return -1; n += r;
        r = put_str(p + n, room - n, will_msg);   if (r < 0) return -1; n += r;
    }
    if (user) { r = put_str(p + n, room - n, user); if (r < 0) return -1; n += r; }
    if (pass && user) { r = put_str(p + n, room - n, pass); if (r < 0) return -1; n += r; }

    return finalize(buf, cap, 0x10, n);
}

int mqtt_encode_publish(unsigned char *buf, size_t cap,
                        const char *topic, const char *payload, int retain)
{
    if (cap < 8)
        return -1;
    unsigned char *p = buf + 5;
    size_t room = cap - 5;
    size_t n = 0;
    int r = put_str(p, room, topic);       /* QoS 0: no packet id */
    if (r < 0) return -1;
    n += r;
    size_t pl = strlen(payload);
    if (room - n < pl)
        return -1;
    memcpy(p + n, payload, pl);
    n += pl;
    return finalize(buf, cap, retain ? 0x31 : 0x30, n);
}

int mqtt_encode_pingreq(unsigned char *buf, size_t cap)
{
    if (cap < 2) return -1;
    buf[0] = 0xC0; buf[1] = 0x00;
    return 2;
}

int mqtt_encode_disconnect(unsigned char *buf, size_t cap)
{
    if (cap < 2) return -1;
    buf[0] = 0xE0; buf[1] = 0x00;
    return 2;
}

int mqtt_encode_subscribe(unsigned char *buf, size_t cap,
                          unsigned short packet_id, const char *topic)
{
    if (cap < 8)
        return -1;
    unsigned char *p = buf + 5;
    size_t room = cap - 5;
    size_t n = 0;
    p[n++] = (unsigned char)(packet_id >> 8);
    p[n++] = (unsigned char)(packet_id & 0xFF);
    int r = put_str(p + n, room - n, topic);
    if (r < 0) return -1;
    n += r;
    if (room - n < 1) return -1;
    p[n++] = 0x00;                         /* requested QoS 0 */
    return finalize(buf, cap, 0x82, n);
}

int mqtt_parse_connack(const unsigned char *buf, size_t len)
{
    if (len < 4 || buf[0] != 0x20 || buf[1] != 0x02)
        return -1;
    return buf[3] == 0x00 ? 0 : (int)buf[3];
}
