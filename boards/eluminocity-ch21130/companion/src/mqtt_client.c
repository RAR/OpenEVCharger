#define _POSIX_C_SOURCE 200112L

#include "mqtt_client.h"
#include "mqtt_codec.h"

#include <errno.h>
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
    c->fd = -1;
    c->keepalive_s = 0;
    c->last_send_ms = 0;
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

    c->fd = fd;
    c->keepalive_s = cfg->keepalive_s;
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

int mqtt_client_tick(struct mqtt_client *c, long now_ms)
{
    if (c->fd < 0)
        return -1;

    /* v1: only PINGREQ resets last_send_ms; publishes do not. MQTT 3.1.1
     * §3.1.2.10 lets any control packet reset the timer — keeping PINGREQ-only
     * is a deliberate simplification (wasteful at most, never wrong). */
    if (c->keepalive_s > 0 &&
        now_ms - c->last_send_ms >= (long)c->keepalive_s * 1000 / 2) {
        unsigned char buf[4];
        unsigned char scratch[MQTT_BUF];
        int n = mqtt_encode_pingreq(buf, sizeof(buf));
        if (n < 0 || send_all(c->fd, buf, (size_t)n) != 0) {
            mqtt_client_disconnect(c);
            return -1;
        }
        c->last_send_ms = now_ms;
        ssize_t r = recv(c->fd, scratch, sizeof(scratch), 0);
        if (r <= 0) {
            mqtt_client_disconnect(c);
            return -1;
        }
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
    c->fd = -1;
}
