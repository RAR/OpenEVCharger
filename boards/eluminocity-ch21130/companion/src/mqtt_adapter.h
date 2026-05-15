/* MQTT northbound adapter: HA discovery + per-field state publish + LWT. */
#ifndef MQTT_ADAPTER_H
#define MQTT_ADAPTER_H

#include "northbound.h"

struct mqtt_adapter_config {
    const char *broker_host;
    int         broker_port;
    const char *broker_user;   /* may be NULL */
    const char *broker_pass;   /* may be NULL */
    const char *topic_prefix;  /* e.g. "delta-bridge" */
    const char *device_id;     /* e.g. unit serial */
    int         keepalive_s;
};

/* Populate `nb` with the MQTT adapter vtable + a static context built from
 * `cfg`. `cfg` strings must outlive `nb`. Returns 0 on success. */
int mqtt_adapter_create(struct northbound *nb,
                        const struct mqtt_adapter_config *cfg);

#endif /* MQTT_ADAPTER_H */
