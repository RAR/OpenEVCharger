/* MQTT 3.1.1 packet encode/parse — pure functions, no I/O. Every encoder
 * writes into a caller buffer and returns the byte count, or -1 if the buffer
 * is too small. */
#ifndef MQTT_CODEC_H
#define MQTT_CODEC_H

#include <stddef.h>

/* Encode the "remaining length" varint. Returns 1..4 bytes written, or -1 if value exceeds the MQTT maximum (268435455). */
int    mqtt_encode_remlen(unsigned char *out, size_t value);

/* Decode a remaining-length varint from `in` (max `len` bytes available).
 * Sets *consumed to the byte count. Returns the value, or (size_t)-1 on error. */
size_t mqtt_decode_remlen(const unsigned char *in, size_t len, size_t *consumed);

/* CONNECT. `user`/`pass` may be NULL. `will_topic`/`will_msg` may be NULL
 * (no will). keepalive is seconds. Returns packet length or -1. */
int mqtt_encode_connect(unsigned char *buf, size_t cap,
                        const char *client_id,
                        const char *user, const char *pass,
                        int keepalive_s,
                        const char *will_topic, const char *will_msg);

/* PUBLISH, QoS 0. `retain` non-zero sets the retain flag. payload is a
 * NUL-terminated string. Returns packet length or -1. */
int mqtt_encode_publish(unsigned char *buf, size_t cap,
                        const char *topic, const char *payload, int retain);

int mqtt_encode_pingreq(unsigned char *buf, size_t cap);
int mqtt_encode_disconnect(unsigned char *buf, size_t cap);

/* SUBSCRIBE — stub for v1 (read-only). Defined so the v1.1 control path has a
 * seam; encodes a single-topic QoS-0 subscribe. Returns length or -1. */
int mqtt_encode_subscribe(unsigned char *buf, size_t cap,
                          unsigned short packet_id, const char *topic);

/* Parse a CONNACK. Returns 0 if the connection was accepted (return code 0),
 * non-zero otherwise (bad packet or non-zero return code). */
int mqtt_parse_connack(const unsigned char *buf, size_t len);

#endif /* MQTT_CODEC_H */
