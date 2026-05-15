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

/* SUBSCRIBE — encode a single-topic QoS-0 subscribe (MQTT 3.1.1 §3.8).
 * Returns total packet length or -1 if the buffer is too small. */
int mqtt_encode_subscribe(unsigned char *buf, size_t cap,
                          unsigned short packet_id, const char *topic);

/* Parse a CONNACK. Returns 0 if the connection was accepted (return code 0),
 * non-zero otherwise (bad packet or non-zero return code). */
int mqtt_parse_connack(const unsigned char *buf, size_t len);

/* Decode a PUBLISH packet from a buffer that starts at the fixed header
 * (byte 0 = 0x30..0x3F). On success returns 0 and writes pointers into
 * `buf` for the topic and payload (with their byte counts). The returned
 * pointers are valid for the caller's `buf` lifetime; no allocation.
 *
 * Defensive: rejects QoS > 0 with -1 (the bridge only subscribes at QoS 0,
 * and we don't carry a packet-id parser); rejects truncated buffers with -1
 * before any out-of-bounds read; rejects non-PUBLISH first byte with -1. */
int mqtt_decode_publish(const unsigned char *buf, size_t len,
                        const char **out_topic, size_t *out_topic_len,
                        const unsigned char **out_payload,
                        size_t *out_payload_len);

#endif /* MQTT_CODEC_H */
