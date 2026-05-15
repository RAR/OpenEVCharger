/* Test double for mqtt_client.h: records publishes/subscribes so tests can
 * assert behaviour, and lets tests synthesise inbound PUBLISHes by invoking
 * the adapter-installed callback. Same symbols as src/mqtt_client.c — link
 * one OR the other. */
#include "mqtt_client.h"
#include "fake_mqtt_client.h"
#include <string.h>

struct fake_pub fake_pubs[FAKE_MAX];
int             fake_pub_count;
int             fake_connected;

char fake_sub_topics[FAKE_SUB_MAX][160];
int  fake_sub_count;

struct mqtt_client *fake_last_client;

void fake_mqtt_client_reset(void)
{
    fake_pub_count = 0;
    fake_sub_count = 0;
    memset(fake_pubs, 0, sizeof(fake_pubs));
    memset(fake_sub_topics, 0, sizeof(fake_sub_topics));
}

void mqtt_client_init(struct mqtt_client *c)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

int mqtt_client_connect(struct mqtt_client *c, const struct mqtt_config *cfg)
{
    (void)cfg;
    c->fd = 1;
    fake_connected  = 1;
    fake_last_client = c;
    fake_pub_count  = 0;
    fake_sub_count  = 0;
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

int mqtt_client_subscribe(struct mqtt_client *c, const char *topic)
{
    (void)c;
    if (fake_sub_count >= FAKE_SUB_MAX)
        return -1;
    strncpy(fake_sub_topics[fake_sub_count],
            topic, sizeof(fake_sub_topics[0]) - 1);
    fake_sub_topics[fake_sub_count][sizeof(fake_sub_topics[0]) - 1] = '\0';
    fake_sub_count++;
    return 0;
}

void mqtt_client_set_publish_cb(struct mqtt_client *c,
                                mqtt_client_publish_cb cb, void *user)
{
    c->pub_cb   = cb;
    c->pub_user = user;
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

int fake_mqtt_client_inject_publish(struct mqtt_client *c,
                                    const char *topic,
                                    const char *payload)
{
    if (!c->pub_cb)
        return -1;
    c->pub_cb(c->pub_user,
              topic, strlen(topic),
              (const unsigned char *)payload, strlen(payload));
    return 0;
}
