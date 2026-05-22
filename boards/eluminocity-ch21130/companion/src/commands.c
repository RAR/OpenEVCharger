/* Shared command-handler implementations. Both mqtt_adapter.c and web.c call
 * these so the same input produces the same shmem write + the same log line.
 *
 * Logging policy: each function emits exactly one stderr line per call
 * (success or failure). Callers don't need to log again — they may, but the
 * trace from this layer alone is enough to debug a misbehaving command from
 * journalctl. The MQTT path historically wrote its own log line; folding
 * that into here means switching to HTTP doesn't lose log coverage. */

#define _POSIX_C_SOURCE 200112L

#include "commands.h"
#include "shmem.h"
#include "shmem_offsets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Parse the payload as a decimal integer in [min..max]. Returns 0 on success
 * and writes *out; -1 if the payload is empty, non-numeric, or out of range.
 *
 * Trailing whitespace is trimmed (some MQTT GUIs append \n; URL form-decoded
 * values may not, but it's harmless either way). */
static int parse_int_payload(const unsigned char *payload, size_t len,
                             long min, long max, long *out)
{
    if (len == 0 || len > 31)
        return -1;
    char buf[32];
    memcpy(buf, payload, len);
    buf[len] = '\0';
    while (len > 0 && (buf[len - 1] == ' '  || buf[len - 1] == '\t' ||
                       buf[len - 1] == '\r' || buf[len - 1] == '\n'))
        buf[--len] = '\0';
    if (len == 0)
        return -1;
    char *endp = NULL;
    long v = strtol(buf, &endp, 10);
    if (!endp || *endp != '\0')
        return -1;
    if (v < min || v > max)
        return -1;
    *out = v;
    return 0;
}

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err && cap)
        snprintf(err, cap, "%s", msg);
}

int cs_apply_rated_amps_write(struct shmem *sm,
                              const unsigned char *payload, size_t len,
                              long *out_amps,
                              char *err, size_t errcap)
{
    long v = 0;
    if (parse_int_payload(payload, len, 6, 30, &v) != 0) {
        set_err(err, errcap, "amps must be int 6..30");
        fprintf(stderr,
                "delta-bridge: rated_amps: payload out of range or invalid "
                "(want int 6..30), ignored\n");
        return -1;
    }
    if (!sm || !sm->writable) {
        set_err(err, errcap, "write disabled (write_enable=false)");
        fprintf(stderr,
                "delta-bridge: rated_amps: write requested but write is "
                "disabled (write_enable=false)\n");
        return -2;
    }
    if (shmem_write_u8(sm, OFF_RATED_AMPS, (uint8_t)v) != 0) {
        set_err(err, errcap, "shmem write failed");
        fprintf(stderr, "delta-bridge: rated_amps: shmem write failed\n");
        return -2;
    }
    if (out_amps)
        *out_amps = v;
    fprintf(stderr, "delta-bridge: rated_amps -> %ld A\n", v);
    return 0;
}

int cs_apply_authorize_write(struct shmem *sm,
                             const unsigned char *payload, size_t len,
                             int *out_on,
                             char *err, size_t errcap)
{
    uint8_t v;
    if (len == 2 && payload[0] == 'O' && payload[1] == 'N')
        v = 1;
    else if (len == 3 &&
             payload[0] == 'O' && payload[1] == 'F' && payload[2] == 'F')
        v = 0;
    else {
        set_err(err, errcap, "state must be ON or OFF");
        fprintf(stderr,
                "delta-bridge: authorize: payload must be 'ON' or 'OFF', "
                "ignored\n");
        return -1;
    }
    if (!sm || !sm->writable) {
        set_err(err, errcap, "write disabled (write_enable=false)");
        fprintf(stderr,
                "delta-bridge: authorize: write requested but write is "
                "disabled (write_enable=false)\n");
        return -2;
    }
    if (shmem_write_u8(sm, OFF_USER_STATE, v) != 0) {
        set_err(err, errcap, "shmem write failed");
        fprintf(stderr, "delta-bridge: authorize: shmem write failed\n");
        return -2;
    }
    if (out_on)
        *out_on = v ? 1 : 0;
    fprintf(stderr, "delta-bridge: authorize -> %s\n", v ? "ON" : "OFF");
    return 0;
}

int cs_apply_clear_faults_write(struct shmem *sm,
                                char *err, size_t errcap)
{
    if (!sm || !sm->writable) {
        set_err(err, errcap, "write disabled (write_enable=false)");
        fprintf(stderr,
                "delta-bridge: clear_faults: write requested but write is "
                "disabled (write_enable=false)\n");
        return -2;
    }
    if (shmem_write_u32_le(sm, OFF_ALARM_BITMAP, 0u) != 0) {
        set_err(err, errcap, "shmem write failed");
        fprintf(stderr, "delta-bridge: clear_faults: shmem write failed\n");
        return -2;
    }
    fprintf(stderr, "delta-bridge: clear_faults: alarm bitmap cleared\n");
    return 0;
}
