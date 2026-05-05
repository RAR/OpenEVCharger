#include "test_harness.h"
#include "system_time.h"

void suite_system_time(void);
void suite_system_time(void)
{
    /* The static state inside system_time.c persists across test cases.
     * Each case explicitly sets/clears via the public API. */

    TEST_CASE("default after explicit clear: not set, now returns 0");
    system_time_set(0u, 0u);
    CHECK_EQ_INT(system_time_is_set(), 0);
    CHECK_EQ_U32(system_time_now(123456u), 0u);

    TEST_CASE("set then read at the same tick returns the base");
    system_time_set(1700000000u, 100u);
    CHECK_EQ_INT(system_time_is_set(), 1);
    CHECK_EQ_U32(system_time_now(100u), 1700000000u);

    TEST_CASE("now advances 1 sec per 1000 ms of tick delta");
    system_time_set(1700000000u, 100u);
    CHECK_EQ_U32(system_time_now(1100u),  1700000001u);
    CHECK_EQ_U32(system_time_now(60100u), 1700000060u);
    CHECK_EQ_U32(system_time_now(3600100u), 1700003600u);

    TEST_CASE("sub-second tick delta floors to the same second");
    system_time_set(1700000000u, 100u);
    CHECK_EQ_U32(system_time_now(101u),    1700000000u);
    CHECK_EQ_U32(system_time_now(999u),    1700000000u);
    CHECK_EQ_U32(system_time_now(1099u),   1700000000u);

    TEST_CASE("tick wraparound: unsigned subtraction is correct across 2^32");
    /* base_tick = 0xFFFFFFF0 (close to wrap), now_tick = 0x000003E8 */
    system_time_set(1700000000u, 0xFFFFFFF0u);
    /* delta = 0x000003E8 - 0xFFFFFFF0 = 0x000003F8 = 1016 ms → +1 sec */
    CHECK_EQ_U32(system_time_now(0x000003E8u), 1700000001u);

    TEST_CASE("re-setting overwrites the base reference");
    system_time_set(1700000000u, 1000u);
    system_time_set(1800000000u, 5000u);
    CHECK_EQ_U32(system_time_now(5000u), 1800000000u);
    CHECK_EQ_U32(system_time_now(7000u), 1800000002u);

    TEST_CASE("explicit clear restores not-set state");
    system_time_set(1700000000u, 1000u);
    system_time_set(0u, 9999u);
    CHECK_EQ_INT(system_time_is_set(), 0);
    CHECK_EQ_U32(system_time_now(20000u), 0u);
}
