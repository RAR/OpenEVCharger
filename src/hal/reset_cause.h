#ifndef OPENEVCHARGER_HAL_RESET_CAUSE_H
#define OPENEVCHARGER_HAL_RESET_CAUSE_H

/* Chip-neutral reset-cause interface.
 *
 * The shared core (crash_state.c) needs to know *why* the MCU last
 * reset so it can distinguish a real crash (watchdog / low-power) from
 * a deliberate boot (power-on, NRST pin, software SYSRESETREQ). The raw
 * reset-cause register and its bit layout are chip-specific; this
 * header hides that behind a small decoded enum.
 *
 * Per-chip implementations live in src/hal/<chip>/reset_cause.c. */

typedef enum {
    RESET_CAUSE_UNKNOWN = 0,
    RESET_CAUSE_POWER_ON,
    RESET_CAUSE_PIN,         /* external NRST */
    RESET_CAUSE_SOFTWARE,    /* deliberate SYSRESETREQ */
    RESET_CAUSE_WATCHDOG,    /* any watchdog (independent or window) */
    RESET_CAUSE_LOW_POWER,
} reset_cause_t;

/* Read the reset-cause flags, clear them, return the decoded cause. Call
 * once early in boot — clearing is one-shot, so a second call returns
 * RESET_CAUSE_UNKNOWN. */
reset_cause_t reset_cause_get_and_clear(void);

/* Human-readable name for a decoded cause. Never returns NULL. */
const char   *reset_cause_str(reset_cause_t c);

#endif
