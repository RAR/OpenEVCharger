/* Recording test double shared by adapter tests. */
#ifndef FAKE_MQTT_CLIENT_H
#define FAKE_MQTT_CLIENT_H

#define FAKE_MAX 64
struct fake_pub {
    char topic[160];
    char payload[512];
    int  retain;
};
extern struct fake_pub fake_pubs[FAKE_MAX];
extern int             fake_pub_count;
extern int             fake_connected;

#endif /* FAKE_MQTT_CLIENT_H */
