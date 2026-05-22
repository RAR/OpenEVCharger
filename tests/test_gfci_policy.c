#include "test_harness.h"
#include "gfci_policy.h"

/* Drive N ticks with a fixed PE2 level + policy; return the action of
 * the LAST tick (caller asserts on it). */
static gfci_policy_action_t drive_n(gfci_policy_ctx_t *ctx, unsigned n,
                                    int pe2_low, uint8_t policy)
{
    gfci_policy_action_t a = GFCI_ACT_NONE;
    for (unsigned i = 0; i < n; ++i)
        a = gfci_policy_step(ctx, pe2_low, policy);
    return a;
}

void suite_gfci_policy(void);
void suite_gfci_policy(void)
{
    gfci_policy_ctx_t ctx;
    gfci_policy_action_t a;

    TEST_CASE("policy constants mirror proto/commands.h GFCI_POLICY_*");
    CHECK_EQ_INT((int)GFCI_POL_FAULT, 0);
    CHECK_EQ_INT((int)GFCI_POL_WARN, 1);

    TEST_CASE("idle: PE2 high → no action, streak stays 0");
    ctx = (gfci_policy_ctx_t){ 0, 0, 0 };
    a = drive_n(&ctx, 50, 0, GFCI_POL_FAULT);
    CHECK_EQ_INT(a, GFCI_ACT_NONE);
    CHECK_EQ_INT(ctx.trip_streak, 0);
    CHECK_EQ_INT(ctx.fault_active, 0);

    TEST_CASE("FAULT: PE2 low for < PERSIST_TICKS → no action yet");
    ctx = (gfci_policy_ctx_t){ 0, 0, 0 };
    for (unsigned i = 0; i < GFCI_PERSIST_TICKS - 1U; ++i) {
        a = gfci_policy_step(&ctx, 1, GFCI_POL_FAULT);
        CHECK_EQ_INT(a, GFCI_ACT_NONE);
    }
    CHECK_EQ_INT(ctx.fault_active, 0);

    TEST_CASE("FAULT: PE2 low for PERSIST_TICKS → RAISE_FAULT on last tick");
    ctx = (gfci_policy_ctx_t){ 0, 0, 0 };
    for (unsigned i = 0; i < GFCI_PERSIST_TICKS - 1U; ++i)
        gfci_policy_step(&ctx, 1, GFCI_POL_FAULT);
    a = gfci_policy_step(&ctx, 1, GFCI_POL_FAULT);
    CHECK_EQ_INT(a, GFCI_ACT_RAISE_FAULT);
    CHECK_EQ_INT(ctx.fault_active, 1);

    TEST_CASE("FAULT: single-edge — further low ticks do NOT re-raise");
    /* continues from above ctx (fault_active = 1) */
    a = drive_n(&ctx, 20, 1, GFCI_POL_FAULT);
    CHECK_EQ_INT(a, GFCI_ACT_NONE);
    CHECK_EQ_INT(ctx.fault_active, 1);

    TEST_CASE("FAULT: streak resets if PE2 returns high before debounce");
    ctx = (gfci_policy_ctx_t){ 0, 0, 0 };
    gfci_policy_step(&ctx, 1, GFCI_POL_FAULT);
    gfci_policy_step(&ctx, 1, GFCI_POL_FAULT);
    a = gfci_policy_step(&ctx, 0, GFCI_POL_FAULT);   /* high → reset */
    CHECK_EQ_INT(a, GFCI_ACT_NONE);
    CHECK_EQ_INT(ctx.trip_streak, 0);
    /* one low tick after the reset is not enough on its own */
    a = gfci_policy_step(&ctx, 1, GFCI_POL_FAULT);
    CHECK_EQ_INT(a, GFCI_ACT_NONE);

    TEST_CASE("WARN: debounced PE2 low → WARN_EMIT, fault NOT raised");
    ctx = (gfci_policy_ctx_t){ 0, 0, 0 };
    a = drive_n(&ctx, GFCI_PERSIST_TICKS, 1, GFCI_POL_WARN);
    CHECK_EQ_INT(a, GFCI_ACT_WARN_EMIT);
    CHECK_EQ_INT(ctx.fault_active, 0);   /* WARN never latches a fault */
    CHECK_EQ_INT(ctx.warned, 1);

    TEST_CASE("WARN: emits exactly once per PE2-low episode");
    /* continues from above (warned = 1); many more low ticks → silent */
    a = drive_n(&ctx, 50, 1, GFCI_POL_WARN);
    CHECK_EQ_INT(a, GFCI_ACT_NONE);
    CHECK_EQ_INT(ctx.fault_active, 0);

    TEST_CASE("WARN: edge re-arms after PE2 returns high → emits again");
    /* still continuing: PE2 high releases the latch and resets streak */
    a = gfci_policy_step(&ctx, 0, GFCI_POL_WARN);
    CHECK_EQ_INT(a, GFCI_ACT_NONE);
    CHECK_EQ_INT(ctx.warned, 0);
    CHECK_EQ_INT(ctx.trip_streak, 0);
    /* a fresh PE2-low episode → a second WARN_EMIT */
    a = drive_n(&ctx, GFCI_PERSIST_TICKS, 1, GFCI_POL_WARN);
    CHECK_EQ_INT(a, GFCI_ACT_WARN_EMIT);

    TEST_CASE("WARN: low for < PERSIST_TICKS → no emit");
    ctx = (gfci_policy_ctx_t){ 0, 0, 0 };
    for (unsigned i = 0; i < GFCI_PERSIST_TICKS - 1U; ++i) {
        a = gfci_policy_step(&ctx, 1, GFCI_POL_WARN);
        CHECK_EQ_INT(a, GFCI_ACT_NONE);
    }

    TEST_CASE("fail-safe: unknown policy byte (2) treated as FAULT");
    ctx = (gfci_policy_ctx_t){ 0, 0, 0 };
    a = drive_n(&ctx, GFCI_PERSIST_TICKS, 1, 2u);
    CHECK_EQ_INT(a, GFCI_ACT_RAISE_FAULT);
    CHECK_EQ_INT(ctx.fault_active, 1);

    TEST_CASE("fail-safe: stale policy byte (0xFF) treated as FAULT");
    ctx = (gfci_policy_ctx_t){ 0, 0, 0 };
    a = drive_n(&ctx, GFCI_PERSIST_TICKS, 1, 0xFFu);
    CHECK_EQ_INT(a, GFCI_ACT_RAISE_FAULT);

    TEST_CASE("explicit FAULT policy byte (0) raises");
    ctx = (gfci_policy_ctx_t){ 0, 0, 0 };
    a = drive_n(&ctx, GFCI_PERSIST_TICKS, 1, GFCI_POL_FAULT);
    CHECK_EQ_INT(a, GFCI_ACT_RAISE_FAULT);

    TEST_CASE("trip_streak caps at PERSIST_TICKS (no unbounded growth)");
    ctx = (gfci_policy_ctx_t){ 0, 0, 0 };
    drive_n(&ctx, 1000, 1, GFCI_POL_WARN);
    CHECK_EQ_INT(ctx.trip_streak, GFCI_PERSIST_TICKS);

    TEST_CASE("policy flip mid-episode: WARN latch never blocks a FAULT raise");
    /* A WARN episode is in progress (warned = 1, fault_active = 0); the
     * policy then flips to FAULT — e.g. an HA write lands. The next
     * debounced tick must RAISE: the fault posture is never weaker
     * than the live policy, and the WARN latch must not suppress it. */
    ctx = (gfci_policy_ctx_t){ 0, 0, 0 };
    drive_n(&ctx, GFCI_PERSIST_TICKS, 1, GFCI_POL_WARN);
    CHECK_EQ_INT(ctx.warned, 1);
    CHECK_EQ_INT(ctx.fault_active, 0);
    a = gfci_policy_step(&ctx, 1, GFCI_POL_FAULT);
    CHECK_EQ_INT(a, GFCI_ACT_RAISE_FAULT);
    CHECK_EQ_INT(ctx.fault_active, 1);
}
