#include "test_harness.h"
#include "fault.h"

void suite_fault(void)
{
    fault_state_t fs;

    TEST_CASE("init: no faults active");
    fault_init(&fs);
    CHECK_EQ_U32(fs.active_bits, 0u);
    CHECK_EQ_INT(fs.first_raised, FAULT_NONE);
    CHECK_EQ_INT(fault_any_active(&fs), 0);
    CHECK_EQ_INT(fault_any_latched_active(&fs), 0);

    TEST_CASE("raise: returns 1 on new raise, 0 on re-raise");
    CHECK_EQ_INT(fault_raise(&fs, FAULT_GFCI), 1);
    CHECK_EQ_INT(fault_raise(&fs, FAULT_GFCI), 0);
    CHECK_EQ_INT(fault_is_active(&fs, FAULT_GFCI), 1);

    TEST_CASE("raise: first_raised tracks the first fault only");
    fault_init(&fs);
    fault_raise(&fs, FAULT_RELAY_WELD);
    fault_raise(&fs, FAULT_GFCI);
    CHECK_EQ_INT(fs.first_raised, FAULT_RELAY_WELD);

    TEST_CASE("raise: invalid id returns -1");
    fault_init(&fs);
    CHECK_EQ_INT(fault_raise(&fs, FAULT_NONE), -1);
    CHECK_EQ_INT(fault_raise(&fs, FAULT_COUNT), -1);

    TEST_CASE("clear: returns 1 if cleared, 0 if not active");
    fault_init(&fs);
    fault_raise(&fs, FAULT_RELAY_WELD);
    CHECK_EQ_INT(fault_clear(&fs, FAULT_RELAY_WELD), 1);
    CHECK_EQ_INT(fault_clear(&fs, FAULT_RELAY_WELD), 0);
    CHECK_EQ_INT(fault_is_active(&fs, FAULT_RELAY_WELD), 0);

    TEST_CASE("clear: GFCI cannot be cleared by API (power-cycle only)");
    fault_init(&fs);
    fault_raise(&fs, FAULT_GFCI);
    CHECK_EQ_INT(fault_clear(&fs, FAULT_GFCI), -1);
    CHECK_EQ_INT(fault_is_active(&fs, FAULT_GFCI), 1);

    TEST_CASE("clear: clearing first_raised picks next active as new first");
    fault_init(&fs);
    fault_raise(&fs, FAULT_RELAY_WELD);
    fault_raise(&fs, FAULT_CP_NO_PILOT);
    CHECK_EQ_INT(fs.first_raised, FAULT_RELAY_WELD);
    fault_clear(&fs, FAULT_RELAY_WELD);
    CHECK_EQ_INT(fs.first_raised, FAULT_CP_NO_PILOT);

    TEST_CASE("clear: clearing first_raised resets to NONE if last");
    fault_init(&fs);
    fault_raise(&fs, FAULT_RELAY_WELD);
    fault_clear(&fs, FAULT_RELAY_WELD);
    CHECK_EQ_INT(fs.first_raised, FAULT_NONE);

    TEST_CASE("clear: invalid id returns -1");
    fault_init(&fs);
    CHECK_EQ_INT(fault_clear(&fs, FAULT_NONE), -1);
    CHECK_EQ_INT(fault_clear(&fs, FAULT_COUNT), -1);

    TEST_CASE("is_latched_kind: GFCI..CRASH_LOOP latched, OVER_TEMP..CP_REGRESSION self-clearing");
    CHECK_EQ_INT(fault_is_latched_kind(FAULT_GFCI), 1);
    CHECK_EQ_INT(fault_is_latched_kind(FAULT_RELAY_WELD), 1);
    CHECK_EQ_INT(fault_is_latched_kind(FAULT_CRASH_LOOP_SAFE_FAIL), 1);
    CHECK_EQ_INT(fault_is_latched_kind(FAULT_OVER_TEMP), 0);
    CHECK_EQ_INT(fault_is_latched_kind(FAULT_CP_REGRESSION), 0);
    CHECK_EQ_INT(fault_is_latched_kind(FAULT_NONE), 0);
    CHECK_EQ_INT(fault_is_latched_kind(FAULT_COUNT), 0);

    TEST_CASE("any_latched_active: ignores self-clearing");
    fault_init(&fs);
    fault_raise(&fs, FAULT_OVER_TEMP);
    CHECK_EQ_INT(fault_any_active(&fs), 1);
    CHECK_EQ_INT(fault_any_latched_active(&fs), 0);
    fault_raise(&fs, FAULT_RELAY_WELD);
    CHECK_EQ_INT(fault_any_latched_active(&fs), 1);

    TEST_CASE("clear_all_clearable: clears latched except GFCI, leaves self-clearing");
    fault_init(&fs);
    fault_raise(&fs, FAULT_GFCI);
    fault_raise(&fs, FAULT_RELAY_WELD);
    fault_raise(&fs, FAULT_BOOT_SELF_TEST);
    fault_raise(&fs, FAULT_OVER_TEMP);
    int cleared = fault_clear_all_clearable(&fs);
    CHECK_EQ_INT(cleared, 2);
    CHECK_EQ_INT(fault_is_active(&fs, FAULT_GFCI), 1);
    CHECK_EQ_INT(fault_is_active(&fs, FAULT_OVER_TEMP), 1);
    CHECK_EQ_INT(fault_is_active(&fs, FAULT_RELAY_WELD), 0);
    CHECK_EQ_INT(fault_is_active(&fs, FAULT_BOOT_SELF_TEST), 0);

    TEST_CASE("clear_all_clearable: first_raised picks lowest remaining");
    /* GFCI (1) survives clear_all_clearable; should become first_raised. */
    fault_init(&fs);
    fault_raise(&fs, FAULT_RELAY_WELD);   /* first */
    fault_raise(&fs, FAULT_GFCI);
    fault_clear_all_clearable(&fs);
    CHECK_EQ_INT(fs.first_raised, FAULT_GFCI);

    TEST_CASE("clear_all_clearable: zero remaining -> first_raised = NONE");
    fault_init(&fs);
    fault_raise(&fs, FAULT_RELAY_WELD);
    fault_clear_all_clearable(&fs);
    CHECK_EQ_INT(fs.first_raised, FAULT_NONE);

    TEST_CASE("fault_name: known ids return non-empty stable strings");
    CHECK(strcmp(fault_name(FAULT_NONE), "none") == 0);
    CHECK(strcmp(fault_name(FAULT_GFCI), "GFCI") == 0);
    CHECK(strcmp(fault_name(FAULT_RELAY_WELD), "RELAY_WELD") == 0);
    CHECK(strcmp(fault_name(FAULT_OVER_TEMP), "OVER_TEMP") == 0);
    CHECK(fault_name(FAULT_COUNT) != NULL);
    CHECK(fault_name((fault_id_t)999) != NULL);
}
