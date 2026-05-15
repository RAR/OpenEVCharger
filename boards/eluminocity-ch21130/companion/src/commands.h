/* Shared command handlers — the bounded shmem writes behind both MQTT and
 * HTTP write controls. Validation + range checks live here so the MQTT path
 * (mqtt_adapter.c on_command) and the v0.4 HTTP path (web.c API) produce
 * identical behaviour for the same logical command.
 *
 * Each helper returns 0 on success, -1 on invalid payload (out of range,
 * non-numeric, etc.) and -2 on infrastructure failure (NULL shm pointer or
 * the shmem write itself failing — out-of-band offset / RO mapping). A
 * short human-readable reason is written into `err` (capacity `errcap`) for
 * the caller to surface in JSON / logs.
 */
#ifndef COMMANDS_H
#define COMMANDS_H

#include <stddef.h>
#include <stdint.h>

struct shmem;

/* rated_amps: payload must be a decimal int in [6..30]. Writes OFF_RATED_AMPS
 * as u8. On success, *out_amps receives the value actually written. */
int cs_apply_rated_amps_write(struct shmem *sm,
                              const unsigned char *payload, size_t len,
                              long *out_amps,
                              char *err, size_t errcap);

/* authorize: payload must be exactly "ON" or "OFF" (case-sensitive — matches
 * the MQTT HA convention). Writes OFF_USER_STATE = 1 / 0. On success,
 * *out_on receives 1 for ON, 0 for OFF. */
int cs_apply_authorize_write(struct shmem *sm,
                             const unsigned char *payload, size_t len,
                             int *out_on,
                             char *err, size_t errcap);

/* clear_faults: payload is ignored (HA buttons send "PRESS"). Writes
 * OFF_ALARM_BITMAP = 0. */
int cs_apply_clear_faults_write(struct shmem *sm,
                                char *err, size_t errcap);

#endif /* COMMANDS_H */
