/* MQTT client transport: TCP socket + keepalive over mqtt_codec. Single
 * connection, QoS 0, blocking sends with bounded timeouts. */
#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

struct mqtt_client {
    int   fd;                 /* socket, -1 when disconnected */
    int   keepalive_s;
    long  last_send_ms;       /* for keepalive scheduling */
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
 * failure (caller retries with backoff). */
int  mqtt_client_connect(struct mqtt_client *c, const struct mqtt_config *cfg);

/* Publish QoS 0. Returns 0 on success, -1 on socket error (caller should treat
 * the link as down). */
int  mqtt_client_publish(struct mqtt_client *c, const char *topic,
                         const char *payload, int retain);

/* Housekeeping: send PINGREQ if keepalive is due, drain/ignore inbound packets.
 * `now_ms` is a monotonic millisecond clock. Returns 0 normally, -1 if the link
 * is down. */
int  mqtt_client_tick(struct mqtt_client *c, long now_ms);

/* Send DISCONNECT (best effort) and close the socket. */
void mqtt_client_disconnect(struct mqtt_client *c);

#endif /* MQTT_CLIENT_H */
