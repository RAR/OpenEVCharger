#include "test_harness.h"
#include "mocks/w25q_mock.h"
#include "boot_config.h"
#include "../src/hal/w25q.h"

#include <string.h>

void suite_boot_config(void);
void suite_boot_config(void)
{
    /* All cases assume a freshly-erased W25Q; reset between cases.
     * boot_config.c keeps a static cache, so we exercise it via its
     * public API only — load() then accessors / setters then load()
     * again to cross-check that the persisted record drives state. */

    TEST_CASE("blank flash: load writes defaults; advertised_amps == 0");
    w25q_mock_reset(); w25q_init();
    int rc = boot_config_load();
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(boot_config_advertised_amps(), 0);
    /* After defaults write, slot A should be valid (counter=1). */
    CHECK(!w25q_mock_sector_is_blank(BOOT_CONFIG_SLOT_A));
    CHECK(w25q_mock_sector_is_blank(BOOT_CONFIG_SLOT_B));

    TEST_CASE("set + reload round-trips advertised_amps");
    w25q_mock_reset(); w25q_init();
    boot_config_load();
    rc = boot_config_set_advertised_amps(32);
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(boot_config_advertised_amps(), 32);
    /* Reload from flash — bypasses the in-RAM cache by going through
     * the same code path the firmware uses on next boot. */
    rc = boot_config_load();
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(boot_config_advertised_amps(), 32);

    TEST_CASE("set: idempotent for unchanged value (no slot churn)");
    w25q_mock_reset(); w25q_init();
    boot_config_load();
    boot_config_set_advertised_amps(20);
    /* Find which sector is currently active. After defaults+one set,
     * defaults landed in A (counter=1), set landed in B (counter=2). */
    CHECK(!w25q_mock_sector_is_blank(BOOT_CONFIG_SLOT_B));
    CHECK(w25q_mock_sector_is_blank(BOOT_CONFIG_SLOT_A));
    /* Re-setting same value: no flash write → B stays put, A stays blank. */
    rc = boot_config_set_advertised_amps(20);
    CHECK_EQ_INT(rc, 0);
    CHECK(!w25q_mock_sector_is_blank(BOOT_CONFIG_SLOT_B));
    CHECK(w25q_mock_sector_is_blank(BOOT_CONFIG_SLOT_A));

    TEST_CASE("two consecutive sets: latest wins on reload");
    w25q_mock_reset(); w25q_init();
    boot_config_load();
    boot_config_set_advertised_amps(16);
    boot_config_set_advertised_amps(48);
    rc = boot_config_load();
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(boot_config_advertised_amps(), 48);

    TEST_CASE("corrupt CRC on the live slot → load falls back to defaults");
    w25q_mock_reset(); w25q_init();
    boot_config_load();
    boot_config_set_advertised_amps(40);
    /* After this, slot B is the live one (counter=2), A is erased. */
    CHECK(!w25q_mock_sector_is_blank(BOOT_CONFIG_SLOT_B));
    /* Corrupt B's CRC. */
    w25q_mock_poke(BOOT_CONFIG_SLOT_B + 32U - 4U, 0x00, 1);
    /* Reload: both invalid (A blank, B CRC-bad) → defaults written. */
    rc = boot_config_load();
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(boot_config_advertised_amps(), 0);

    TEST_CASE("both-slot-blank load creates defaults exactly once");
    w25q_mock_reset(); w25q_init();
    rc = boot_config_load();
    CHECK_EQ_INT(rc, 0);
    /* Re-load — should NOT re-write (slot A still valid, no defaults
     * write path taken). Capture the byte at slot A's CRC offset
     * before/after to confirm. */
    uint8_t crc_byte_pre = w25q_mock_peek(BOOT_CONFIG_SLOT_A + 32U - 4U);
    rc = boot_config_load();
    CHECK_EQ_INT(rc, 0);
    uint8_t crc_byte_post = w25q_mock_peek(BOOT_CONFIG_SLOT_A + 32U - 4U);
    CHECK_EQ_INT(crc_byte_pre, crc_byte_post);

    TEST_CASE("set 0xFF (max u8) round-trips");
    w25q_mock_reset(); w25q_init();
    boot_config_load();
    rc = boot_config_set_advertised_amps(0xFFu);
    CHECK_EQ_INT(rc, 0);
    rc = boot_config_load();
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(boot_config_advertised_amps(), 0xFFu);

    TEST_CASE("set 0 round-trips (intentional 'unset' sentinel)");
    w25q_mock_reset(); w25q_init();
    boot_config_load();
    boot_config_set_advertised_amps(32);   /* burn one slot */
    rc = boot_config_set_advertised_amps(0);
    CHECK_EQ_INT(rc, 0);
    rc = boot_config_load();
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(boot_config_advertised_amps(), 0);
}
