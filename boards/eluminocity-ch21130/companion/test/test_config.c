#include <string.h>
#include "test_harness.h"
#include "config.h"

int main(void)
{
    struct config c;

    /* defaults applied when keys are absent */
    config_defaults(&c);
    CHECK_EQ(c.broker_port, 1883);
    CHECK_EQ(c.poll_hz, 1);
    CHECK_STR(c.topic_prefix, "delta-bridge");
    CHECK_EQ(c.write_enable, 0);                  /* v0.3: opt-in default off */
    CHECK_EQ(c.web_enable, 0);                    /* v0.4: opt-in default off */
    CHECK_EQ(c.web_port, 8080);
    CHECK_STR(c.web_user, "");
    CHECK_STR(c.web_pass, "");

    /* parse overrides; comments and blank lines ignored; whitespace trimmed */
    const char *text =
        "# sample config\n"
        "broker_host = 10.0.0.5\n"
        "broker_port=8883\n"
        "  topic_prefix =  evse  \n"
        "\n"
        "device_id = unitA\n"
        "poll_hz = 2\n";
    CHECK_EQ(config_parse(&c, text), 0);
    CHECK_STR(c.broker_host, "10.0.0.5");
    CHECK_EQ(c.broker_port, 8883);
    CHECK_STR(c.topic_prefix, "evse");
    CHECK_STR(c.device_id, "unitA");
    CHECK_EQ(c.poll_hz, 2);

    /* unknown keys are non-fatal (still returns 0) but warn to stderr */
    CHECK_EQ(config_parse(&c, "bogus_key = 1\n"), 0);

    /* write_enable accepts true/false/yes/no/on/off/1/0, case-insensitive */
    config_defaults(&c);
    CHECK_EQ(config_parse(&c, "write_enable = true\n"), 0);
    CHECK_EQ(c.write_enable, 1);

    config_defaults(&c);
    CHECK_EQ(config_parse(&c, "write_enable = false\n"), 0);
    CHECK_EQ(c.write_enable, 0);

    config_defaults(&c);
    CHECK_EQ(config_parse(&c, "write_enable=1\n"), 0);
    CHECK_EQ(c.write_enable, 1);

    config_defaults(&c);
    CHECK_EQ(config_parse(&c, "write_enable=0\n"), 0);
    CHECK_EQ(c.write_enable, 0);

    config_defaults(&c);
    CHECK_EQ(config_parse(&c, "write_enable = YES\n"), 0);
    CHECK_EQ(c.write_enable, 1);

    config_defaults(&c);
    CHECK_EQ(config_parse(&c, "write_enable = No\n"), 0);
    CHECK_EQ(c.write_enable, 0);

    config_defaults(&c);
    CHECK_EQ(config_parse(&c, "write_enable = on\n"), 0);
    CHECK_EQ(c.write_enable, 1);

    /* Unrecognised bool: warns, leaves write_enable off (safe default) */
    config_defaults(&c);
    c.write_enable = 1;
    CHECK_EQ(config_parse(&c, "write_enable = maybe\n"), 0);
    CHECK_EQ(c.write_enable, 0);

    /* v0.4 web_* keys */
    config_defaults(&c);
    CHECK_EQ(config_parse(&c,
        "web_enable = true\n"
        "web_port   = 9090\n"
        "web_user   = admin\n"
        "web_pass   = hunter2\n"), 0);
    CHECK_EQ(c.web_enable, 1);
    CHECK_EQ(c.web_port, 9090);
    CHECK_STR(c.web_user, "admin");
    CHECK_STR(c.web_pass, "hunter2");

    /* web_enable accepts the same bool spellings */
    config_defaults(&c);
    CHECK_EQ(config_parse(&c, "web_enable = on\n"), 0);
    CHECK_EQ(c.web_enable, 1);
    config_defaults(&c);
    CHECK_EQ(config_parse(&c, "web_enable=1\n"), 0);
    CHECK_EQ(c.web_enable, 1);
    config_defaults(&c);
    CHECK_EQ(config_parse(&c, "web_enable = false\n"), 0);
    CHECK_EQ(c.web_enable, 0);
    config_defaults(&c);
    CHECK_EQ(config_parse(&c, "web_enable = no\n"), 0);
    CHECK_EQ(c.web_enable, 0);

    /* Unrecognised bool for web_enable warns + defaults to off */
    config_defaults(&c);
    c.web_enable = 1;
    CHECK_EQ(config_parse(&c, "web_enable = perhaps\n"), 0);
    CHECK_EQ(c.web_enable, 0);

    /* Out-of-range web_port clamps to default */
    config_defaults(&c);
    CHECK_EQ(config_parse(&c, "web_port = 0\n"), 0);
    CHECK_EQ(c.web_port, 8080);
    config_defaults(&c);
    CHECK_EQ(config_parse(&c, "web_port = 99999\n"), 0);
    CHECK_EQ(c.web_port, 8080);

    /* v0.5 — RFID keys */
    config_defaults(&c);
    CHECK_EQ(c.rfid_enable, 0);                   /* opt-in, off by default */
    CHECK_STR(c.rfid_port, "/dev/ttyAMA4");
    CHECK_EQ(c.rfid_kill_stock, 1);
    CHECK_EQ(c.rfid_poll_hz, 5);

    config_defaults(&c);
    CHECK_EQ(config_parse(&c,
        "rfid_enable     = true\n"
        "rfid_port       = /dev/ttyAMA9\n"
        "rfid_kill_stock = false\n"
        "rfid_poll_hz    = 10\n"), 0);
    CHECK_EQ(c.rfid_enable, 1);
    CHECK_STR(c.rfid_port, "/dev/ttyAMA9");
    CHECK_EQ(c.rfid_kill_stock, 0);
    CHECK_EQ(c.rfid_poll_hz, 10);

    /* rfid_enable accepts the standard bool spellings. */
    config_defaults(&c);
    CHECK_EQ(config_parse(&c, "rfid_enable = on\n"), 0);
    CHECK_EQ(c.rfid_enable, 1);
    config_defaults(&c);
    CHECK_EQ(config_parse(&c, "rfid_enable = 0\n"), 0);
    CHECK_EQ(c.rfid_enable, 0);

    /* Garbage bool for rfid_enable: warns + safe default (off). */
    config_defaults(&c);
    c.rfid_enable = 1;
    CHECK_EQ(config_parse(&c, "rfid_enable = nope\n"), 0);
    CHECK_EQ(c.rfid_enable, 0);

    /* Garbage bool for rfid_kill_stock: warns + safe default (on — we
     * default-take the UART once rfid_enable lit up, otherwise stock
     * fights us for it). */
    config_defaults(&c);
    c.rfid_kill_stock = 0;
    CHECK_EQ(config_parse(&c, "rfid_kill_stock = perhaps\n"), 0);
    CHECK_EQ(c.rfid_kill_stock, 1);

    /* Poll Hz clamping. */
    config_defaults(&c);
    CHECK_EQ(config_parse(&c, "rfid_poll_hz = 0\n"), 0);
    CHECK_EQ(c.rfid_poll_hz, 1);
    config_defaults(&c);
    CHECK_EQ(config_parse(&c, "rfid_poll_hz = 9999\n"), 0);
    CHECK_EQ(c.rfid_poll_hz, 50);

    TEST_MAIN_END();
}
