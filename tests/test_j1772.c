#include "test_harness.h"
#include "j1772.h"

/* Classifier band thresholds per j1772.c::classify_strict():
 *    A : >= 10500 mV
 *    B : >=  7500 mV
 *    C : >=  4500 mV
 *    D : >=  1500 mV
 *    E : >= -1500 mV
 *    F : <  -1500 mV */

static j1772_state_t classify_via_step(int32_t cp_mv)
{
    j1772_ctx_t c;
    j1772_init(&c);
    /* with debounce_n=1, first step commits immediately */
    return j1772_step(&c, cp_mv, 1u);
}

void suite_j1772(void)
{
    TEST_CASE("init -> INVALID");
    j1772_ctx_t c;
    j1772_init(&c);
    CHECK_EQ_INT(c.committed, J1772_STATE_INVALID);
    CHECK_EQ_INT(c.candidate, J1772_STATE_INVALID);
    CHECK_EQ_INT(c.streak, 0);

    TEST_CASE("classifier band A high");
    CHECK_EQ_INT(classify_via_step(12000), J1772_STATE_A);

    TEST_CASE("classifier band A low edge");
    CHECK_EQ_INT(classify_via_step(10500), J1772_STATE_A);

    TEST_CASE("classifier band B");
    CHECK_EQ_INT(classify_via_step(9000), J1772_STATE_B);

    TEST_CASE("classifier band B low edge");
    CHECK_EQ_INT(classify_via_step(7500), J1772_STATE_B);

    TEST_CASE("classifier band C");
    CHECK_EQ_INT(classify_via_step(6000), J1772_STATE_C);

    TEST_CASE("classifier band C low edge");
    CHECK_EQ_INT(classify_via_step(4500), J1772_STATE_C);

    TEST_CASE("classifier band D");
    CHECK_EQ_INT(classify_via_step(3000), J1772_STATE_D);

    TEST_CASE("classifier band D low edge");
    CHECK_EQ_INT(classify_via_step(1500), J1772_STATE_D);

    TEST_CASE("classifier band E (negative-ish)");
    CHECK_EQ_INT(classify_via_step(-1000), J1772_STATE_E);

    TEST_CASE("classifier band E low edge");
    CHECK_EQ_INT(classify_via_step(-1500), J1772_STATE_E);

    TEST_CASE("classifier band F");
    CHECK_EQ_INT(classify_via_step(-12000), J1772_STATE_F);

    TEST_CASE("debounce: single read does not commit when n=3");
    j1772_init(&c);
    CHECK_EQ_INT(j1772_step(&c, 12000, 3u), J1772_STATE_INVALID);
    CHECK_EQ_INT(j1772_step(&c, 12000, 3u), J1772_STATE_INVALID);

    TEST_CASE("debounce: 3 consecutive reads commit at n=3");
    CHECK_EQ_INT(j1772_step(&c, 12000, 3u), J1772_STATE_A);

    TEST_CASE("debounce: candidate change resets streak");
    j1772_init(&c);
    j1772_step(&c, 12000, 3u);   /* candidate=A streak=1 */
    j1772_step(&c, 12000, 3u);   /* candidate=A streak=2 */
    /* Now bounce to C — should reset streak, NOT commit */
    j1772_state_t s = j1772_step(&c, 6000, 3u);
    CHECK_EQ_INT(s, J1772_STATE_INVALID);

    TEST_CASE("debounce: returns prior committed value while next is debouncing");
    j1772_init(&c);
    j1772_step(&c, 12000, 1u);   /* commits A immediately */
    /* Now jump to B; with n=3 it should NOT yet show B */
    s = j1772_step(&c, 9000, 3u);
    CHECK_EQ_INT(s, J1772_STATE_A);
    s = j1772_step(&c, 9000, 3u);
    CHECK_EQ_INT(s, J1772_STATE_A);
    s = j1772_step(&c, 9000, 3u);
    CHECK_EQ_INT(s, J1772_STATE_B);

    TEST_CASE("streak saturates at 0xFF");
    j1772_init(&c);
    for (int i = 0; i < 300; ++i) j1772_step(&c, 12000, 1u);
    CHECK_EQ_INT(c.streak, 0xFF);
}
