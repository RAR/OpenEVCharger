#include <string.h>
#include "test_harness.h"
#include "mqtt_adapter.h"
#include "northbound.h"
#include "fake_mqtt_client.h"

static int find_pub(const char *topic, const char *payload)
{
    for (int i = 0; i < fake_pub_count; i++)
        if (strcmp(fake_pubs[i].topic, topic) == 0 &&
            strcmp(fake_pubs[i].payload, payload) == 0)
            return 1;
    return 0;
}

static int saw_topic(const char *topic)
{
    for (int i = 0; i < fake_pub_count; i++)
        if (strcmp(fake_pubs[i].topic, topic) == 0)
            return 1;
    return 0;
}

static const char *payload_for(const char *topic)
{
    for (int i = 0; i < fake_pub_count; i++)
        if (strcmp(fake_pubs[i].topic, topic) == 0)
            return fake_pubs[i].payload;
    return NULL;
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

    /* init() must have published availability=online (retained) first */
    CHECK_EQ(fake_pub_count, 1);
    CHECK_STR(fake_pubs[0].topic,   "delta-bridge/abc/availability");
    CHECK_STR(fake_pubs[0].payload, "online");
    CHECK_EQ(fake_pubs[0].retain, 1);

    struct charger_state cs;
    charger_state_init(&cs);
    cs.voltage_v       = 230.5f;
    cs.current_a       = 15.75f;
    cs.power_w         = 3625.0f;
    cs.pilot_duty_pct  = 50;
    cs.rated_amps      = 30;
    cs.pilot_state     = PILOT_C;
    cs.pri_state       = 3;
    cs.user_state      = 2;
    cs.red_led         = 0;
    cs.stm32_link_ok   = true;
    cs.fault_bits      = 0;

    /* full publish emits discovery + every state field */
    CHECK_EQ(nb.publish_state(&nb, &cs, 0, 1 /*full*/), 0);

    /* Discovery topics — one per entity. */
    CHECK(saw_topic("homeassistant/sensor/delta_abc_voltage/config"));
    CHECK(saw_topic("homeassistant/sensor/delta_abc_current/config"));
    CHECK(saw_topic("homeassistant/sensor/delta_abc_power/config"));
    CHECK(saw_topic("homeassistant/sensor/delta_abc_pilot_duty/config"));
    CHECK(saw_topic("homeassistant/sensor/delta_abc_rated_amps/config"));
    CHECK(saw_topic("homeassistant/sensor/delta_abc_pilot_state/config"));
    CHECK(saw_topic("homeassistant/sensor/delta_abc_pri_state/config"));
    CHECK(saw_topic("homeassistant/sensor/delta_abc_user_state/config"));
    CHECK(saw_topic("homeassistant/sensor/delta_abc_red_led/config"));
    CHECK(saw_topic("homeassistant/binary_sensor/delta_abc_stm32_link_ok/config"));
    CHECK(saw_topic("homeassistant/sensor/delta_abc_active_faults/config"));

    /* Removed entities must NOT be published */
    CHECK(!saw_topic("homeassistant/sensor/delta_abc_evse_state/config"));
    CHECK(!saw_topic("homeassistant/sensor/delta_abc_heartbeat/config"));
    CHECK(!saw_topic("delta-bridge/abc/evse_state"));
    CHECK(!saw_topic("delta-bridge/abc/heartbeat"));

    /* Discovery payload content — voltage entity has device_class+unit+state_class. */
    {
        const char *p = payload_for("homeassistant/sensor/delta_abc_voltage/config");
        CHECK(p != NULL);
        CHECK(strstr(p, "\"unit_of_measurement\":\"V\"") != NULL);
        CHECK(strstr(p, "\"device_class\":\"voltage\"") != NULL);
        CHECK(strstr(p, "\"state_class\":\"measurement\"") != NULL);
    }
    {
        const char *p = payload_for("homeassistant/sensor/delta_abc_current/config");
        CHECK(p != NULL);
        CHECK(strstr(p, "\"unit_of_measurement\":\"A\"") != NULL);
        CHECK(strstr(p, "\"device_class\":\"current\"") != NULL);
        CHECK(strstr(p, "\"state_class\":\"measurement\"") != NULL);
    }
    {
        const char *p = payload_for("homeassistant/sensor/delta_abc_power/config");
        CHECK(p != NULL);
        CHECK(strstr(p, "\"unit_of_measurement\":\"W\"") != NULL);
        CHECK(strstr(p, "\"device_class\":\"power\"") != NULL);
        CHECK(strstr(p, "\"state_class\":\"measurement\"") != NULL);
    }
    {
        /* binary_sensor has connectivity device_class, no unit, no state_class */
        const char *p = payload_for("homeassistant/binary_sensor/delta_abc_stm32_link_ok/config");
        CHECK(p != NULL);
        CHECK(strstr(p, "\"device_class\":\"connectivity\"") != NULL);
        CHECK(strstr(p, "\"unit_of_measurement\"") == NULL);
    }
    {
        /* pilot_state is a plain enum: no unit, no device_class */
        const char *p = payload_for("homeassistant/sensor/delta_abc_pilot_state/config");
        CHECK(p != NULL);
        CHECK(strstr(p, "\"unit_of_measurement\"") == NULL);
        CHECK(strstr(p, "\"device_class\"") == NULL);
    }

    /* State payloads */
    CHECK(find_pub("delta-bridge/abc/voltage",       "230.5"));
    CHECK(find_pub("delta-bridge/abc/current",       "15.75"));
    CHECK(find_pub("delta-bridge/abc/power",         "3625"));
    CHECK(find_pub("delta-bridge/abc/pilot_duty",    "50"));
    CHECK(find_pub("delta-bridge/abc/rated_amps",    "30"));
    CHECK(find_pub("delta-bridge/abc/pilot_state",   "C"));
    CHECK(find_pub("delta-bridge/abc/pri_state",     "3"));
    CHECK(find_pub("delta-bridge/abc/user_state",    "2"));
    CHECK(find_pub("delta-bridge/abc/red_led",       "0"));
    CHECK(find_pub("delta-bridge/abc/stm32_link_ok", "ON"));
    CHECK(find_pub("delta-bridge/abc/active_faults", "none"));

    /* delta publish: only the dirty field is re-sent. */
    fake_pub_count = 0;
    cs.current_a = 16.50f;
    CHECK_EQ(nb.publish_state(&nb, &cs, CS_DIRTY_CURRENT, 0), 0);
    CHECK(find_pub("delta-bridge/abc/current", "16.50"));
    CHECK_EQ(fake_pub_count, 1);

    /* stm32_link_ok=false → "OFF" */
    fake_pub_count = 0;
    cs.stm32_link_ok = false;
    CHECK_EQ(nb.publish_state(&nb, &cs, CS_DIRTY_STM32_LINK, 0), 0);
    CHECK(find_pub("delta-bridge/abc/stm32_link_ok", "OFF"));
    CHECK_EQ(fake_pub_count, 1);

    /* active_faults: bit 0 + bit 6 set => "OVP,UVP" (ascending bit order). */
    fake_pub_count = 0;
    cs.fault_bits = (1u << 0) | (1u << 6);
    CHECK_EQ(nb.publish_state(&nb, &cs, CS_DIRTY_FAULTS, 0), 0);
    CHECK(find_pub("delta-bridge/abc/active_faults", "OVP,UVP"));

    /* bit 31 alone => "RFID_module_fail" */
    fake_pub_count = 0;
    cs.fault_bits = (1u << 31);
    CHECK_EQ(nb.publish_state(&nb, &cs, CS_DIRTY_FAULTS, 0), 0);
    CHECK(find_pub("delta-bridge/abc/active_faults", "RFID_module_fail"));

    CHECK_EQ(nb.tick(&nb), 0);
    nb.shutdown(&nb);
    TEST_MAIN_END();
}
