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

/* Phase-2 thresholds (LUT-derived from stock fw V1.0.066):
 *   TRIP  = 396  (≈85 °C)
 *   CLEAR = 525  (≈75 °C)
 *
 * Test canonical raw values were tuned for these:
 *   "hot" sustained reading      = 350    (~88 °C, ≤ TRIP)
 *   "cool but still active" raw  = 450    (between TRIP+1 and CLEAR-1)
 *   "cleared" raw                = 525    (= CLEAR)
 *   "well cool"                  = 2000   (~26 °C, plenty of room) */
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
    /* raw=350 < TRIP=396. Wall-plug populated, gun not. */
    for (unsigned i = 0; i < OT_PERSIST_TICKS - 1U; ++i) {
        a = over_temp_step(&ctx, 350, 0, 1, 0);
        CHECK_EQ_INT(a.trip, 0);
        CHECK_EQ_INT(a.clear, 0);
    }
    a = over_temp_step(&ctx, 350, 0, 1, 0);
    CHECK_EQ_INT(a.trip, 1);
    CHECK_EQ_INT(a.clear, 0);
    CHECK_EQ_INT(ctx.fault_active, 1);

    TEST_CASE("trip is single-edge: subsequent hot ticks don't re-trip");
    /* Continue from above ctx (fault_active=1). */
    a = over_temp_step(&ctx, 350, 0, 1, 0);
    CHECK_EQ_INT(a.trip, 0);
    CHECK_EQ_INT(a.clear, 0);
    CHECK_EQ_INT(ctx.fault_active, 1);

    TEST_CASE("4 hot ticks then 1 cold tick → streak resets, no trip");
    ctx = (over_temp_ctx_t){ 0, 0 };
    for (unsigned i = 0; i < 4; ++i) {
        a = over_temp_step(&ctx, 350, 0, 1, 0);
        CHECK_EQ_INT(a.trip, 0);
    }
    /* Cold reading (well above CLEAR). */
    a = over_temp_step(&ctx, 2000, 0, 1, 0);
    CHECK_EQ_INT(a.trip, 0);
    CHECK_EQ_INT(ctx.trip_streak, 0);
    /* Re-trip requires another full PERSIST_TICKS run. */
    for (unsigned i = 0; i < OT_PERSIST_TICKS - 1U; ++i) {
        a = over_temp_step(&ctx, 350, 0, 1, 0);
        CHECK_EQ_INT(a.trip, 0);
    }
    a = over_temp_step(&ctx, 350, 0, 1, 0);
    CHECK_EQ_INT(a.trip, 1);

    TEST_CASE("trip → 4 ticks at TRIP+1 (still cooler than CLEAR) → still active");
    /* Set up active fault. */
    ctx = (over_temp_ctx_t){ 0, 0 };
    drive_n(&ctx, OT_PERSIST_TICKS, 350, 0, 1, 0);
    CHECK_EQ_INT(ctx.fault_active, 1);
    /* Reading raw=450: above TRIP=396 so the hot branch doesn't run, but
     * still below CLEAR=525. Should NOT clear. */
    for (unsigned i = 0; i < 4; ++i) {
        a = over_temp_step(&ctx, 450, 0, 1, 0);
        CHECK_EQ_INT(a.trip, 0);
        CHECK_EQ_INT(a.clear, 0);
    }
    CHECK_EQ_INT(ctx.fault_active, 1);

    TEST_CASE("trip → reading at CLEAR → clear edge fires once");
    ctx = (over_temp_ctx_t){ 0, 0 };
    drive_n(&ctx, OT_PERSIST_TICKS, 350, 0, 1, 0);
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
    drive_n(&ctx, OT_PERSIST_TICKS, 350, 0, 1, 0);
    a = over_temp_step(&ctx, 3000, 0, 1, 0);
    CHECK_EQ_INT(a.clear, 1);
    CHECK_EQ_INT(ctx.fault_active, 0);

    TEST_CASE("gun unpopulated (mask=0): noise spike to 234 must NOT trip");
    /* If the gun cable isn't fitted, PA2 may float. With mask=0 it's ignored. */
    ctx = (over_temp_ctx_t){ 0, 0 };
    a = drive_n(&ctx, OT_PERSIST_TICKS + 5U,
                /* wall cool */ 2000,
                /* gun noise spike, well below TRIP */ 234,
                /* presence */ 1, 0);
    CHECK_EQ_INT(a.trip, 0);
    CHECK_EQ_INT(ctx.fault_active, 0);

    TEST_CASE("gun populated + raw=234 → still ignored by populated guard");
    /* raw=234 is below OT_GUARD_LO=300, so even with mask=1 the populated
     * guard rejects it. Wall=2000 is cool, so no trip. Fail-safe against
     * a floating-near-GND input even if a future build sets the mask
     * by accident. */
    ctx = (over_temp_ctx_t){ 0, 0 };
    a = drive_n(&ctx, OT_PERSIST_TICKS + 5U, 2000, 234, 1, 1);
    CHECK_EQ_INT(a.trip, 0);

    TEST_CASE("gun populated + raw=350 (clearly hot, above guard) → trips");
    /* Sanity-check the mask is actually doing the work. raw=350 is
     * above LO guard, below TRIP, so a real over-temp on the gun NTC. */
    ctx = (over_temp_ctx_t){ 0, 0 };
    a = drive_n(&ctx, OT_PERSIST_TICKS, 2000, 350, 1, 1);
    CHECK_EQ_INT(a.trip, 1);

    TEST_CASE("wall alone, sustained low → trips after 5 ticks");
    ctx = (over_temp_ctx_t){ 0, 0 };
    a = drive_n(&ctx, OT_PERSIST_TICKS, 380, 4000 /* unpopulated rail */,
                1, 0);
    CHECK_EQ_INT(a.trip, 1);

    TEST_CASE("hottest-of-two: wall=2000, gun=350 (both populated) → trip on gun");
    ctx = (over_temp_ctx_t){ 0, 0 };
    a = drive_n(&ctx, OT_PERSIST_TICKS, 2000, 350, 1, 1);
    CHECK_EQ_INT(a.trip, 1);

    TEST_CASE("hottest-of-two: wall=350, gun=2000 (both populated) → trip on wall");
    ctx = (over_temp_ctx_t){ 0, 0 };
    a = drive_n(&ctx, OT_PERSIST_TICKS, 350, 2000, 1, 1);
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
    drive_n(&ctx, OT_PERSIST_TICKS, 350, 0, 1, 0);
    CHECK_EQ_INT(ctx.fault_active, 1);
    /* Now drop both populated masks. Detector reports no action — the
     * fault stays latched on the caller side. */
    a = over_temp_step(&ctx, 350, 234, 0, 0);
    CHECK_EQ_INT(a.trip, 0);
    CHECK_EQ_INT(a.clear, 0);
    CHECK_EQ_INT(ctx.fault_active, 1);
    CHECK_EQ_INT(ctx.trip_streak, 0);

    TEST_CASE("Phase-2 thresholds match LUT-derived 85 °C / 75 °C");
    /* Sanity check: the constants compile-in match the documented
     * 85 °C trip / 75 °C clear from stock fw NTC LUT. */
    CHECK_EQ_INT((int)OT_TRIP_RAW,  396);
    CHECK_EQ_INT((int)OT_CLEAR_RAW, 525);

    TEST_CASE("regression: old β=3380 hot raw 400 must NOT trip post-Phase-2");
    /* Pre-Phase-2 the canonical hot reading was 400 (just below the
     * β=3380 trip of 532). With LUT thresholds it sits just above
     * TRIP=396, so it should NOT trip. Catches accidental revert. */
    ctx = (over_temp_ctx_t){ 0, 0 };
    a = drive_n(&ctx, OT_PERSIST_TICKS + 5U, 400, 0, 1, 0);
    CHECK_EQ_INT(a.trip, 0);
}
