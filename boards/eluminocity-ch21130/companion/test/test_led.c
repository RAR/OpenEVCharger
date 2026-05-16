/* test_led — host tests for the led personality.
 * Covers led_decide() — the pure state-mapping function. */
#include "test_harness.h"
#include "led.h"

static void test_normal_mapping(void)
{
    enum led_action g, r, g2;

    /* All zero → all off. */
    led_decide(0, 0, 0, 0, 0, &g, &r, &g2);
    CHECK_EQ(g,  LED_OFF);
    CHECK_EQ(r,  LED_OFF);
    CHECK_EQ(g2, LED_OFF);

    /* USER_STATE=1 (auth/ready) → green solid. */
    led_decide(1, 0, 0, 0, 0, &g, &r, &g2);
    CHECK_EQ(g,  LED_SOLID);
    CHECK_EQ(r,  LED_OFF);

    /* USER_STATE=2 (charging) → green flash. */
    led_decide(2, 0, 0, 0, 0, &g, &r, &g2);
    CHECK_EQ(g,  LED_FLASH);

    /* RED_LED=1 (alarm) → red solid. */
    led_decide(0, 1, 0, 0, 0, &g, &r, &g2);
    CHECK_EQ(r,  LED_SOLID);

    /* RED_LED=2 (attention) → red flash. */
    led_decide(0, 2, 0, 0, 0, &g, &r, &g2);
    CHECK_EQ(r,  LED_FLASH);

    /* USER + RED combined. */
    led_decide(2, 1, 0, 0, 0, &g, &r, &g2);
    CHECK_EQ(g,  LED_FLASH);
    CHECK_EQ(r,  LED_SOLID);
}

static void test_override_firmware_update(void)
{
    enum led_action g, r, g2;

    /* shmem[0xa71]=1 → override regardless of USER_STATE/RED. */
    int ovr = led_decide(2, 2, 0, 1, 0, &g, &r, &g2);
    CHECK_EQ(ovr, 1);
    CHECK_EQ(g,  LED_SOLID);
    CHECK_EQ(g2, LED_SOLID);
    CHECK_EQ(r,  LED_OFF);

    /* shmem[0xa72] override too. */
    ovr = led_decide(0, 0, 0, 0, 0x42, &g, &r, &g2);
    CHECK_EQ(ovr, 1);
    CHECK_EQ(g,  LED_SOLID);
    CHECK_EQ(g2, LED_SOLID);
}

static void test_override_fault(void)
{
    enum led_action g, r, g2;

    /* PRI_STATE=5 → fault: red flash, green off. */
    int ovr = led_decide(2, 0, 5, 0, 0, &g, &r, &g2);
    CHECK_EQ(ovr, 1);
    CHECK_EQ(g,  LED_OFF);
    CHECK_EQ(r,  LED_FLASH);
    CHECK_EQ(g2, LED_OFF);

    /* PRI_STATE != 5 and no override → back to normal map. */
    ovr = led_decide(1, 0, 4, 0, 0, &g, &r, &g2);
    CHECK_EQ(ovr, 0);
    CHECK_EQ(g,  LED_SOLID);
}

static void test_override_precedence(void)
{
    enum led_action g, r, g2;

    /* Firmware-update override wins over PRI_STATE=5 fault. */
    int ovr = led_decide(0, 0, 5, 1, 0, &g, &r, &g2);
    CHECK_EQ(ovr, 1);
    CHECK_EQ(g,  LED_SOLID);    /* firmware-update pattern */
    CHECK_EQ(g2, LED_SOLID);
}

int main(void)
{
    test_normal_mapping();
    test_override_firmware_update();
    test_override_fault();
    test_override_precedence();
    TEST_MAIN_END();
}
