/* MQTT northbound adapter: HA discovery + per-field state publish + LWT.
 * v0.3 adds write controls (rated_amps / authorize / clear_faults). When
 * `write_enable` is non-zero, the adapter:
 *   - replaces the read-only `sensor.rated_amps` discovery payload with a
 *     `number.set_current` that uses the same state topic + a command topic;
 *   - publishes `switch.authorize` and `button.clear_faults` discovery;
 *   - subscribes to `<topic_prefix>/<device_id>/set/+` and dispatches commands
 *     to bounded shmem writes via the supplied `shmem *`.
 */
#ifndef MQTT_ADAPTER_H
#define MQTT_ADAPTER_H

#include "northbound.h"

struct shmem;

struct mqtt_adapter_config {
    const char *broker_host;
    int         broker_port;
    const char *broker_user;   /* may be NULL */
    const char *broker_pass;   /* may be NULL */
    const char *topic_prefix;  /* e.g. "delta-bridge" */
    const char *device_id;     /* e.g. unit serial */
    int         keepalive_s;
    /* v0.3 write controls. write_enable=0 preserves v0.2 read-only behaviour:
     * no subscribe, no command dispatch, and `sensor.rated_amps` is the
     * advertised HA entity for the rated-amps topic. write_enable=1 swaps to
     * the writable trio (number.set_current / switch.authorize /
     * button.clear_faults) and uses `shm` for bounds-checked shmem writes.
     * When write_enable=1 the caller MUST pass a non-NULL `shm` that is
     * writable (typically attached via shmem_attach_rw). */
    int         write_enable;
    struct shmem *shm;
};

/* Populate `nb` with the MQTT adapter vtable + a static context built from
 * `cfg`. `cfg` strings must outlive `nb`. Returns 0 on success. */
int mqtt_adapter_create(struct northbound *nb,
                        const struct mqtt_adapter_config *cfg);

#endif /* MQTT_ADAPTER_H */
