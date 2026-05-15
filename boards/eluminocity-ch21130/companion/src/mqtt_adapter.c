#include "mqtt_adapter.h"
#include "mqtt_client.h"
#include "charger_state.h"

#include <stdio.h>
#include <string.h>

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

/* Publish one HA discovery config for a sensor field. */
static void publish_discovery(struct adapter_ctx *a, const char *component,
                              const char *field, const char *name,
                              const char *unit, const char *device_class)
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
    if (n > 0 && n < (int)sizeof(payload))
        snprintf(payload + n, sizeof(payload) - n, "}");

    mqtt_client_publish(&a->client, topic, payload, 1);
}

static void publish_all_discovery(struct adapter_ctx *a)
{
    publish_discovery(a, "sensor", "voltage", "Voltage", "V", "voltage");
    publish_discovery(a, "sensor", "current", "Current", "A", "current");
    publish_discovery(a, "sensor", "evse_state", "EVSE State", NULL, NULL);
    publish_discovery(a, "sensor", "heartbeat", "Heartbeat", NULL, NULL);
    publish_discovery(a, "binary_sensor", "stm32_link", "STM32 Link", NULL,
                      "connectivity");
    publish_discovery(a, "sensor", "faults", "Active Faults", NULL, NULL);
}

/* Build the comma-joined list of active fault names (or "none"). */
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

    if (full)
        publish_all_discovery(a);

    char topic[160], val[256];

    if (full || (dirty & CS_DIRTY_VOLTAGE)) {
        state_topic(a, topic, sizeof(topic), "voltage");
        snprintf(val, sizeof(val), "%d", cs->voltage_v);
        mqtt_client_publish(&a->client, topic, val, 1);
    }
    if (full || (dirty & CS_DIRTY_CURRENT)) {
        state_topic(a, topic, sizeof(topic), "current");
        snprintf(val, sizeof(val), "%d", cs->current_a);
        mqtt_client_publish(&a->client, topic, val, 1);
    }
    if (full || (dirty & CS_DIRTY_EVSE_STATE)) {
        state_topic(a, topic, sizeof(topic), "evse_state");
        mqtt_client_publish(&a->client, topic, evse_state_str(cs->evse_state), 1);
    }
    if (full || (dirty & CS_DIRTY_HEARTBEAT)) {
        state_topic(a, topic, sizeof(topic), "heartbeat");
        snprintf(val, sizeof(val), "%d", cs->heartbeat);
        mqtt_client_publish(&a->client, topic, val, 1);
    }
    if (full || (dirty & CS_DIRTY_LINK)) {
        state_topic(a, topic, sizeof(topic), "stm32_link");
        mqtt_client_publish(&a->client, topic, cs->stm32_link ? "ON" : "OFF", 1);
    }
    if (full || (dirty & CS_DIRTY_FAULTS)) {
        state_topic(a, topic, sizeof(topic), "faults");
        format_faults(cs->fault_bits, val, sizeof(val));
        mqtt_client_publish(&a->client, topic, val, 1);
    }
    return 0;
}

static int nb_tick(struct northbound *nb)
{
    struct adapter_ctx *a = nb->ctx;
    /* now_ms is supplied by main.c in the real loop; the fake ignores it. */
    return mqtt_client_tick(&a->client, 0);
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
