/* MQTT client transport: TCP socket + keepalive over mqtt_codec. Single
 * connection, QoS 0, blocking sends with bounded timeouts. RX is non-blocking
 * and parsed packet-by-packet on each tick; any received PUBLISH packets are
 * surfaced via the publish callback. */
#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <stddef.h>

/* Callback invoked synchronously from mqtt_client_tick() for each fully
 * decoded incoming PUBLISH. Pointers live in the client's internal RX buffer
 * and are valid only for the duration of the callback. */
typedef void (*mqtt_client_publish_cb)(void *user,
                                       const char *topic, size_t topic_len,
                                       const unsigned char *payload,
                                       size_t payload_len);

#define MQTT_CLIENT_RX_BUF 1024

struct mqtt_client {
    int           fd;                 /* socket, -1 when disconnected */
    int           keepalive_s;
    long          last_send_ms;       /* for keepalive scheduling */
    unsigned short next_packet_id;    /* monotonic; wraps */
    /* RX parse buffer: append on each tick, peel complete packets off the
     * front. Bounded so a misbehaving broker can't grow it unbounded. */
    unsigned char rx[MQTT_CLIENT_RX_BUF];
    size_t        rx_len;
    /* Publish-dispatch callback. NULL = drop. */
    mqtt_client_publish_cb pub_cb;
    void                  *pub_user;
};

/* Config passed to mqtt_client_connect(). */
struct mqtt_config {
    const char *host;
    int         port;
    const char *client_id;
    const char *user;         /* may be NULL */
    const char *pass;         /* may be NULL */
    int         keepalive_s;
    const char *will_topic;   /* may be NULL */
    const char *will_msg;     /* may be NULL */
};

void mqtt_client_init(struct mqtt_client *c);

/* Connect TCP, send CONNECT, wait for CONNACK. Returns 0 on success, -1 on any
 * failure (caller retries with backoff). The CONNACK recv is bounded by the
 * 5-second socket timeout established here. Post-CONNACK the socket is set to
 * non-blocking so mqtt_client_tick() can poll without stalling. */
int  mqtt_client_connect(struct mqtt_client *c, const struct mqtt_config *cfg);

/* Publish QoS 0. Returns 0 on success, -1 on socket error (caller should treat
 * the link as down). */
int  mqtt_client_publish(struct mqtt_client *c, const char *topic,
                         const char *payload, int retain);

/* SUBSCRIBE — single-topic QoS 0. Returns 0 if the SUBSCRIBE frame was
 * accepted by the socket layer. SUBACK is not awaited (the client drains it
 * from tick() and drops it). */
int  mqtt_client_subscribe(struct mqtt_client *c, const char *topic);

/* Register a publish-dispatch callback. Must be called before the first
 * incoming PUBLISH or that packet is dropped. */
void mqtt_client_set_publish_cb(struct mqtt_client *c,
                                mqtt_client_publish_cb cb, void *user);

/* Housekeeping: send PINGREQ if keepalive is due, drain/dispatch inbound
 * packets. `now_ms` is a monotonic millisecond clock. Returns 0 normally,
 * -1 if the link is down (caller should reconnect). */
int  mqtt_client_tick(struct mqtt_client *c, long now_ms);

/* Send DISCONNECT (best effort) and close the socket. */
void mqtt_client_disconnect(struct mqtt_client *c);

#endif /* MQTT_CLIENT_H */
