#include <string.h>
#include "test_harness.h"
#include "mqtt_adapter.h"
#include "northbound.h"
#include "fake_mqtt_client.h"
#include "shmem.h"
#include "shmem_offsets.h"
#include "charger_state.h"

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

static int saw_sub(const char *topic)
{
    for (int i = 0; i < fake_sub_count; i++)
        if (strcmp(fake_sub_topics[i], topic) == 0)
            return 1;
    return 0;
}

/* --- Phase 1: write_enable = 0 (preserves v0.2 read-only contract). ---- */
static void test_read_only(void)
{
    fake_mqtt_client_reset();

    struct mqtt_adapter_config cfg = {
        .broker_host = "localhost", .broker_port = 1883,
        .broker_user = NULL, .broker_pass = NULL,
        .topic_prefix = "delta-bridge", .device_id = "abc",
        .keepalive_s = 60,
        .write_enable = 0,
        .shm = NULL,
    };
    struct northbound nb;
    CHECK_EQ(mqtt_adapter_create(&nb, &cfg), 0);
    CHECK_EQ(nb.init(&nb), 0);

    /* No subscribe in read-only mode. */
    CHECK_EQ(fake_sub_count, 0);

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

    /* Read-only set still present */
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

    /* WRITE entities must NOT appear when write_enable=0 */
    CHECK(!saw_topic("homeassistant/number/delta_abc_set_current/config"));
    CHECK(!saw_topic("homeassistant/switch/delta_abc_authorize/config"));
    CHECK(!saw_topic("homeassistant/button/delta_abc_clear_faults/config"));

    /* Removed v0.2 entities */
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
        const char *p = payload_for("homeassistant/binary_sensor/delta_abc_stm32_link_ok/config");
        CHECK(p != NULL);
        CHECK(strstr(p, "\"device_class\":\"connectivity\"") != NULL);
        CHECK(strstr(p, "\"unit_of_measurement\"") == NULL);
    }
    {
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
    /* authorize_state is published in both modes — full publish emits all. */
    CHECK(find_pub("delta-bridge/abc/authorize_state", "ON"));

    /* delta publish: only the dirty field is re-sent. */
    fake_pub_count = 0;
    cs.current_a = 16.50f;
    CHECK_EQ(nb.publish_state(&nb, &cs, CS_DIRTY_CURRENT, 0), 0);
    CHECK(find_pub("delta-bridge/abc/current", "16.50"));
    CHECK_EQ(fake_pub_count, 1);

    fake_pub_count = 0;
    cs.stm32_link_ok = false;
    CHECK_EQ(nb.publish_state(&nb, &cs, CS_DIRTY_STM32_LINK, 0), 0);
    CHECK(find_pub("delta-bridge/abc/stm32_link_ok", "OFF"));
    CHECK_EQ(fake_pub_count, 1);

    fake_pub_count = 0;
    cs.fault_bits = (1u << 0) | (1u << 6);
    CHECK_EQ(nb.publish_state(&nb, &cs, CS_DIRTY_FAULTS, 0), 0);
    CHECK(find_pub("delta-bridge/abc/active_faults", "OVP,UVP"));

    fake_pub_count = 0;
    cs.fault_bits = (1u << 31);
    CHECK_EQ(nb.publish_state(&nb, &cs, CS_DIRTY_FAULTS, 0), 0);
    CHECK(find_pub("delta-bridge/abc/active_faults", "RFID_module_fail"));

    /* authorize_state only republishes when the boolean (>=1) flips. */
    fake_pub_count = 0;
    cs.user_state = 0;
    CHECK_EQ(nb.publish_state(&nb, &cs, CS_DIRTY_AUTHORIZE, 0), 0);
    CHECK(find_pub("delta-bridge/abc/authorize_state", "OFF"));
    CHECK_EQ(fake_pub_count, 1);

    CHECK_EQ(nb.tick(&nb), 0);
    nb.shutdown(&nb);
}

/* --- Phase 2: write_enable = 1 (writable entities + subscribe + dispatch).
 *
 * Host-test pattern: load the fixture shmem buffer (always writable), wire it
 * into the adapter via cfg.shm, then call fake_mqtt_client_inject_publish()
 * which synchronously invokes the adapter's command callback. After each
 * inject, read the bytes back through shmem_u8/u32_le to assert the write
 * landed (or didn't, for invalid payloads).
 */
static void test_write_enabled(void)
{
    fake_mqtt_client_reset();

    struct shmem sm;
    CHECK_EQ(shmem_load_file(&sm, "test/fixtures/shmem_snapshot.bin"), 0);

    /* Stable known starting values regardless of fixture content. */
    CHECK_EQ(shmem_write_u8(&sm, OFF_RATED_AMPS, 30), 0);
    CHECK_EQ(shmem_write_u8(&sm, OFF_USER_STATE, 0), 0);
    CHECK_EQ(shmem_write_u32_le(&sm, OFF_ALARM_BITMAP, 0xDEADBEEFu), 0);

    struct mqtt_adapter_config cfg = {
        .broker_host = "localhost", .broker_port = 1883,
        .topic_prefix = "delta-bridge", .device_id = "abc",
        .keepalive_s = 60,
        .write_enable = 1,
        .shm = &sm,
    };
    struct northbound nb;
    CHECK_EQ(mqtt_adapter_create(&nb, &cfg), 0);
    CHECK_EQ(nb.init(&nb), 0);

    /* Subscribed to the wildcard command topic. */
    CHECK_EQ(fake_sub_count, 1);
    CHECK(saw_sub("delta-bridge/abc/set/+"));
    CHECK(fake_last_client != NULL);

    /* Full publish triggers discovery for the new write entities. */
    struct charger_state cs;
    charger_state_init(&cs);
    cs.rated_amps = 30;
    cs.user_state = 0;
    CHECK_EQ(nb.publish_state(&nb, &cs, 0, 1 /*full*/), 0);

    /* New discovery payloads exist; the old sensor.rated_amps does NOT. */
    CHECK(saw_topic("homeassistant/number/delta_abc_set_current/config"));
    CHECK(saw_topic("homeassistant/switch/delta_abc_authorize/config"));
    CHECK(saw_topic("homeassistant/button/delta_abc_clear_faults/config"));
    CHECK(!saw_topic("homeassistant/sensor/delta_abc_rated_amps/config"));

    /* Remaining read-only entities are still there. */
    CHECK(saw_topic("homeassistant/sensor/delta_abc_voltage/config"));
    CHECK(saw_topic("homeassistant/sensor/delta_abc_user_state/config"));

    /* number.set_current payload essentials */
    {
        const char *p = payload_for("homeassistant/number/delta_abc_set_current/config");
        CHECK(p != NULL);
        CHECK(strstr(p, "\"name\":\"Set Current\"")               != NULL);
        CHECK(strstr(p, "\"command_topic\":\"delta-bridge/abc/set/rated_amps\"") != NULL);
        CHECK(strstr(p, "\"state_topic\":\"delta-bridge/abc/rated_amps\"")       != NULL);
        CHECK(strstr(p, "\"unique_id\":\"delta_abc_set_current\"") != NULL);
        CHECK(strstr(p, "\"min\":6")                              != NULL);
        CHECK(strstr(p, "\"max\":30")                             != NULL);
        CHECK(strstr(p, "\"step\":1")                             != NULL);
        CHECK(strstr(p, "\"mode\":\"slider\"")                    != NULL);
        CHECK(strstr(p, "\"unit_of_measurement\":\"A\"")          != NULL);
        CHECK(strstr(p, "\"device_class\":\"current\"")           != NULL);
        CHECK(strstr(p, "\"optimistic\":false")                   != NULL);
    }
    /* switch.authorize payload essentials */
    {
        const char *p = payload_for("homeassistant/switch/delta_abc_authorize/config");
        CHECK(p != NULL);
        CHECK(strstr(p, "\"name\":\"Authorize\"")                       != NULL);
        CHECK(strstr(p, "\"command_topic\":\"delta-bridge/abc/set/authorize\"") != NULL);
        CHECK(strstr(p, "\"state_topic\":\"delta-bridge/abc/authorize_state\"") != NULL);
        CHECK(strstr(p, "\"unique_id\":\"delta_abc_authorize\"") != NULL);
        CHECK(strstr(p, "\"payload_on\":\"ON\"")  != NULL);
        CHECK(strstr(p, "\"payload_off\":\"OFF\"") != NULL);
        CHECK(strstr(p, "\"state_on\":\"ON\"")    != NULL);
        CHECK(strstr(p, "\"state_off\":\"OFF\"")  != NULL);
        CHECK(strstr(p, "\"optimistic\":false")   != NULL);
    }
    /* button.clear_faults payload essentials */
    {
        const char *p = payload_for("homeassistant/button/delta_abc_clear_faults/config");
        CHECK(p != NULL);
        CHECK(strstr(p, "\"name\":\"Clear Faults\"") != NULL);
        CHECK(strstr(p, "\"command_topic\":\"delta-bridge/abc/set/clear_faults\"") != NULL);
        CHECK(strstr(p, "\"unique_id\":\"delta_abc_clear_faults\"") != NULL);
        CHECK(strstr(p, "\"state_topic\"") == NULL);
    }

    /* --- Command dispatch --- */

    /* rated_amps in band -> shmem write to OFF_RATED_AMPS. */
    CHECK_EQ(fake_mqtt_client_inject_publish(fake_last_client,
                                             "delta-bridge/abc/set/rated_amps",
                                             "20"), 0);
    CHECK_EQ(shmem_u8(&sm, OFF_RATED_AMPS), 20);

    /* rated_amps below min -> ignored, value unchanged. */
    CHECK_EQ(fake_mqtt_client_inject_publish(fake_last_client,
                                             "delta-bridge/abc/set/rated_amps",
                                             "5"), 0);
    CHECK_EQ(shmem_u8(&sm, OFF_RATED_AMPS), 20);

    /* rated_amps above max -> ignored. */
    CHECK_EQ(fake_mqtt_client_inject_publish(fake_last_client,
                                             "delta-bridge/abc/set/rated_amps",
                                             "45"), 0);
    CHECK_EQ(shmem_u8(&sm, OFF_RATED_AMPS), 20);

    /* rated_amps non-numeric -> ignored. */
    CHECK_EQ(fake_mqtt_client_inject_publish(fake_last_client,
                                             "delta-bridge/abc/set/rated_amps",
                                             "abc"), 0);
    CHECK_EQ(shmem_u8(&sm, OFF_RATED_AMPS), 20);

    /* rated_amps with trailing whitespace is accepted. */
    CHECK_EQ(fake_mqtt_client_inject_publish(fake_last_client,
                                             "delta-bridge/abc/set/rated_amps",
                                             "16\n"), 0);
    CHECK_EQ(shmem_u8(&sm, OFF_RATED_AMPS), 16);

    /* authorize ON -> writes 1 to OFF_USER_STATE. */
    CHECK_EQ(shmem_u8(&sm, OFF_USER_STATE), 0);
    CHECK_EQ(fake_mqtt_client_inject_publish(fake_last_client,
                                             "delta-bridge/abc/set/authorize",
                                             "ON"), 0);
    CHECK_EQ(shmem_u8(&sm, OFF_USER_STATE), 1);

    /* authorize OFF -> writes 0. */
    CHECK_EQ(fake_mqtt_client_inject_publish(fake_last_client,
                                             "delta-bridge/abc/set/authorize",
                                             "OFF"), 0);
    CHECK_EQ(shmem_u8(&sm, OFF_USER_STATE), 0);

    /* authorize garbage -> ignored. Pre-seed a sentinel value to confirm
     * the byte is genuinely untouched. */
    CHECK_EQ(shmem_write_u8(&sm, OFF_USER_STATE, 0x42), 0);
    CHECK_EQ(fake_mqtt_client_inject_publish(fake_last_client,
                                             "delta-bridge/abc/set/authorize",
                                             "garbage"), 0);
    CHECK_EQ(shmem_u8(&sm, OFF_USER_STATE), 0x42);

    /* clear_faults: any payload -> alarm bitmap zeroed. */
    CHECK_EQ(shmem_u32_le(&sm, OFF_ALARM_BITMAP), 0xDEADBEEFu);
    CHECK_EQ(fake_mqtt_client_inject_publish(fake_last_client,
                                             "delta-bridge/abc/set/clear_faults",
                                             "PRESS"), 0);
    CHECK_EQ(shmem_u32_le(&sm, OFF_ALARM_BITMAP), 0u);

    /* clear_faults again with a different payload — still works. */
    CHECK_EQ(shmem_write_u32_le(&sm, OFF_ALARM_BITMAP, 0xAAAAAAAAu), 0);
    CHECK_EQ(fake_mqtt_client_inject_publish(fake_last_client,
                                             "delta-bridge/abc/set/clear_faults",
                                             ""), 0);
    CHECK_EQ(shmem_u32_le(&sm, OFF_ALARM_BITMAP), 0u);

    /* Unknown suffix -> no writes, no crash. */
    CHECK_EQ(shmem_write_u8(&sm, OFF_RATED_AMPS, 22), 0);
    CHECK_EQ(fake_mqtt_client_inject_publish(fake_last_client,
                                             "delta-bridge/abc/set/UNKNOWN",
                                             "99"), 0);
    CHECK_EQ(shmem_u8(&sm, OFF_RATED_AMPS), 22);

    nb.shutdown(&nb);
    shmem_release(&sm);
}

/* --- Phase 3: rfid_enable = 1 (RFID discovery + on-scan publishes). ----
 *
 * Decoupled from write_enable: a user can flip on the RFID reader without
 * also flipping on the control surface. */
static void test_rfid_enabled(void)
{
    fake_mqtt_client_reset();

    struct mqtt_adapter_config cfg = {
        .broker_host = "localhost", .broker_port = 1883,
        .topic_prefix = "delta-bridge", .device_id = "abc",
        .keepalive_s = 60,
        .write_enable = 0,
        .shm = NULL,
        .rfid_enable = 1,
    };
    struct northbound nb;
    CHECK_EQ(mqtt_adapter_create(&nb, &cfg), 0);
    CHECK_EQ(nb.init(&nb), 0);

    /* Discovery republished on a full publish. */
    struct charger_state cs;
    charger_state_init(&cs);
    CHECK_EQ(nb.publish_state(&nb, &cs, 0, 1 /*full*/), 0);

    CHECK(saw_topic("homeassistant/sensor/delta_abc_last_uid/config"));
    CHECK(saw_topic("homeassistant/event/delta_abc_scan/config"));

    /* sensor.last_uid discovery payload essentials. */
    {
        const char *p = payload_for("homeassistant/sensor/delta_abc_last_uid/config");
        CHECK(p != NULL);
        CHECK(strstr(p, "\"name\":\"Last RFID Scan\"") != NULL);
        CHECK(strstr(p, "\"state_topic\":\"delta-bridge/abc/rfid/last_uid\"") != NULL);
        CHECK(strstr(p, "\"unique_id\":\"delta_abc_last_uid\"") != NULL);
    }
    /* event.scan discovery payload essentials. */
    {
        const char *p = payload_for("homeassistant/event/delta_abc_scan/config");
        CHECK(p != NULL);
        CHECK(strstr(p, "\"state_topic\":\"delta-bridge/abc/rfid/scan_event\"") != NULL);
        CHECK(strstr(p, "\"event_types\":[\"card_scanned\"]") != NULL);
        CHECK(strstr(p, "\"unique_id\":\"delta_abc_scan_event\"") != NULL);
    }

    /* on_rfid_scan publishes retained last_uid + non-retained event JSON. */
    fake_pub_count = 0;
    mqtt_adapter_on_rfid_scan("04A1B2C3");
    CHECK_EQ(fake_pub_count, 2);
    CHECK(find_pub("delta-bridge/abc/rfid/last_uid", "04A1B2C3"));
    {
        const char *p = payload_for("delta-bridge/abc/rfid/scan_event");
        CHECK(p != NULL);
        CHECK(strstr(p, "\"event_type\":\"card_scanned\"") != NULL);
        CHECK(strstr(p, "\"uid\":\"04A1B2C3\"") != NULL);
    }
    /* Retain flags: last_uid retained (1), scan_event non-retained (0). */
    int saw_uid_retained = 0, saw_event_nonretained = 0;
    for (int i = 0; i < fake_pub_count; i++) {
        if (!strcmp(fake_pubs[i].topic, "delta-bridge/abc/rfid/last_uid"))
            saw_uid_retained = fake_pubs[i].retain == 1;
        if (!strcmp(fake_pubs[i].topic, "delta-bridge/abc/rfid/scan_event"))
            saw_event_nonretained = fake_pubs[i].retain == 0;
    }
    CHECK(saw_uid_retained);
    CHECK(saw_event_nonretained);

    /* mqtt_adapter_get_last_scan returns the most recent. */
    char uid[32];
    long age = -1;
    CHECK_EQ(mqtt_adapter_get_last_scan(uid, sizeof(uid), &age), 1);
    CHECK_STR(uid, "04A1B2C3");
    CHECK(age >= 0);

    nb.shutdown(&nb);
}

/* And when rfid_enable=0, the discovery topics MUST NOT appear. */
static void test_rfid_disabled(void)
{
    fake_mqtt_client_reset();

    struct mqtt_adapter_config cfg = {
        .broker_host = "localhost", .broker_port = 1883,
        .topic_prefix = "delta-bridge", .device_id = "abc",
        .keepalive_s = 60,
        .rfid_enable = 0,
    };
    struct northbound nb;
    CHECK_EQ(mqtt_adapter_create(&nb, &cfg), 0);
    CHECK_EQ(nb.init(&nb), 0);

    struct charger_state cs;
    charger_state_init(&cs);
    CHECK_EQ(nb.publish_state(&nb, &cs, 0, 1 /*full*/), 0);

    CHECK(!saw_topic("homeassistant/sensor/delta_abc_last_uid/config"));
    CHECK(!saw_topic("homeassistant/event/delta_abc_scan/config"));

    nb.shutdown(&nb);
}

int main(void)
{
    test_read_only();
    test_write_enabled();
    test_rfid_enabled();
    test_rfid_disabled();
    TEST_MAIN_END();
}
