#ifndef OPENEVCHARGER_CORE_OVER_TEMP_H
#define OPENEVCHARGER_CORE_OVER_TEMP_H

#include <stdint.h>

/* Pure over-temp detector. Caller (safety_task) feeds in the latest
 * raw ADC counts for two NTC channels plus per-channel "populated"
 * masks, and the function reports whether the FAULT_OVER_TEMP edge
 * fired this tick. The caller does the bookkeeping (fault_raise /
 * fault_clear / post_fault_event / EVSE transition / printk).
 *
 * Channel mapping after the 2026-05-04 channel-role correction:
 *   ntc1 = PA3 wall-plug NTC (always populated on production units)
 *   ntc2 = PA2 gun-cable NTC (populated on production; bench may not be)
 *   PB0 ("NTC2 ADC raw" in system_state) is NOT a thermistor and is
 *   never fed to this detector — it stays exposed as raw-only diag.
 *
 * NTC wiring: 10 kΩ NTC pulldown to GND with 10 kΩ pullup to 3.3 V.
 * 12-bit ADC. As the thermistor heats, resistance drops, divider
 * voltage drops, raw count drops.
 *
 * Trip / clear thresholds are LUT-derived from the stock fw V1.0.066
 * NTC table extracted at flash 0x08024f28 (commit ffbfc68; mirrored
 * in fc41d/components/openevcharger_tlv/ntc_lut.h):
 *
 *   85 °C → raw 396 (TRIP)
 *   75 °C → raw 525 (CLEAR — +10 °C hysteresis)
 *
 * Phase-2 (2026-05-04) replaced the earlier β=3380-derived thresholds
 * (532/672) with the LUT-direct values; the OEM thermistor's β is
 * actually closer to 3980 and the β-fit was off by ~10 °C at the trip
 * point.
 *
 * Populated guard:
 *   raw <= 300  → input near GND, no thermistor (PB0-style float)
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

#define OT_TRIP_RAW         396U
#define OT_CLEAR_RAW        525U
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
