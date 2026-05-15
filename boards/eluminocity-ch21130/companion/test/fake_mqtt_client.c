/* Test double for mqtt_client.h: records publishes so tests can assert topic
 * and payload. Same symbols as src/mqtt_client.c — link one OR the other. */
#include "mqtt_client.h"
#include <string.h>

#define FAKE_MAX 64
struct fake_pub {
    char topic[160];
    char payload[256];
    int  retain;
};
struct fake_pub fake_pubs[FAKE_MAX];
int             fake_pub_count;
int             fake_connected;

void mqtt_client_init(struct mqtt_client *c) { c->fd = -1; }

int mqtt_client_connect(struct mqtt_client *c, const struct mqtt_config *cfg)
{
    (void)cfg;
    c->fd = 1;
    fake_connected = 1;
    fake_pub_count = 0;
    return 0;
}

int mqtt_client_publish(struct mqtt_client *c, const char *topic,
                        const char *payload, int retain)
{
    (void)c;
    if (fake_pub_count >= FAKE_MAX)
        return -1;
    struct fake_pub *p = &fake_pubs[fake_pub_count++];
    strncpy(p->topic, topic, sizeof(p->topic) - 1);
    p->topic[sizeof(p->topic) - 1] = '\0';
    strncpy(p->payload, payload, sizeof(p->payload) - 1);
    p->payload[sizeof(p->payload) - 1] = '\0';
    p->retain = retain;
    return 0;
}

int mqtt_client_tick(struct mqtt_client *c, long now_ms)
{
    (void)c; (void)now_ms;
    return 0;
}

void mqtt_client_disconnect(struct mqtt_client *c)
{
    c->fd = -1;
    fake_connected = 0;
}
