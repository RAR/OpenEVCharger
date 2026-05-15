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
    /* v0.5 RFID. When `rfid_enable` is non-zero the adapter publishes HA
     * discovery for `sensor.last_uid` + `event.scan`, and mqtt_adapter_on_rfid_scan()
     * is the publish entry point that the main loop wires to the reader's
     * on_scan callback. */
    int         rfid_enable;
};

/* Populate `nb` with the MQTT adapter vtable + a static context built from
 * `cfg`. `cfg` strings must outlive `nb`. Returns 0 on success. */
int mqtt_adapter_create(struct northbound *nb,
                        const struct mqtt_adapter_config *cfg);

/* Publish an RFID scan: retained `<prefix>/<dev>/rfid/last_uid` + non-retained
 * `<prefix>/<dev>/rfid/scan_event` JSON `{"event_type":"card_scanned","uid":...}`.
 * The adapter is single-instance so this finds it via static ctx; safe to call
 * from any thread that's also the publish thread (we're single-threaded). */
void mqtt_adapter_on_rfid_scan(const char *uid_hex);

/* Web layer hook: copy the most-recent UID into `uid_out` and the
 * monotonic-ms age of the scan into `*ms_ago`. Returns 1 if a scan has been
 * observed (uid_out + *ms_ago valid), 0 otherwise (uid_out="" and *ms_ago=0).
 * Cap of 32 is plenty (20-char UltraLight UID + NUL). */
#include <stddef.h>
int  mqtt_adapter_get_last_scan(char *uid_out, size_t uid_cap, long *ms_ago);

#endif /* MQTT_ADAPTER_H */
