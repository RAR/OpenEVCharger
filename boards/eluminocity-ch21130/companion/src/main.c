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
#include "web.h"
#include "backoff.h"
#include "rfid.h"
#include "meter.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
/* meter personality uses a plain int for portability of the volatile
 * read in its sleep loop — translate from sig_atomic_t on dispatch. */
static volatile int g_stop_int = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; g_stop_int = 1; }

/* Sleep `s` seconds but wake early on a stop signal. */
static void interruptible_sleep(int s)
{
    for (int i = 0; i < s && !g_stop; i++)
        sleep(1);
}

/* RFID -> MQTT adapter bridge. The reader callback ABI takes (user, uid)
 * but our adapter is single-instance and reaches its ctx via static linkage;
 * `user` is unused. Defined at file scope (not inline) so the build doesn't
 * depend on GCC's nested-function extension. */
static void on_rfid_scan(void *user, const char *uid_hex)
{
    (void)user;
    fprintf(stderr, "delta-bridge: rfid: scan UID=%s\n", uid_hex);
    mqtt_adapter_on_rfid_scan(uid_hex);
}

int main(int argc, char **argv)
{
    /* Arg parser: accept either positional `delta-bridge <path>` (legacy)
     * or `delta-bridge -c <path>`. M0 bench session caught the original
     * implementation taking argv[1] raw, which made `-c foo.conf` open
     * the literal file "-c" instead. */
    const char *conf_path = "/Storage/delta-bridge.conf";
    /* Personality dispatch — when set, we skip the normal MQTT/web/RFID
     * stack and exec a focused replacement for a stock daemon. Default
     * (empty) keeps v0.6 behavior unchanged.
     * Recognised: "meter"  (replaces /root/MeterIC_new)
     * Planned:    "adc"    (replaces /root/Adc)
     *             "pricomm" (replaces /root/Pri_Comm)
     * See docs/14 §7 + docs/16 for design. */
    const char *personality = NULL;
    const char *meter_port  = "/dev/ttyAMA2";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            conf_path = argv[++i];
        } else if (!strncmp(argv[i], "--personality=", 14)) {
            personality = argv[i] + 14;
        } else if (!strcmp(argv[i], "--personality") && i + 1 < argc) {
            personality = argv[++i];
        } else if (!strncmp(argv[i], "--port=", 7)) {
            meter_port = argv[i] + 7;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr,
                    "Usage: delta-bridge [-c <config>] [--personality=<role>]\n"
                    "  default config: /Storage/delta-bridge.conf\n"
                    "  default role:   (none — runs MQTT bridge + opt-in RFID/web)\n"
                    "  --personality=meter   replaces stock /root/MeterIC_new\n"
                    "  --port=<dev>          override UART for personality\n");
            return 0;
        } else if (argv[i][0] != '-') {
            conf_path = argv[i];      /* positional fallback */
        } else {
            fprintf(stderr, "delta-bridge: unknown flag '%s', ignored\n",
                    argv[i]);
        }
    }

    /* SIGTERM/INT must reach both bridge and personality loops. Install
     * handlers early so the personality branch below sees them. */
    struct sigaction sa_early;
    memset(&sa_early, 0, sizeof(sa_early));
    sa_early.sa_handler = on_signal;
    sigaction(SIGTERM, &sa_early, NULL);
    sigaction(SIGINT,  &sa_early, NULL);
    sa_early.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa_early, NULL);

    /* Personality dispatch — handed off entirely; no MQTT/web/RFID setup. */
    if (personality) {
        if (!strcmp(personality, "meter")) {
            fprintf(stderr, "delta-bridge: dispatching to meter personality\n");
            return meter_personality_run(meter_port, &g_stop_int);
        }
        fprintf(stderr,
                "delta-bridge: unknown personality '%s' — exiting\n",
                personality);
        return 64;
    }

    struct config cfg;
    if (config_load(&cfg, conf_path) != 0)
        fprintf(stderr,
                "delta-bridge: no config at '%s', using defaults\n",
                conf_path);

    /* device_id: config value, else a fixed fallback (M0 will wire the real
     * serial source — /Storage/SerialNumber or a shmem offset). */
    const char *device_id = cfg.device_id[0] ? cfg.device_id : "evmu30";

    /* Surface the loaded config — the M0 session called out silent
     * write_enable / unknown-key behaviour. Print this BEFORE the loop so an
     * operator can confirm intent at a glance from journalctl. */
    fprintf(stderr,
            "delta-bridge: config loaded — broker=%s:%d topic_prefix=%s "
            "device_id=%s poll_hz=%d write_enable=%s web_enable=%s "
            "web_port=%d rfid_enable=%s rfid_port=%s\n",
            cfg.broker_host, cfg.broker_port, cfg.topic_prefix, device_id,
            cfg.poll_hz,
            cfg.write_enable ? "true" : "false",
            cfg.web_enable   ? "true" : "false",
            cfg.web_port,
            cfg.rfid_enable     ? "true" : "false",
            cfg.rfid_port);

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
        .rfid_enable  = cfg.rfid_enable,
    };
    struct northbound nb;
    mqtt_adapter_create(&nb, &acfg);

    /* RFID reader — opt-in. v0.6 owns /dev/ttyAMA4 + GPIO/PWM init exactly
     * as stock /root/RFID does (see docs/10 §1). Deploy by replacing
     * /root/RFID with a wrapper that exec's delta-bridge so /root/main
     * starts us in stock's place. Do NOT enable this alongside stock — we
     * would race for the UART and the PWM driver close+reopen kernel bug
     * (docs/09 §1) would crash whichever loses. */
    struct rfid_reader *rdr = NULL;
    if (cfg.rfid_enable) {
        if (rfid_reader_start(&rdr, cfg.rfid_port,
                              on_rfid_scan, NULL) != 0) {
            fprintf(stderr,
                    "delta-bridge: rfid: start failed — continuing without "
                    "RFID\n");
        }
    }

    /* v0.4 embedded web server — opt-in via web_enable. State reads always
     * work; control writes only succeed when write_enable=true (the helpers
     * return -2/503 if shm is NULL). bring_up is best-effort: a port
     * collision logs and skips the server but does NOT bring down the MQTT
     * bridge. */
    struct web_server ws;
    memset(&ws, 0, sizeof(ws));
    ws.listen_fd = -1;
    int web_up = 0;
    if (cfg.web_enable) {
        ws.port      = cfg.web_port;
        ws.web_user  = cfg.web_user;
        ws.web_pass  = cfg.web_pass;
        ws.cfg       = &cfg;
        ws.shm       = cfg.write_enable ? &sm : NULL;
        ws.orig_argv = argv;
        ws.conf_path = conf_path;
        if (web_server_start(&ws) == 0)
            web_up = 1;
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

        /* 5. web tick — drain any pending HTTP requests. Best-effort: a
         * failure here doesn't impact the MQTT loop. */
        if (web_up)
            web_tick(&ws);

        /* 6. rfid tick — non-blocking; cheap when no card is in field. */
        if (rdr)
            rfid_reader_tick(rdr);

        usleep(period_us);
    }

    /* 7. graceful shutdown */
    nb.shutdown(&nb);
    if (web_up)
        web_server_stop(&ws);
    if (rdr)
        rfid_reader_stop(rdr);
    shmem_release(&sm);
    fprintf(stderr, "delta-bridge: stopped\n");
    return 0;
}
