#define _POSIX_C_SOURCE 200112L

#include "mqtt_adapter.h"
#include "mqtt_client.h"
#include "charger_state.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static long mqtt_adapter_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Static single-instance context (the bridge has exactly one adapter). */
struct adapter_ctx {
    struct mqtt_adapter_config cfg;
    struct mqtt_client         client;
    char availability_topic[160];
    int  connected;
};
static struct adapter_ctx g_ctx;

/* --- topic helpers --- */
static void state_topic(const struct adapter_ctx *a, char *out, size_t cap,
                        const char *field)
{
    snprintf(out, cap, "%s/%s/%s", a->cfg.topic_prefix, a->cfg.device_id, field);
}
static void discovery_topic(const struct adapter_ctx *a, char *out, size_t cap,
                            const char *component, const char *field)
{
    snprintf(out, cap, "homeassistant/%s/delta_%s_%s/config",
             component, a->cfg.device_id, field);
}

/* Publish one HA discovery config. `unit`, `device_class`, `state_class` are
 * optional (NULL -> omit). */
static void publish_discovery(struct adapter_ctx *a, const char *component,
                              const char *field, const char *name,
                              const char *unit, const char *device_class,
                              const char *state_class)
{
    char topic[160], st[160], payload[512];
    discovery_topic(a, topic, sizeof(topic), component, field);
    state_topic(a, st, sizeof(st), field);

    int n = snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\",\"state_topic\":\"%s\","
        "\"unique_id\":\"delta_%s_%s\","
        "\"availability_topic\":\"%s\","
        "\"device\":{\"identifiers\":[\"delta_%s\"],"
        "\"name\":\"Delta EVMU30 (%s)\",\"manufacturer\":\"Delta\","
        "\"model\":\"EVMU30\"}",
        name, st, a->cfg.device_id, field, a->availability_topic,
        a->cfg.device_id, a->cfg.device_id);
    if (unit && n > 0 && n < (int)sizeof(payload))
        n += snprintf(payload + n, sizeof(payload) - n,
                      ",\"unit_of_measurement\":\"%s\"", unit);
    if (device_class && n > 0 && n < (int)sizeof(payload))
        n += snprintf(payload + n, sizeof(payload) - n,
                      ",\"device_class\":\"%s\"", device_class);
    if (state_class && n > 0 && n < (int)sizeof(payload))
        n += snprintf(payload + n, sizeof(payload) - n,
                      ",\"state_class\":\"%s\"", state_class);
    if (n > 0 && n < (int)sizeof(payload))
        snprintf(payload + n, sizeof(payload) - n, "}");

    mqtt_client_publish(&a->client, topic, payload, 1);
}

static void publish_all_discovery(struct adapter_ctx *a)
{
    /* Metering — full HA SensorDeviceClass attributes for energy dashboards. */
    publish_discovery(a, "sensor", "voltage",    "Voltage",    "V", "voltage",     "measurement");
    publish_discovery(a, "sensor", "current",    "Current",    "A", "current",     "measurement");
    publish_discovery(a, "sensor", "power",      "Power",      "W", "power",       "measurement");

    /* J1772 PWM + ampacity */
    publish_discovery(a, "sensor", "pilot_duty", "Pilot Duty", "%", NULL, "measurement");
    publish_discovery(a, "sensor", "rated_amps", "Rated Amps", "A", "current", NULL);

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

int mqtt_adapter_create(struct northbound *nb,
                        const struct mqtt_adapter_config *cfg)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.cfg = *cfg;
    mqtt_client_init(&g_ctx.client);
    snprintf(g_ctx.availability_topic, sizeof(g_ctx.availability_topic),
             "%s/%s/availability", cfg->topic_prefix, cfg->device_id);

    nb->ctx           = &g_ctx;
    nb->init          = nb_init;
    nb->publish_state = nb_publish_state;
    nb->tick          = nb_tick;
    nb->shutdown      = nb_shutdown;
    return 0;
}
