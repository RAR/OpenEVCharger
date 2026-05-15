/* Recording test double shared by adapter tests. */
#ifndef FAKE_MQTT_CLIENT_H
#define FAKE_MQTT_CLIENT_H

#include <stddef.h>

#define FAKE_MAX 128
struct fake_pub {
    char topic[160];
    char payload[512];
    int  retain;
};
extern struct fake_pub fake_pubs[FAKE_MAX];
extern int             fake_pub_count;
extern int             fake_connected;

/* Recorded SUBSCRIBE topics — adapter tests assert the wildcard subscribe
 * was issued. */
#define FAKE_SUB_MAX 16
extern char fake_sub_topics[FAKE_SUB_MAX][160];
extern int  fake_sub_count;

/* Reset all recording arrays. Useful between phases of a single test. */
void fake_mqtt_client_reset(void);

/* The most-recently-connected client. Stashed on mqtt_client_connect() so
 * adapter tests can target a specific client for injection without reaching
 * inside the adapter's static ctx. */
struct mqtt_client;
extern struct mqtt_client *fake_last_client;

/* Inject an inbound PUBLISH: synchronously invoke whatever publish-cb the
 * client `c` has installed. Returns 0 if the callback was called, -1 if no
 * callback was registered. */
int fake_mqtt_client_inject_publish(struct mqtt_client *c,
                                    const char *topic,
                                    const char *payload);

#endif /* FAKE_MQTT_CLIENT_H */
