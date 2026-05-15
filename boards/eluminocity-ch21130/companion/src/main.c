#define _GNU_SOURCE

/* delta-bridge — companion MQTT bridge entry point.
 *
 * Loop: attach shmem (retry+backoff) -> init adapter (retry+backoff) ->
 *       every 1/poll_hz s: read state, diff, publish dirty fields, tick.
 * Read-only: never writes the shmem segment, never touches /dev/watchdog. */
#include "config.h"
#include "shmem.h"
#include "charger_state.h"
#include "northbound.h"
#include "mqtt_adapter.h"
#include "backoff.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Sleep `s` seconds but wake early on a stop signal. */
static void interruptible_sleep(int s)
{
    for (int i = 0; i < s && !g_stop; i++)
        sleep(1);
}

int main(int argc, char **argv)
{
    const char *conf_path = (argc > 1) ? argv[1]
                                       : "/Storage/delta-bridge.conf";
    struct config cfg;
    if (config_load(&cfg, conf_path) != 0)
        fprintf(stderr, "delta-bridge: no config at %s, using defaults\n",
                conf_path);

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* device_id: config value, else a fixed fallback (M0 will wire the real
     * serial source — /Storage/SerialNumber or a shmem offset). */
    const char *device_id = cfg.device_id[0] ? cfg.device_id : "evmu30";

    struct mqtt_adapter_config acfg = {
        .broker_host  = cfg.broker_host,
        .broker_port  = cfg.broker_port,
        .broker_user  = cfg.broker_user[0] ? cfg.broker_user : NULL,
        .broker_pass  = cfg.broker_pass[0] ? cfg.broker_pass : NULL,
        .topic_prefix = cfg.topic_prefix,
        .device_id    = device_id,
        .keepalive_s  = 60,
    };
    struct northbound nb;
    mqtt_adapter_create(&nb, &acfg);

    /* 1. attach shmem, retrying — we may have raced Delta's `main` at boot */
    struct shmem sm;
    int bo = 0;
    while (!g_stop && shmem_attach(&sm) != 0) {
        bo = backoff_next(bo);
        fprintf(stderr, "delta-bridge: shmem not ready, retry in %ds\n", bo);
        interruptible_sleep(bo);
    }

    struct charger_state prev, cur;
    charger_state_init(&prev);
    int adapter_up = 0;
    int period_us  = 1000000 / cfg.poll_hz;
    bo = 0;

    while (!g_stop) {
        /* 2. ensure the adapter is up */
        if (!adapter_up) {
            if (nb.init(&nb) == 0) {
                adapter_up = 1;
                bo = 0;
                charger_state_init(&prev);   /* force a full publish */
            } else {
                bo = backoff_next(bo);
                fprintf(stderr, "delta-bridge: broker down, retry in %ds\n", bo);
                interruptible_sleep(bo);
                continue;
            }
        }

        /* 3. read + publish */
        charger_state_read(&cur, &sm);
        unsigned int dirty = charger_state_diff(&prev, &cur);
        int full = (prev.evse_state == EVSE_STATE_UNKNOWN &&
                    prev.voltage_v == 0 && prev.current_a == 0);
        if (full || dirty) {
            if (nb.publish_state(&nb, &cur, dirty, full) != 0) {
                adapter_up = 0;             /* link down -> reconnect + full */
                continue;
            }
        }
        prev = cur;

        /* 4. housekeeping; tick() reporting down also forces reconnect */
        if (nb.tick(&nb) != 0)
            adapter_up = 0;

        (void)now_ms;                       /* used by the real keepalive path */
        usleep(period_us);
    }

    /* 5. graceful shutdown */
    nb.shutdown(&nb);
    shmem_release(&sm);
    fprintf(stderr, "delta-bridge: stopped\n");
    return 0;
}
