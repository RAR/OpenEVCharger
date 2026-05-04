#include "test_harness.h"
#include "over_temp.h"

/* Helper to drive N ticks with the same NTC reading and presence mask.
 * Returns the action of the LAST tick (caller asserts on it). */
static over_temp_action_t drive_n(over_temp_ctx_t *ctx, unsigned n,
                                  uint16_t r1, uint16_t r2,
                                  int p1, int p2)
{
    over_temp_action_t a = { 0, 0 };
    for (unsigned i = 0; i < n; ++i) {
        a = over_temp_step(ctx, r1, r2, p1, p2);
    }
    return a;
}

void suite_over_temp(void);
void suite_over_temp(void)
{
    over_temp_ctx_t ctx;

    TEST_CASE("cold-start, both unpopulated → no action regardless of raw");
    ctx = (over_temp_ctx_t){ 0, 0 };
    /* Even with raw=0 (well below trip), no populated channel → no trip. */
    over_temp_action_t a = drive_n(&ctx, 50, 0, 0, 0, 0);
    CHECK_EQ_INT(a.trip, 0);
    CHECK_EQ_INT(a.clear, 0);
    CHECK_EQ_INT(ctx.trip_streak, 0);
    CHECK_EQ_INT(ctx.fault_active, 0);

    TEST_CASE("single hot reading sustained 5 ticks → trip on the 5th");
    ctx = (over_temp_ctx_t){ 0, 0 };
    /* raw=400 < TRIP=532. NTC1 populated, NTC2 not. */
    for (unsigned i = 0; i < OT_PERSIST_TICKS - 1U; ++i) {
        a = over_temp_step(&ctx, 400, 0, 1, 0);
        CHECK_EQ_INT(a.trip, 0);
        CHECK_EQ_INT(a.clear, 0);
    }
    a = over_temp_step(&ctx, 400, 0, 1, 0);
    CHECK_EQ_INT(a.trip, 1);
    CHECK_EQ_INT(a.clear, 0);
    CHECK_EQ_INT(ctx.fault_active, 1);

    TEST_CASE("trip is single-edge: subsequent hot ticks don't re-trip");
    /* Continue from above ctx (fault_active=1). */
    a = over_temp_step(&ctx, 400, 0, 1, 0);
    CHECK_EQ_INT(a.trip, 0);
    CHECK_EQ_INT(a.clear, 0);
    CHECK_EQ_INT(ctx.fault_active, 1);

    TEST_CASE("4 hot ticks then 1 cold tick → streak resets, no trip");
    ctx = (over_temp_ctx_t){ 0, 0 };
    for (unsigned i = 0; i < 4; ++i) {
        a = over_temp_step(&ctx, 400, 0, 1, 0);
        CHECK_EQ_INT(a.trip, 0);
    }
    /* Cold reading (well above CLEAR). */
    a = over_temp_step(&ctx, 2000, 0, 1, 0);
    CHECK_EQ_INT(a.trip, 0);
    CHECK_EQ_INT(ctx.trip_streak, 0);
    /* Re-trip requires another full PERSIST_TICKS run. */
    for (unsigned i = 0; i < OT_PERSIST_TICKS - 1U; ++i) {
        a = over_temp_step(&ctx, 400, 0, 1, 0);
        CHECK_EQ_INT(a.trip, 0);
    }
    a = over_temp_step(&ctx, 400, 0, 1, 0);
    CHECK_EQ_INT(a.trip, 1);

    TEST_CASE("trip → 4 ticks at TRIP+1 (still cooler than CLEAR) → still active");
    /* Set up active fault. */
    ctx = (over_temp_ctx_t){ 0, 0 };
    drive_n(&ctx, OT_PERSIST_TICKS, 400, 0, 1, 0);
    CHECK_EQ_INT(ctx.fault_active, 1);
    /* Reading raw=600: above TRIP=532 so the hot branch doesn't run, but
     * still below CLEAR=672. Should NOT clear. */
    for (unsigned i = 0; i < 4; ++i) {
        a = over_temp_step(&ctx, 600, 0, 1, 0);
        CHECK_EQ_INT(a.trip, 0);
        CHECK_EQ_INT(a.clear, 0);
    }
    CHECK_EQ_INT(ctx.fault_active, 1);

    TEST_CASE("trip → reading at CLEAR → clear edge fires once");
    ctx = (over_temp_ctx_t){ 0, 0 };
    drive_n(&ctx, OT_PERSIST_TICKS, 400, 0, 1, 0);
    CHECK_EQ_INT(ctx.fault_active, 1);
    a = over_temp_step(&ctx, OT_CLEAR_RAW, 0, 1, 0);
    CHECK_EQ_INT(a.clear, 1);
    CHECK_EQ_INT(a.trip, 0);
    CHECK_EQ_INT(ctx.fault_active, 0);
    /* Subsequent at-clear readings: no edge. */
    a = over_temp_step(&ctx, OT_CLEAR_RAW, 0, 1, 0);
    CHECK_EQ_INT(a.clear, 0);
    CHECK_EQ_INT(a.trip, 0);

    TEST_CASE("trip → reading well above CLEAR → clear fires");
    ctx = (over_temp_ctx_t){ 0, 0 };
    drive_n(&ctx, OT_PERSIST_TICKS, 400, 0, 1, 0);
    a = over_temp_step(&ctx, 3000, 0, 1, 0);
    CHECK_EQ_INT(a.clear, 1);
    CHECK_EQ_INT(ctx.fault_active, 0);

    TEST_CASE("NTC2 unpopulated (mask=0): noise spike to 234 must NOT trip");
    /* PB0 floats and noise-spikes to ~234. With mask=0 it's ignored. */
    ctx = (over_temp_ctx_t){ 0, 0 };
    a = drive_n(&ctx, OT_PERSIST_TICKS + 5U,
                /* NTC1 cool */ 2000,
                /* NTC2 noise spike, well below TRIP */ 234,
                /* presence */ 1, 0);
    CHECK_EQ_INT(a.trip, 0);
    CHECK_EQ_INT(ctx.fault_active, 0);

    TEST_CASE("NTC2 populated + raw=234 → still ignored by populated guard");
    /* Cross-check: raw=234 is below OT_GUARD_LO=300, so even with the
     * mask=1 the populated guard rejects it. NTC1=2000 is cool, so no
     * trip. This is the bench's fail-safe against a floating-near-GND
     * NTC2 input even if a future build sets the mask by accident. */
    ctx = (over_temp_ctx_t){ 0, 0 };
    a = drive_n(&ctx, OT_PERSIST_TICKS + 5U, 2000, 234, 1, 1);
    CHECK_EQ_INT(a.trip, 0);

    TEST_CASE("NTC2 populated + raw=400 (clearly hot, above guard) → trips");
    /* Sanity-check the mask is actually doing the work. raw=400 is
     * above LO guard, below TRIP, so a real over-temp on NTC2. */
    ctx = (over_temp_ctx_t){ 0, 0 };
    a = drive_n(&ctx, OT_PERSIST_TICKS, 2000, 400, 1, 1);
    CHECK_EQ_INT(a.trip, 1);

    TEST_CASE("NTC1 alone, sustained low → trips after 5 ticks");
    ctx = (over_temp_ctx_t){ 0, 0 };
    a = drive_n(&ctx, OT_PERSIST_TICKS, 500, 4000 /* unpopulated rail */,
                1, 0);
    CHECK_EQ_INT(a.trip, 1);

    TEST_CASE("hottest-of-two: NTC1=2000, NTC2=400 (both populated) → trip on NTC2");
    ctx = (over_temp_ctx_t){ 0, 0 };
    a = drive_n(&ctx, OT_PERSIST_TICKS, 2000, 400, 1, 1);
    CHECK_EQ_INT(a.trip, 1);

    TEST_CASE("populated guard: raw=300 (lower edge, exclusive) ignored");
    ctx = (over_temp_ctx_t){ 0, 0 };
    /* OT_GUARD_LO is exclusive: raw must be > 300 to be considered. */
    a = drive_n(&ctx, OT_PERSIST_TICKS + 5U, OT_GUARD_LO, 0, 1, 0);
    CHECK_EQ_INT(a.trip, 0);

    TEST_CASE("populated guard: raw=3990 (upper edge, exclusive) ignored");
    ctx = (over_temp_ctx_t){ 0, 0 };
    a = drive_n(&ctx, OT_PERSIST_TICKS + 5U, OT_GUARD_HI, 0, 1, 0);
    CHECK_EQ_INT(a.trip, 0);

    TEST_CASE("populated guard: raw=301 (just above LO) considered → trips");
    ctx = (over_temp_ctx_t){ 0, 0 };
    a = drive_n(&ctx, OT_PERSIST_TICKS, 301, 0, 1, 0);
    CHECK_EQ_INT(a.trip, 1);

    TEST_CASE("populated guard: raw=3989 (just below HI) considered (cool, no trip)");
    ctx = (over_temp_ctx_t){ 0, 0 };
    a = drive_n(&ctx, OT_PERSIST_TICKS + 5U, 3989, 0, 1, 0);
    CHECK_EQ_INT(a.trip, 0);
    CHECK_EQ_INT(a.clear, 0);

    TEST_CASE("trip exactly at OT_TRIP_RAW boundary (<=, inclusive)");
    ctx = (over_temp_ctx_t){ 0, 0 };
    a = drive_n(&ctx, OT_PERSIST_TICKS, OT_TRIP_RAW, 0, 1, 0);
    CHECK_EQ_INT(a.trip, 1);

    TEST_CASE("no trip at OT_TRIP_RAW + 1 (just cooler than trip)");
    ctx = (over_temp_ctx_t){ 0, 0 };
    a = drive_n(&ctx, OT_PERSIST_TICKS + 5U, OT_TRIP_RAW + 1U, 0, 1, 0);
    CHECK_EQ_INT(a.trip, 0);

    TEST_CASE("populated lost mid-active: streak resets, fault stays latched");
    /* Trip first. */
    ctx = (over_temp_ctx_t){ 0, 0 };
    drive_n(&ctx, OT_PERSIST_TICKS, 400, 0, 1, 0);
    CHECK_EQ_INT(ctx.fault_active, 1);
    /* Now drop both populated masks (e.g. someone unplugged the gun
     * and the bench NTC1 was somehow unpopulated). Detector reports no
     * action — the fault stays latched on the caller side. */
    a = over_temp_step(&ctx, 400, 234, 0, 0);
    CHECK_EQ_INT(a.trip, 0);
    CHECK_EQ_INT(a.clear, 0);
    CHECK_EQ_INT(ctx.fault_active, 1);
    CHECK_EQ_INT(ctx.trip_streak, 0);
}
