#define _POSIX_C_SOURCE 200112L

#include "mqtt_client.h"
#include "mqtt_codec.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define MQTT_BUF 512

void mqtt_client_init(struct mqtt_client *c)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->next_packet_id = 1;
}

/* Send all `len` bytes; returns 0 on success, -1 on error. */
static int send_all(int fd, const unsigned char *p, size_t len)
{
    while (len) {
        ssize_t w = send(fd, p, len, 0);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

int mqtt_client_connect(struct mqtt_client *c, const struct mqtt_config *cfg)
{
    mqtt_client_init(c);

    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", cfg->port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(cfg->host, portstr, &hints, &res) != 0 || !res)
        return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    /* bounded I/O so a dead broker cannot stall the poll loop */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) {
        close(fd);
        return -1;
    }

    unsigned char buf[MQTT_BUF];
    int n = mqtt_encode_connect(buf, sizeof(buf), cfg->client_id,
                                cfg->user, cfg->pass, cfg->keepalive_s,
                                cfg->will_topic, cfg->will_msg);
    if (n < 0 || send_all(fd, buf, (size_t)n) != 0) {
        close(fd);
        return -1;
    }

    ssize_t r = recv(fd, buf, sizeof(buf), 0);
    if (r < 4 || mqtt_parse_connack(buf, (size_t)r) != 0) {
        close(fd);
        return -1;
    }

    /* Switch to non-blocking so tick() can poll without stalling the loop. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    c->fd            = fd;
    c->keepalive_s   = cfg->keepalive_s;
    return 0;
}

int mqtt_client_publish(struct mqtt_client *c, const char *topic,
                        const char *payload, int retain)
{
    if (c->fd < 0)
        return -1;
    unsigned char buf[MQTT_BUF];
    int n = mqtt_encode_publish(buf, sizeof(buf), topic, payload, retain);
    if (n < 0 || send_all(c->fd, buf, (size_t)n) != 0) {
        mqtt_client_disconnect(c);
        return -1;
    }
    return 0;
}

int mqtt_client_subscribe(struct mqtt_client *c, const char *topic)
{
    if (c->fd < 0)
        return -1;
    unsigned char buf[MQTT_BUF];
    unsigned short pid = c->next_packet_id++;
    if (pid == 0) pid = c->next_packet_id++;     /* skip 0, MQTT-reserved */
    int n = mqtt_encode_subscribe(buf, sizeof(buf), pid, topic);
    if (n < 0 || send_all(c->fd, buf, (size_t)n) != 0) {
        mqtt_client_disconnect(c);
        return -1;
    }
    return 0;
}

void mqtt_client_set_publish_cb(struct mqtt_client *c,
                                mqtt_client_publish_cb cb, void *user)
{
    c->pub_cb   = cb;
    c->pub_user = user;
}

/* Try to peel ONE complete packet off the front of c->rx. Returns:
 *   >0  bytes consumed (caller compacts and may try again)
 *    0  buffer has a partial packet — need more bytes
 *   -1  protocol error — caller should drop the link */
static int try_consume_one(struct mqtt_client *c)
{
    if (c->rx_len < 2)
        return 0;                         /* need at least type + remlen[0] */

    /* Decode remaining length (varint, 1..4 bytes). */
    size_t rl_consumed = 0;
    size_t remlen = mqtt_decode_remlen(c->rx + 1, c->rx_len - 1, &rl_consumed);
    if (rl_consumed == 0) {
        /* If the varint never terminated within the bytes we have and we
         * have at least 4 bytes after the type byte, it's malformed. */
        if (c->rx_len - 1 >= 4)
            return -1;
        return 0;                         /* incomplete varint, wait */
    }
    if (remlen == (size_t)-1)
        return -1;

    size_t pkt_total = 1 + rl_consumed + remlen;
    if (pkt_total > sizeof(c->rx))
        return -1;                        /* would overflow internal buffer */
    if (pkt_total > c->rx_len)
        return 0;                         /* partial — wait for more bytes */

    unsigned char type = c->rx[0] & 0xF0;
    switch (type) {
    case 0x30: { /* PUBLISH */
        const char         *topic;
        size_t              topic_len;
        const unsigned char *payload;
        size_t              payload_len;
        if (mqtt_decode_publish(c->rx, pkt_total, &topic, &topic_len,
                                &payload, &payload_len) == 0) {
            if (c->pub_cb)
                c->pub_cb(c->pub_user, topic, topic_len, payload, payload_len);
        } else {
            fprintf(stderr,
                    "delta-bridge: mqtt: dropped malformed PUBLISH "
                    "(or QoS > 0; we only subscribe at QoS 0)\n");
        }
        break;
    }
    case 0xD0: /* PINGRESP */
    case 0x90: /* SUBACK   */
        /* No bridge-level state to update; the keepalive timer is reset
         * already in the send path. */
        break;
    default:
        fprintf(stderr, "delta-bridge: mqtt: dropping unknown packet 0x%02x\n",
                (unsigned)c->rx[0]);
        break;
    }
    return (int)pkt_total;
}

int mqtt_client_tick(struct mqtt_client *c, long now_ms)
{
    if (c->fd < 0)
        return -1;

    /* 1. Drain whatever the kernel has queued; non-blocking, bail on EAGAIN. */
    for (;;) {
        if (c->rx_len >= sizeof(c->rx)) {
            /* RX buffer full but no complete packet — broker is sending us
             * something we can't handle; safest action is reconnect. */
            fprintf(stderr,
                    "delta-bridge: mqtt: rx buffer full, dropping link\n");
            mqtt_client_disconnect(c);
            return -1;
        }
        ssize_t r = recv(c->fd, c->rx + c->rx_len,
                         sizeof(c->rx) - c->rx_len, MSG_DONTWAIT);
        if (r > 0) {
            c->rx_len += (size_t)r;
            continue;
        }
        if (r == 0) {
            /* peer closed */
            mqtt_client_disconnect(c);
            return -1;
        }
        /* r < 0 */
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            break;
        mqtt_client_disconnect(c);
        return -1;
    }

    /* 2. Try to peel complete packets off the front. */
    while (c->rx_len) {
        int n = try_consume_one(c);
        if (n < 0) {
            mqtt_client_disconnect(c);
            return -1;
        }
        if (n == 0)
            break;
        /* compact */
        if ((size_t)n < c->rx_len)
            memmove(c->rx, c->rx + n, c->rx_len - (size_t)n);
        c->rx_len -= (size_t)n;
    }

    /* 3. Keepalive — same heuristic as v0.2 (every keepalive/2). PINGREQ is
     * the only path that updates last_send_ms; publishes don't. MQTT 3.1.1
     * §3.1.2.10 lets any control packet reset the timer, but pinging slightly
     * too often is harmless. */
    if (c->keepalive_s > 0 &&
        now_ms - c->last_send_ms >= (long)c->keepalive_s * 1000 / 2) {
        unsigned char buf[4];
        int n = mqtt_encode_pingreq(buf, sizeof(buf));
        if (n < 0 || send_all(c->fd, buf, (size_t)n) != 0) {
            mqtt_client_disconnect(c);
            return -1;
        }
        c->last_send_ms = now_ms;
        /* PINGRESP is drained in (1) on a subsequent tick. */
    }
    return 0;
}

void mqtt_client_disconnect(struct mqtt_client *c)
{
    if (c->fd >= 0) {
        unsigned char buf[4];
        int n = mqtt_encode_disconnect(buf, sizeof(buf));
        if (n > 0)
            (void)send_all(c->fd, buf, (size_t)n);
        close(c->fd);
    }
    c->fd     = -1;
    c->rx_len = 0;
    c->pub_cb = NULL;
    c->pub_user = NULL;
}
