#include <string.h>
#include "test_harness.h"
#include "mqtt_adapter.h"
#include "northbound.h"

/* exposed by fake_mqtt_client.c */
struct fake_pub { char topic[160]; char payload[256]; int retain; };
extern struct fake_pub fake_pubs[];
extern int fake_pub_count;

static int find_pub(const char *topic, const char *payload)
{
    for (int i = 0; i < fake_pub_count; i++)
        if (strcmp(fake_pubs[i].topic, topic) == 0 &&
            strcmp(fake_pubs[i].payload, payload) == 0)
            return 1;
    return 0;
}

int main(void)
{
    struct mqtt_adapter_config cfg = {
        .broker_host = "localhost", .broker_port = 1883,
        .broker_user = NULL, .broker_pass = NULL,
        .topic_prefix = "delta-bridge", .device_id = "abc",
        .keepalive_s = 60,
    };
    struct northbound nb;
    CHECK_EQ(mqtt_adapter_create(&nb, &cfg), 0);
    CHECK_EQ(nb.init(&nb), 0);

    struct charger_state cs;
    charger_state_init(&cs);
    cs.voltage_v = 121; cs.current_a = 15; cs.stm32_link = 1;
    cs.evse_state = EVSE_STATE_CHARGING;

    /* full publish emits discovery + every state field */
    CHECK_EQ(nb.publish_state(&nb, &cs, 0, 1 /*full*/), 0);
    CHECK(find_pub("homeassistant/sensor/delta_abc_voltage/config",
                   "") == 0);              /* discovery payload is non-empty */
    /* discovery topic exists with *some* payload */
    int saw_discovery = 0, saw_voltage = 0, saw_state = 0;
    for (int i = 0; i < fake_pub_count; i++) {
        if (strcmp(fake_pubs[i].topic,
                   "homeassistant/sensor/delta_abc_voltage/config") == 0)
            saw_discovery = 1;
        if (strcmp(fake_pubs[i].topic, "delta-bridge/abc/voltage") == 0 &&
            strcmp(fake_pubs[i].payload, "121") == 0)
            saw_voltage = 1;
        if (strcmp(fake_pubs[i].topic, "delta-bridge/abc/evse_state") == 0 &&
            strcmp(fake_pubs[i].payload, "charging") == 0)
            saw_state = 1;
    }
    CHECK(saw_discovery);
    CHECK(saw_voltage);
    CHECK(saw_state);

    /* delta publish: only the dirty field is re-sent */
    fake_pub_count = 0;
    cs.current_a = 16;
    CHECK_EQ(nb.publish_state(&nb, &cs, CS_DIRTY_CURRENT, 0), 0);
    CHECK(find_pub("delta-bridge/abc/current", "16"));
    CHECK_EQ(fake_pub_count, 1);            /* nothing else re-published */

    /* availability published online on init */
    nb.shutdown(&nb);
    TEST_MAIN_END();
}
