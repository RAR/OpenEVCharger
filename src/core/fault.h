#ifndef OPENEVCHARGER_CORE_FAULT_H
#define OPENEVCHARGER_CORE_FAULT_H

#include <stdint.h>

/* Fault catalog. Numeric values double as bit indices into the active
 * bitmap, so enum order matters. Latched faults come first to keep the
 * latched/self-clearing split contiguous. */
typedef enum {
    FAULT_NONE = 0,

    /* Latched (cleared only by power-cycle for GFCI, by FC41D
     * CLEAR_FAULT TLV otherwise). Bit 0 unused. */
    FAULT_GFCI                  = 1,
    FAULT_RELAY_WELD            = 2,
    FAULT_RELAY_STUCK_OPEN      = 3,
    FAULT_PE_CONTINUITY         = 4,
    FAULT_CP_NO_PILOT           = 5,   /* J1772 state E sustained */
    FAULT_DIODE_CHECK           = 6,
    FAULT_BOOT_SELF_TEST        = 7,
    FAULT_GFCI_SELF_TEST        = 8,
    FAULT_RELAY_WELD_AT_BOOT    = 9,
    FAULT_RELAY_OPEN_AT_BOOT    = 10,
    FAULT_ADC_OUT_OF_RANGE      = 11,
    FAULT_HARD_OVER_CURRENT     = 12,
    FAULT_CRASH_LOOP_SAFE_FAIL  = 13,

    /* Self-clearing (auto-clear when the underlying condition removes,
     * with hysteresis where applicable). */
    FAULT_OVER_TEMP             = 16,
    FAULT_SOFT_OVER_CURRENT     = 17,
    FAULT_CC_OUT_OF_RANGE       = 18,
    FAULT_AC_ABSENT             = 19,
    FAULT_CP_REGRESSION         = 20,

    FAULT_COUNT                 = 21
} fault_id_t;

#define FAULT_FIRST_SELF_CLEARING  FAULT_OVER_TEMP

/* Bitmask of latched fault ids — those that halt charging and require
 * intervention to clear. Used by the UI layer to gate red-flash LED +
 * buzzer alerts so self-clearing/informational faults (SOFT_OVER_CURRENT
 * post-derate, AC_ABSENT during a brief mains glitch, CP_REGRESSION on
 * graceful end-of-charge, etc.) don't beep + flash red on the user.
 * Bit 0 is unused (FAULT_NONE), bits 14-15 are gaps in the enum, and
 * the mask deliberately excludes both. */
#define FAULT_LATCHED_MASK \
    (((1u << FAULT_FIRST_SELF_CLEARING) - 1u) & ~1u)

typedef struct {
    uint32_t   active_bits;       /* bit n = fault id n is currently raised */
    fault_id_t first_raised;      /* first fault that drove the EVSE into FAULT */
} fault_state_t;

void fault_init(fault_state_t *s);

/* Returns 1 if this is a new raise (bit was clear), 0 if already set,
 * <0 on invalid id. */
int  fault_raise(fault_state_t *s, fault_id_t id);

/* Returns 1 if this cleared (bit was set), 0 if already clear,
 * <0 on invalid id or if id is GFCI (which is power-cycle only). */
int  fault_clear(fault_state_t *s, fault_id_t id);

int  fault_is_active(const fault_state_t *s, fault_id_t id);
int  fault_any_active(const fault_state_t *s);
int  fault_any_latched_active(const fault_state_t *s);

/* True if id is a latched-class fault (vs self-clearing). */
int  fault_is_latched_kind(fault_id_t id);

/* Clear all FC41D-clearable latched faults (everything latched except
 * GFCI). Returns count cleared. Does not touch self-clearing faults. */
int  fault_clear_all_clearable(fault_state_t *s);

/* Stable short string for logging / TLV. Never returns NULL. */
const char *fault_name(fault_id_t id);

#endif /* OPENEVCHARGER_CORE_FAULT_H */
