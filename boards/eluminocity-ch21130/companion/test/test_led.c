/* test_led — host tests for the led personality.
 * Covers led_decide() (the pure state-mapping function) and the
 * polarity helper led_sysfs_byte_for(). */
#include "test_harness.h"
#include "led.h"

static void test_normal_mapping(void)
{
    enum led_action g, r, g2;

    /* All zero → all off. */
    led_decide(0, 0, 0, 0, &g, &r, &g2);
    CHECK_EQ(g,  LED_OFF);
    CHECK_EQ(r,  LED_OFF);
    CHECK_EQ(g2, LED_OFF);

    /* USER_STATE=1 → green (middle) solid. */
    led_decide(1, 0, 0, 0, &g, &r, &g2);
    CHECK_EQ(g,  LED_SOLID);
    CHECK_EQ(r,  LED_OFF);
    CHECK_EQ(g2, LED_OFF);

    /* USER_STATE=2 → green (middle) flash. */
    led_decide(2, 0, 0, 0, &g, &r, &g2);
    CHECK_EQ(g,  LED_FLASH);

    /* RED_LED=1 → red (bottom) solid. */
    led_decide(0, 1, 0, 0, &g, &r, &g2);
    CHECK_EQ(r,  LED_SOLID);

    /* RED_LED=2 → red flash. */
    led_decide(0, 2, 0, 0, &g, &r, &g2);
    CHECK_EQ(r,  LED_FLASH);

    /* GREEN2_STATE=1 → green2 (top) solid. */
    led_decide(0, 0, 1, 0, &g, &r, &g2);
    CHECK_EQ(g,  LED_OFF);
    CHECK_EQ(r,  LED_OFF);
    CHECK_EQ(g2, LED_SOLID);

    /* GREEN2_STATE=2 → green2 flash. */
    led_decide(0, 0, 2, 0, &g, &r, &g2);
    CHECK_EQ(g2, LED_FLASH);

    /* Combined: USER=2, RED=1, GREEN2=1 (current bench-equivalent for
     * an idle ready unit on a different cycle phase). */
    led_decide(2, 1, 1, 0, &g, &r, &g2);
    CHECK_EQ(g,  LED_FLASH);
    CHECK_EQ(r,  LED_SOLID);
    CHECK_EQ(g2, LED_SOLID);
}

static void test_bench_idle_pattern(void)
{
    /* Bench-observed 2026-05-16: shmem says USER_STATE=0, RED_LED=1,
     * GREEN2_STATE=1, no fw-update flag. Stock writes:
     *   gpio55 (BOTTOM red)   = 1  (Red_IOCtrl(1))      → red SOLID
     *   gpio56 (MIDDLE green) = 0  (Green_IOCtrl(0))    → green OFF
     *   gpio57 (TOP green2)   = 1  (Green2_IOCtrl(1))   → green2 SOLID
     * Our led_decide should produce the same actions. */
    enum led_action g, r, g2;
    led_decide(0 /*USER*/, 1 /*RED*/, 1 /*GREEN2*/, 0 /*fw*/,
               &g, &r, &g2);
    CHECK_EQ(g,  LED_OFF);
    CHECK_EQ(r,  LED_SOLID);
    CHECK_EQ(g2, LED_SOLID);
}

static void test_fw_update_override(void)
{
    enum led_action g, r, g2;

    /* fw_update != 0 → override regardless of USER/RED/GREEN2. */
    int ovr = led_decide(2, 2, 2, 1, &g, &r, &g2);
    CHECK_EQ(ovr, 1);
    CHECK_EQ(g,  LED_SOLID);   /* MIDDLE green solid (Green_IOCtrl(1)) */
    CHECK_EQ(g2, LED_SOLID);   /* TOP green2 solid (Green2_IOCtrl(1)) */
    CHECK_EQ(r,  LED_OFF);     /* BOTTOM red off (Red_IOCtrl(0)) */

    /* Any nonzero counts as "in fw update". */
    ovr = led_decide(0, 0, 0, 0xff, &g, &r, &g2);
    CHECK_EQ(ovr, 1);
    CHECK_EQ(g,  LED_SOLID);

    /* Normal path returns 0. */
    ovr = led_decide(0, 1, 1, 0, &g, &r, &g2);
    CHECK_EQ(ovr, 0);
}

static void test_unknown_state_codes(void)
{
    enum led_action g, r, g2;
    /* Stock's Green2_State=3 calls Green2_Wifi() — we don't replicate
     * that pattern, but anything >= 3 should land on solid so a
     * mistaken value doesn't blank the LED. */
    led_decide(3, 4, 5, 0, &g, &r, &g2);
    CHECK_EQ(g,  LED_SOLID);
    CHECK_EQ(r,  LED_SOLID);
    CHECK_EQ(g2, LED_SOLID);
}

static void test_active_high_polarity(void)
{
    /* LEDs are wired active-high (bench-verified 2026-05-16 by
     * controlled per-GPIO toggle): logical "on" sends sysfs byte 1,
     * logical "off" sends sysfs byte 0. Pin this so a future cleanup
     * doesn't silently flip it. */
    CHECK_EQ(led_sysfs_byte_for(1), 1);
    CHECK_EQ(led_sysfs_byte_for(0), 0);
    /* Any nonzero is "on". */
    CHECK_EQ(led_sysfs_byte_for(42), 1);
    CHECK_EQ(led_sysfs_byte_for(-1), 1);
}

int main(void)
{
    test_normal_mapping();
    test_bench_idle_pattern();
    test_fw_update_override();
    test_unknown_state_codes();
    test_active_high_polarity();
    TEST_MAIN_END();
}
