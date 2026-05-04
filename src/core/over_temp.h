#ifndef OPENBHZD_CORE_OVER_TEMP_H
#define OPENBHZD_CORE_OVER_TEMP_H

#include <stdint.h>

/* Pure over-temp detector. Caller (safety_task) feeds in the latest
 * NTC1 / NTC2 raw ADC counts plus a per-channel "populated" mask, and
 * the function reports whether the FAULT_OVER_TEMP edge fired this
 * tick. The caller does the bookkeeping (fault_raise / fault_clear /
 * post_fault_event / EVSE transition / printk).
 *
 * NTC wiring: 10 kΩ pulldown to GND with 10 kΩ pullup to 3.3 V. As the
 * thermistor heats, resistance drops, divider voltage drops, ADC count
 * drops. Trip / clear thresholds are pre-computed from a β=3380 model:
 *
 *   85 °C → R_ntc ≈ 1.49 kΩ → raw ≈ 532 (TRIP)
 *   75 °C → R_ntc ≈ 1.96 kΩ → raw ≈ 672 (CLEAR — +10 °C hysteresis)
 *
 * Populated guard:
 *   raw <= 300  → input near GND, no thermistor (bench NTC2 floats here)
 *   raw >= 3990 → input pulled to VDD (open thermistor / shorted upper
 *                 resistor)
 *
 * Persistence: TRIP requires OT_PERSIST_TICKS consecutive ticks at-or-
 * below TRIP. CLEAR is single-tick (no debounce; cooling is monotonic).
 *
 * The implementation lives entirely in this header so the firmware can
 * inline it into safety_task::check_over_temp (avoiding the call /
 * 5-arg-frame cost), while host tests can exercise it through a thin
 * non-inline wrapper provided by core/over_temp.c. */

#define OT_TRIP_RAW         532U
#define OT_CLEAR_RAW        672U
#define OT_GUARD_LO         300U
#define OT_GUARD_HI        3990U
#define OT_PERSIST_TICKS    5U

typedef struct {
    unsigned trip_streak;     /* consecutive ticks at or below trip */
    int      fault_active;    /* 1 if FAULT_OVER_TEMP currently raised */
} over_temp_ctx_t;

typedef struct {
    int trip;     /* 1 if the caller should raise FAULT_OVER_TEMP this tick */
    int clear;    /* 1 if the caller should clear FAULT_OVER_TEMP this tick */
} over_temp_action_t;

static inline int over_temp_populated(uint16_t raw)
{
    return (raw > OT_GUARD_LO) && (raw < OT_GUARD_HI);
}

static inline over_temp_action_t over_temp_step(over_temp_ctx_t *ctx,
                                                uint16_t ntc1_raw,
                                                uint16_t ntc2_raw,
                                                int ntc1_present,
                                                int ntc2_present)
{
    over_temp_action_t out = { 0, 0 };

    int p1 = ntc1_present && over_temp_populated(ntc1_raw);
    int p2 = ntc2_present && over_temp_populated(ntc2_raw);
    if (!p1 && !p2) {
        /* No populated NTC reachable. Don't auto-clear an existing
         * fault into a sensorless state; just reset the streak. */
        ctx->trip_streak = 0;
        return out;
    }

    /* Hottest = smallest raw among the populated channels. */
    uint16_t hottest_raw = 0xFFFFu;
    if (p1 && ntc1_raw < hottest_raw) hottest_raw = ntc1_raw;
    if (p2 && ntc2_raw < hottest_raw) hottest_raw = ntc2_raw;

    if (!ctx->fault_active && hottest_raw <= OT_TRIP_RAW) {
        if (ctx->trip_streak < OT_PERSIST_TICKS) ++ctx->trip_streak;
        if (ctx->trip_streak >= OT_PERSIST_TICKS) {
            out.trip = 1;
            ctx->fault_active = 1;
        }
    } else {
        ctx->trip_streak = 0;
        if (ctx->fault_active && hottest_raw >= OT_CLEAR_RAW) {
            out.clear = 1;
            ctx->fault_active = 0;
        }
    }
    return out;
}

#endif
