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
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

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

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    /* device_id: config value, else a fixed fallback (M0 will wire the real
     * serial source — /Storage/SerialNumber or a shmem offset). */
    const char *device_id = cfg.device_id[0] ? cfg.device_id : "evmu30";

    /* Surface the loaded config — the M0 session called out silent
     * write_enable / unknown-key behaviour. Print this BEFORE the loop so an
     * operator can confirm intent at a glance from journalctl. */
    fprintf(stderr,
            "delta-bridge: config loaded — broker=%s:%d topic_prefix=%s "
            "device_id=%s poll_hz=%d write_enable=%s\n",
            cfg.broker_host, cfg.broker_port, cfg.topic_prefix, device_id,
            cfg.poll_hz, cfg.write_enable ? "true" : "false");

    /* 1. attach shmem first (RW vs RO depending on write_enable); the
     * adapter needs the pointer so it can route commands to bounded writes. */
    struct shmem sm;
    memset(&sm, 0, sizeof(sm));
    sm.shmid = -1;
    int bo = 0;
    while (!g_stop) {
        int rc = cfg.write_enable ? shmem_attach_rw(&sm) : shmem_attach(&sm);
        if (rc == 0)
            break;
        bo = backoff_next(bo);
        fprintf(stderr, "delta-bridge: shmem not ready, retry in %ds\n", bo);
        interruptible_sleep(bo);
    }

    struct mqtt_adapter_config acfg = {
        .broker_host  = cfg.broker_host,
        .broker_port  = cfg.broker_port,
        .broker_user  = cfg.broker_user[0] ? cfg.broker_user : NULL,
        .broker_pass  = cfg.broker_pass[0] ? cfg.broker_pass : NULL,
        .topic_prefix = cfg.topic_prefix,
        .device_id    = device_id,
        .keepalive_s  = 60,
        .write_enable = cfg.write_enable,
        .shm          = cfg.write_enable ? &sm : NULL,
    };
    struct northbound nb;
    mqtt_adapter_create(&nb, &acfg);

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
        int full = (prev.pilot_state == PILOT_UNKNOWN &&
                    prev.voltage_v == 0.0f && prev.current_a == 0.0f);
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

        usleep(period_us);
    }

    /* 5. graceful shutdown */
    nb.shutdown(&nb);
    shmem_release(&sm);
    fprintf(stderr, "delta-bridge: stopped\n");
    return 0;
}
