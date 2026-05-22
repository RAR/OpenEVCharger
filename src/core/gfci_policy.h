#ifndef OPENEVCHARGER_CORE_GFCI_POLICY_H
#define OPENEVCHARGER_CORE_GFCI_POLICY_H

#include <stdint.h>

/* Pure GFCI fault-handling policy decision. safety_task::check_gfci
 * samples PE2 (the GFCI module's active-low fault line) and the
 * persisted policy byte, feeds them to gfci_policy_step(), and carries
 * out the returned action. The caller owns ALL I/O — fault_raise /
 * post_fault_event / evse_transition / printk. The decision state
 * machine (debounce, the WARN edge-latch, the fail-safe rule) lives
 * here so it is host-testable in isolation — see
 * tests/test_gfci_policy.c. This mirrors the core/over_temp.h pattern.
 *
 * Policy values mirror GFCI_POLICY_* in proto/commands.h; a
 * _Static_assert in safety_task.c (which sees both headers) locks the
 * two against drift:
 *
 *   FAULT (0, default) — raise FAULT_GFCI, force EVSE_FAULT, contactor
 *                        latched open. Power-cycle-only clear per
 *                        UL2231 (fault.c::fault_clear() refuses GFCI).
 *   WARN  (1)          — emit one fault event so HA records the trip,
 *                        but do NOT raise into fault_state and do NOT
 *                        open the contactor — charging continues. A
 *                        bench/diagnostic escape for a known external
 *                        leakage fault; it suppresses a real safety
 *                        interlock.
 *
 * Any other policy value is treated as FAULT — an unknown or stale
 * byte fails safe to the UL2231 interlock.
 *
 * Debounce: PE2 must read LOW for GFCI_PERSIST_TICKS consecutive ticks
 * before any action. At the 20 ms safety tick that is 60 ms — well
 * under UL2231's 25 ms + upstream contactor-open budget for the trip
 * path, but long enough to ride out coupling glitches (cf. the PD6
 * bench-wiggle).
 *
 * WARN edge-latch: WARN emits exactly once per PE2-LOW episode; the
 * latch re-arms when PE2 returns HIGH, so a fault that clears and
 * re-asserts is logged again.
 *
 * gfci_policy_step() is static inline so the firmware inlines it
 * straight into check_gfci; core/gfci_policy.c is a stable compile
 * target carrying no logic. */

#define GFCI_PERSIST_TICKS  3U

/* Policy values — mirror GFCI_POLICY_* in proto/commands.h. */
#define GFCI_POL_FAULT  0u
#define GFCI_POL_WARN   1u

typedef struct {
    unsigned trip_streak;   /* consecutive ticks PE2 read LOW (capped) */
    int      warned;        /* WARN edge-latch: 1 once emitted this episode */
    int      fault_active;  /* 1 once GFCI_ACT_RAISE_FAULT has been returned */
} gfci_policy_ctx_t;

typedef enum {
    GFCI_ACT_NONE = 0,     /* nothing for the caller to do this tick */
    GFCI_ACT_WARN_EMIT,    /* WARN: emit one fault event, keep charging */
    GFCI_ACT_RAISE_FAULT,  /* FAULT: raise FAULT_GFCI + force EVSE_FAULT */
} gfci_policy_action_t;

/* Advance the detector one safety tick. pe2_low is 1 when the GFCI
 * module asserts its (active-low) fault line, 0 when idle. policy is
 * the persisted GFCI_POL_* byte. Updates *ctx in place and returns the
 * action the caller must carry out. */
static inline gfci_policy_action_t
gfci_policy_step(gfci_policy_ctx_t *ctx, int pe2_low, uint8_t policy)
{
    if (pe2_low) {
        if (ctx->trip_streak < GFCI_PERSIST_TICKS) ++ctx->trip_streak;
    } else {
        ctx->trip_streak = 0;
        ctx->warned = 0;   /* PE2 released — re-arm the WARN edge */
    }

    if (ctx->trip_streak < GFCI_PERSIST_TICKS)
        return GFCI_ACT_NONE;

    if (policy == GFCI_POL_WARN) {
        if (ctx->warned)
            return GFCI_ACT_NONE;   /* already emitted this episode */
        ctx->warned = 1;
        return GFCI_ACT_WARN_EMIT;
    }

    /* FAULT (default) — and any non-WARN value: fail-safe. Raise once;
     * the streak keeps accumulating harmlessly while latched. */
    if (ctx->fault_active)
        return GFCI_ACT_NONE;
    ctx->fault_active = 1;
    return GFCI_ACT_RAISE_FAULT;
}

#endif /* OPENEVCHARGER_CORE_GFCI_POLICY_H */
