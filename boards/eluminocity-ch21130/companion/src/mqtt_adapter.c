#define _POSIX_C_SOURCE 200112L

#include "mqtt_adapter.h"
#include "mqtt_client.h"
#include "charger_state.h"
#include "commands.h"
#include "shmem.h"
#include "shmem_offsets.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long mqtt_adapter_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Single-instance context (the bridge has exactly one adapter). The publish
 * callback gets a void* to this struct, so command handlers can reach the
 * shmem pointer + topic prefix without globals. */
struct adapter_ctx {
    struct mqtt_adapter_config cfg;
    struct mqtt_client         client;
    char availability_topic[160];
    char cmd_prefix[160];          /* "<topic_prefix>/<device_id>/set/" */
    int  connected;
    /* v0.5: last RFID UID + when (CLOCK_MONOTONIC ms) it was seen. Empty
     * uid means "never scanned". Updated from mqtt_adapter_on_rfid_scan();
     * read by the web layer via mqtt_adapter_get_last_scan(). */
    char last_uid[32];
    long last_scan_ms;
};
static struct adapter_ctx g_ctx;

/* --- topic helpers --- */
static void state_topic(const struct adapter_ctx *a, char *out, size_t cap,
                        const char *field)
{
    snprintf(out, cap, "%s/%s/%s", a->cfg.topic_prefix, a->cfg.device_id, field);
}
static void command_topic(const struct adapter_ctx *a, char *out, size_t cap,
                          const char *field)
{
    snprintf(out, cap, "%s/%s/set/%s",
             a->cfg.topic_prefix, a->cfg.device_id, field);
}
static void discovery_topic(const struct adapter_ctx *a, char *out, size_t cap,
                            const char *component, const char *field)
{
    snprintf(out, cap, "homeassistant/%s/delta_%s_%s/config",
             component, a->cfg.device_id, field);
}

/* Stitch the standard HA `device` block into a payload buffer. Returns the
 * number of bytes appended (or 0 on overflow). Kept as a helper so every
 * discovery payload here ships the same identifiers — HA groups entities by
 * `device.identifiers`, and any drift fragments the device card. */
static int append_device_block(char *out, size_t n, size_t cap,
                               const struct adapter_ctx *a)
{
    if (n >= cap) return 0;
    int w = snprintf(out + n, cap - n,
        ",\"device\":{\"identifiers\":[\"delta_%s\"],"
        "\"name\":\"Delta EVMU30 (%s)\",\"manufacturer\":\"Delta\","
        "\"model\":\"EVMU30\"}",
        a->cfg.device_id, a->cfg.device_id);
    return (w > 0 && (size_t)w < cap - n) ? w : 0;
}

/* Publish one HA discovery config. `unit`, `device_class`, `state_class` are
 * optional (NULL -> omit). */
static void publish_discovery(struct adapter_ctx *a, const char *component,
                              const char *field, const char *name,
                              const char *unit, const char *device_class,
                              const char *state_class)
{
    char topic[160], st[160], payload[640];
    discovery_topic(a, topic, sizeof(topic), component, field);
    state_topic(a, st, sizeof(st), field);

    int n = snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\",\"state_topic\":\"%s\","
        "\"unique_id\":\"delta_%s_%s\","
        "\"availability_topic\":\"%s\"",
        name, st, a->cfg.device_id, field, a->availability_topic);
    if (n <= 0 || (size_t)n >= sizeof(payload)) return;
    n += append_device_block(payload, (size_t)n, sizeof(payload), a);
    if (unit && (size_t)n < sizeof(payload))
        n += snprintf(payload + n, sizeof(payload) - n,
                      ",\"unit_of_measurement\":\"%s\"", unit);
    if (device_class && (size_t)n < sizeof(payload))
        n += snprintf(payload + n, sizeof(payload) - n,
                      ",\"device_class\":\"%s\"", device_class);
    if (state_class && (size_t)n < sizeof(payload))
        n += snprintf(payload + n, sizeof(payload) - n,
                      ",\"state_class\":\"%s\"", state_class);
    if ((size_t)n < sizeof(payload))
        snprintf(payload + n, sizeof(payload) - n, "}");

    mqtt_client_publish(&a->client, topic, payload, 1);
}

/* number.set_current — shares the rated_amps state topic with the read-only
 * sensor it replaces. min/max/step pulled from the EVSE's hardware-published
 * J1772 range (6..30 A); the firmware clamps writes below 6, so HA enforcing
 * the same band keeps the UI honest. */
static void publish_discovery_set_current(struct adapter_ctx *a)
{
    char topic[160], st[160], ct[160], payload[640];
    discovery_topic(a, topic, sizeof(topic), "number", "set_current");
    state_topic(a, st, sizeof(st), "rated_amps");   /* reuses RO state topic */
    command_topic(a, ct, sizeof(ct), "rated_amps");

    int n = snprintf(payload, sizeof(payload),
        "{\"name\":\"Set Current\","
        "\"command_topic\":\"%s\",\"state_topic\":\"%s\","
        "\"unique_id\":\"delta_%s_set_current\","
        "\"availability_topic\":\"%s\","
        "\"min\":6,\"max\":30,\"step\":1,\"mode\":\"slider\","
        "\"unit_of_measurement\":\"A\",\"device_class\":\"current\","
        "\"optimistic\":false",
        ct, st, a->cfg.device_id, a->availability_topic);
    if (n <= 0 || (size_t)n >= sizeof(payload)) return;
    n += append_device_block(payload, (size_t)n, sizeof(payload), a);
    if ((size_t)n < sizeof(payload))
        snprintf(payload + n, sizeof(payload) - n, "}");

    mqtt_client_publish(&a->client, topic, payload, 1);
}

/* switch.authorize — payload "ON"/"OFF", state_topic carries the same value
 * (HA needs state_on/state_off so it can render correctly without an
 * extra value_template). */
static void publish_discovery_authorize(struct adapter_ctx *a)
{
    char topic[160], st[160], ct[160], payload[640];
    discovery_topic(a, topic, sizeof(topic), "switch", "authorize");
    state_topic(a, st, sizeof(st), "authorize_state");
    command_topic(a, ct, sizeof(ct), "authorize");

    int n = snprintf(payload, sizeof(payload),
        "{\"name\":\"Authorize\","
        "\"command_topic\":\"%s\",\"state_topic\":\"%s\","
        "\"unique_id\":\"delta_%s_authorize\","
        "\"availability_topic\":\"%s\","
        "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
        "\"state_on\":\"ON\",\"state_off\":\"OFF\","
        "\"optimistic\":false",
        ct, st, a->cfg.device_id, a->availability_topic);
    if (n <= 0 || (size_t)n >= sizeof(payload)) return;
    n += append_device_block(payload, (size_t)n, sizeof(payload), a);
    if ((size_t)n < sizeof(payload))
        snprintf(payload + n, sizeof(payload) - n, "}");

    mqtt_client_publish(&a->client, topic, payload, 1);
}

/* sensor.last_uid — retained UID string. We deliberately do NOT set a
 * device_class (HA has no "rfid" class) so the entity renders as a generic
 * text sensor. No unit. The state topic is `<prefix>/<dev>/rfid/last_uid`
 * — we go via the state_topic helper but with the multi-segment "rfid/last_uid"
 * field name, which works because the helper just appends. */
static void publish_discovery_last_uid(struct adapter_ctx *a)
{
    char topic[160], st[160], payload[640];
    discovery_topic(a, topic, sizeof(topic), "sensor", "last_uid");
    state_topic(a, st, sizeof(st), "rfid/last_uid");

    int n = snprintf(payload, sizeof(payload),
        "{\"name\":\"Last RFID Scan\","
        "\"state_topic\":\"%s\","
        "\"unique_id\":\"delta_%s_last_uid\","
        "\"availability_topic\":\"%s\","
        "\"icon\":\"mdi:nfc-variant\"",
        st, a->cfg.device_id, a->availability_topic);
    if (n <= 0 || (size_t)n >= sizeof(payload)) return;
    n += append_device_block(payload, (size_t)n, sizeof(payload), a);
    if ((size_t)n < sizeof(payload))
        snprintf(payload + n, sizeof(payload) - n, "}");

    mqtt_client_publish(&a->client, topic, payload, 1);
}

/* event.scan — HA "event" component. Each scan_event publish fires a card-
 * scanned event in HA, with the UID in the JSON attributes. Non-retained
 * on the state side so HA's last_triggered timestamp moves each scan. */
static void publish_discovery_scan_event(struct adapter_ctx *a)
{
    char topic[160], st[160], payload[640];
    discovery_topic(a, topic, sizeof(topic), "event", "scan");
    state_topic(a, st, sizeof(st), "rfid/scan_event");

    int n = snprintf(payload, sizeof(payload),
        "{\"name\":\"RFID Scan Event\","
        "\"state_topic\":\"%s\","
        "\"unique_id\":\"delta_%s_scan_event\","
        "\"availability_topic\":\"%s\","
        "\"event_types\":[\"card_scanned\"],"
        "\"icon\":\"mdi:nfc-tap\"",
        st, a->cfg.device_id, a->availability_topic);
    if (n <= 0 || (size_t)n >= sizeof(payload)) return;
    n += append_device_block(payload, (size_t)n, sizeof(payload), a);
    if ((size_t)n < sizeof(payload))
        snprintf(payload + n, sizeof(payload) - n, "}");

    mqtt_client_publish(&a->client, topic, payload, 1);
}

/* button.clear_faults — no state topic. HA buttons publish "PRESS" by
 * convention but we accept anything (and ignore the payload). */
static void publish_discovery_clear_faults(struct adapter_ctx *a)
{
    char topic[160], ct[160], payload[640];
    discovery_topic(a, topic, sizeof(topic), "button", "clear_faults");
    command_topic(a, ct, sizeof(ct), "clear_faults");

    int n = snprintf(payload, sizeof(payload),
        "{\"name\":\"Clear Faults\","
        "\"command_topic\":\"%s\","
        "\"unique_id\":\"delta_%s_clear_faults\","
        "\"availability_topic\":\"%s\"",
        ct, a->cfg.device_id, a->availability_topic);
    if (n <= 0 || (size_t)n >= sizeof(payload)) return;
    n += append_device_block(payload, (size_t)n, sizeof(payload), a);
    if ((size_t)n < sizeof(payload))
        snprintf(payload + n, sizeof(payload) - n, "}");

    mqtt_client_publish(&a->client, topic, payload, 1);
}

static void publish_all_discovery(struct adapter_ctx *a)
{
    /* Metering — full HA SensorDeviceClass attributes for energy dashboards. */
    publish_discovery(a, "sensor", "voltage",    "Voltage",    "V", "voltage",     "measurement");
    publish_discovery(a, "sensor", "current",    "Current",    "A", "current",     "measurement");
    publish_discovery(a, "sensor", "power",      "Power",      "W", "power",       "measurement");

    /* J1772 PWM + ampacity. The rated_amps entity is published as a plain
     * read-only sensor in v0.2 mode; v0.3 with write_enable swaps it for a
     * number entity (see below). The state topic is the same in both cases
     * so existing automations don't break across the toggle. */
    publish_discovery(a, "sensor", "pilot_duty", "Pilot Duty", "%", NULL, "measurement");
    if (!a->cfg.write_enable)
        publish_discovery(a, "sensor", "rated_amps", "Rated Amps",
                          "A", "current", NULL);

    /* Plain enum / scalar state — no unit, no device_class. */
    publish_discovery(a, "sensor", "pilot_state", "Pilot State", NULL, NULL, NULL);
    publish_discovery(a, "sensor", "pri_state",   "Pri State",   NULL, NULL, NULL);
    publish_discovery(a, "sensor", "user_state",  "User State",  NULL, NULL, NULL);
    publish_discovery(a, "sensor", "red_led",     "Red LED",     NULL, NULL, NULL);

    /* STM32 link — binary connectivity sensor. */
    publish_discovery(a, "binary_sensor", "stm32_link_ok", "STM32 Link",
                      NULL, "connectivity", NULL);

    /* Active alarm-bit names (comma-separated, "none" when no bits set). */
    publish_discovery(a, "sensor", "active_faults", "Active Faults", NULL, NULL, NULL);

    /* Writable entities — only when the bridge is configured RW. */
    if (a->cfg.write_enable) {
        publish_discovery_set_current(a);
        publish_discovery_authorize(a);
        publish_discovery_clear_faults(a);
    }

    /* RFID entities — only when the bridge is operating the reader. */
    if (a->cfg.rfid_enable) {
        publish_discovery_last_uid(a);
        publish_discovery_scan_event(a);
    }
}

/* Build the comma-joined list of active fault names (or "none"). Bits are
 * iterated in ascending bit-position order so the string is deterministic. */
static void format_faults(unsigned int bits, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = '\0';
    for (int i = 0; i < CHARGER_MAX_FAULTS; i++) {
        if (!(bits & (1u << i)))
            continue;
        const char *nm = charger_fault_name(i);
        int w = snprintf(out + n, cap - n, "%s%s", n ? "," : "", nm);
        if (w < 0 || (size_t)w >= cap - n)
            break;
        n += (size_t)w;
    }
    if (n == 0)
        snprintf(out, cap, "none");
}

/* --- Command handlers ----------------------------------------------------
 * Invoked from on_command(), which has already isolated the suffix after
 * the last '/' of the topic. Payloads are NOT NUL-terminated — pass the
 * pointer + length explicitly.
 *
 * v0.4: validation + shmem write now live in commands.c so the HTTP API
 * shares the exact same code path. These wrappers just route from the
 * MQTT-side suffix dispatch into those helpers and discard return values
 * (the helpers log success/failure themselves). */

static void handle_rated_amps(struct adapter_ctx *a,
                              const unsigned char *payload, size_t len)
{
    cs_apply_rated_amps_write(a->cfg.shm, payload, len, NULL, NULL, 0);
}

static void handle_authorize(struct adapter_ctx *a,
                             const unsigned char *payload, size_t len)
{
    cs_apply_authorize_write(a->cfg.shm, payload, len, NULL, NULL, 0);
}

static void handle_clear_faults(struct adapter_ctx *a,
                                const unsigned char *payload, size_t len)
{
    (void)payload; (void)len;        /* HA buttons send "PRESS"; we ignore */
    cs_apply_clear_faults_write(a->cfg.shm, NULL, 0);
}

/* Dispatch incoming PUBLISH packets routed by the MQTT client. The topic
 * is NOT NUL-terminated — work with topic+topic_len. We match against the
 * last path segment, which is the human-readable command name. */
static void on_command(void *user,
                       const char *topic, size_t topic_len,
                       const unsigned char *payload, size_t payload_len)
{
    struct adapter_ctx *a = user;

    /* Find the last '/'. */
    size_t slash = topic_len;
    for (size_t i = topic_len; i > 0; i--) {
        if (topic[i - 1] == '/') { slash = i - 1; break; }
    }
    if (slash >= topic_len) {
        fprintf(stderr,
                "delta-bridge: command: topic has no '/', dropped\n");
        return;
    }
    const char *suffix = topic + slash + 1;
    size_t suffix_len = topic_len - slash - 1;

    /* Compare suffix without allocating. */
    #define IS(s) (suffix_len == sizeof(s) - 1 && \
                   memcmp(suffix, (s), sizeof(s) - 1) == 0)
    if (IS("rated_amps"))        handle_rated_amps(a, payload, payload_len);
    else if (IS("authorize"))    handle_authorize(a, payload, payload_len);
    else if (IS("clear_faults")) handle_clear_faults(a, payload, payload_len);
    else {
        fprintf(stderr,
                "delta-bridge: command: unknown set/* suffix '%.*s', ignored\n",
                (int)suffix_len, suffix);
    }
    #undef IS
}

/* --- northbound vtable --- */
static int nb_init(struct northbound *nb)
{
    struct adapter_ctx *a = nb->ctx;
    struct mqtt_config mc = {
        .host = a->cfg.broker_host, .port = a->cfg.broker_port,
        .client_id = a->cfg.device_id,
        .user = a->cfg.broker_user, .pass = a->cfg.broker_pass,
        .keepalive_s = a->cfg.keepalive_s,
        .will_topic = a->availability_topic, .will_msg = "offline",
    };
    if (mqtt_client_connect(&a->client, &mc) != 0)
        return -1;
    mqtt_client_publish(&a->client, a->availability_topic, "online", 1);

    if (a->cfg.write_enable) {
        /* Install dispatch BEFORE subscribing so we don't drop a retained
         * SUBACK-then-publish race. */
        mqtt_client_set_publish_cb(&a->client, on_command, a);
        /* +2 over cmd_prefix to fit "+\0"; sizeof(cmd_prefix)=160 so 162 is safe. */
        char wildcard[sizeof(a->cmd_prefix) + 2];
        snprintf(wildcard, sizeof(wildcard), "%s+", a->cmd_prefix);
        if (mqtt_client_subscribe(&a->client, wildcard) != 0) {
            /* SUBSCRIBE failure means the link is unhealthy; treat as
             * "init failed" and let main.c reconnect. */
            return -1;
        }
    }
    a->connected = 1;
    return 0;
}

static int nb_publish_state(struct northbound *nb,
                            const struct charger_state *cs,
                            unsigned int dirty, int full)
{
    struct adapter_ctx *a = nb->ctx;
    if (!a->connected)
        return -1;

    int err = 0;
    if (full)
        publish_all_discovery(a);    /* discovery failures are best-effort */

    char topic[160], val[384];

    if (full || (dirty & CS_DIRTY_VOLTAGE)) {
        state_topic(a, topic, sizeof(topic), "voltage");
        snprintf(val, sizeof(val), "%.1f", cs->voltage_v);
        if (mqtt_client_publish(&a->client, topic, val, 1) != 0) err = -1;
    }
    if (full || (dirty & CS_DIRTY_CURRENT)) {
        state_topic(a, topic, sizeof(topic), "current");
        snprintf(val, sizeof(val), "%.2f", cs->current_a);
        if (mqtt_client_publish(&a->client, topic, val, 1) != 0) err = -1;
    }
    if (full || (dirty & CS_DIRTY_POWER)) {
        state_topic(a, topic, sizeof(topic), "power");
        snprintf(val, sizeof(val), "%.0f", cs->power_w);
        if (mqtt_client_publish(&a->client, topic, val, 1) != 0) err = -1;
    }
    if (full || (dirty & CS_DIRTY_PILOT_DUTY)) {
        state_topic(a, topic, sizeof(topic), "pilot_duty");
        snprintf(val, sizeof(val), "%u", (unsigned)cs->pilot_duty_pct);
        if (mqtt_client_publish(&a->client, topic, val, 1) != 0) err = -1;
    }
    if (full || (dirty & CS_DIRTY_RATED_AMPS)) {
        state_topic(a, topic, sizeof(topic), "rated_amps");
        snprintf(val, sizeof(val), "%u", (unsigned)cs->rated_amps);
        if (mqtt_client_publish(&a->client, topic, val, 1) != 0) err = -1;
    }
    if (full || (dirty & CS_DIRTY_PILOT_STATE)) {
        state_topic(a, topic, sizeof(topic), "pilot_state");
        if (mqtt_client_publish(&a->client, topic,
                                pilot_state_str(cs->pilot_state), 1) != 0) err = -1;
    }
    if (full || (dirty & CS_DIRTY_PRI_STATE)) {
        state_topic(a, topic, sizeof(topic), "pri_state");
        snprintf(val, sizeof(val), "%u", (unsigned)cs->pri_state);
        if (mqtt_client_publish(&a->client, topic, val, 1) != 0) err = -1;
    }
    if (full || (dirty & CS_DIRTY_USER_STATE)) {
        state_topic(a, topic, sizeof(topic), "user_state");
        snprintf(val, sizeof(val), "%u", (unsigned)cs->user_state);
        if (mqtt_client_publish(&a->client, topic, val, 1) != 0) err = -1;
    }
    if (full || (dirty & CS_DIRTY_RED_LED)) {
        state_topic(a, topic, sizeof(topic), "red_led");
        snprintf(val, sizeof(val), "%u", (unsigned)cs->red_led);
        if (mqtt_client_publish(&a->client, topic, val, 1) != 0) err = -1;
    }
    if (full || (dirty & CS_DIRTY_STM32_LINK)) {
        state_topic(a, topic, sizeof(topic), "stm32_link_ok");
        if (mqtt_client_publish(&a->client, topic,
                                cs->stm32_link_ok ? "ON" : "OFF", 1) != 0) err = -1;
    }
    if (full || (dirty & CS_DIRTY_FAULTS)) {
        state_topic(a, topic, sizeof(topic), "active_faults");
        format_faults(cs->fault_bits, val, sizeof(val));
        if (mqtt_client_publish(&a->client, topic, val, 1) != 0) err = -1;
    }
    /* authorize_state — published whether or not write_enable is on, so HA
     * can show the switch state even if the user toggles write_enable on
     * later. The topic costs ~30 retained bytes; keeping it always-on keeps
     * the bridge's published contract stable across config changes. */
    if (full || (dirty & CS_DIRTY_AUTHORIZE)) {
        state_topic(a, topic, sizeof(topic), "authorize_state");
        if (mqtt_client_publish(&a->client, topic,
                                cs->user_state >= 1 ? "ON" : "OFF", 1) != 0)
            err = -1;
    }
    return err;
}

static int nb_tick(struct northbound *nb)
{
    struct adapter_ctx *a = nb->ctx;
    return mqtt_client_tick(&a->client, mqtt_adapter_now_ms());
}

static void nb_shutdown(struct northbound *nb)
{
    struct adapter_ctx *a = nb->ctx;
    if (a->connected) {
        mqtt_client_publish(&a->client, a->availability_topic, "offline", 1);
        mqtt_client_disconnect(&a->client);
        a->connected = 0;
    }
}

void mqtt_adapter_on_rfid_scan(const char *uid_hex)
{
    struct adapter_ctx *a = &g_ctx;
    if (!a->connected || !uid_hex || !*uid_hex) {
        /* Even when disconnected we record the last seen UID so the /api/state
         * surface stays useful and the next reconnect's discovery republish
         * gets a representative `last_uid` retained value. */
        if (uid_hex && *uid_hex) {
            snprintf(a->last_uid, sizeof(a->last_uid), "%s", uid_hex);
            a->last_scan_ms = mqtt_adapter_now_ms();
        }
        return;
    }
    snprintf(a->last_uid, sizeof(a->last_uid), "%s", uid_hex);
    a->last_scan_ms = mqtt_adapter_now_ms();

    char topic[160], payload[160];

    /* Retained UID. */
    state_topic(a, topic, sizeof(topic), "rfid/last_uid");
    mqtt_client_publish(&a->client, topic, uid_hex, 1);

    /* Non-retained event payload. The HA `event` component reads
     * `event_type` from the JSON. Keep additional fields under the same
     * top-level object so HA exposes them as event attributes. */
    state_topic(a, topic, sizeof(topic), "rfid/scan_event");
    int n = snprintf(payload, sizeof(payload),
                     "{\"event_type\":\"card_scanned\",\"uid\":\"%s\"}",
                     uid_hex);
    if (n > 0 && (size_t)n < sizeof(payload))
        mqtt_client_publish(&a->client, topic, payload, 0);
}

/* Web layer reads via this helper to avoid leaking g_ctx. Returns 1 if a
 * scan has been observed (and fills *uid_out + *ms_ago), 0 otherwise. */
int mqtt_adapter_get_last_scan(char *uid_out, size_t uid_cap, long *ms_ago)
{
    struct adapter_ctx *a = &g_ctx;
    if (a->last_uid[0] == '\0') {
        if (uid_out && uid_cap) uid_out[0] = '\0';
        if (ms_ago) *ms_ago = 0;
        return 0;
    }
    if (uid_out) snprintf(uid_out, uid_cap, "%s", a->last_uid);
    if (ms_ago)  *ms_ago = mqtt_adapter_now_ms() - a->last_scan_ms;
    return 1;
}

int mqtt_adapter_create(struct northbound *nb,
                        const struct mqtt_adapter_config *cfg)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.cfg = *cfg;
    mqtt_client_init(&g_ctx.client);
    snprintf(g_ctx.availability_topic, sizeof(g_ctx.availability_topic),
             "%s/%s/availability", cfg->topic_prefix, cfg->device_id);
    snprintf(g_ctx.cmd_prefix, sizeof(g_ctx.cmd_prefix),
             "%s/%s/set/", cfg->topic_prefix, cfg->device_id);

    nb->ctx           = &g_ctx;
    nb->init          = nb_init;
    nb->publish_state = nb_publish_state;
    nb->tick          = nb_tick;
    nb->shutdown      = nb_shutdown;
    return 0;
}
