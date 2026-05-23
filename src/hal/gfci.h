#ifndef OPENEVCHARGER_HAL_GFCI_H
#define OPENEVCHARGER_HAL_GFCI_H

#include <stdint.h>

/* GFCI fault sense on PE2.
 *
 * Bench-confirmed 2026-05-04 (gpio_diff wiggle): the GFCI module's
 * fault output drives PE2 LOW when a leakage fault is detected; idle
 * HIGH (open-drain output with a pull-up — the MCU's internal pull-up
 * is enabled so the wire reads HIGH when the module isn't asserting).
 *
 * Configuration is owned by `gpio_init_all()` (PE2 = input pull-up
 * via the legacy `PIN_STRAP_PE2_*` macros, which happen to give us
 * exactly the right config — see core/pin_map.h's GFCI block).
 *
 * The CAL line on PE3 is a separate output, currently configured as
 * output PP idle LOW (= CAL idle at GFCI side per the level-shift
 * inversion). Self-test cycle (drive PE3 + PE4, sample PE2 mid-pulse)
 * is not yet implemented — we only watch for runtime faults.
 *
 * Single-reader rule: only safety_task should call gfci_fault_active().
 * Other tasks observe the fault via system_state's fault_active_bits. */

void gfci_init(void);

/* Returns 1 if PE2 reads LOW (= GFCI module is asserting fault),
 * 0 if PE2 reads HIGH (= idle / no fault). Active-low. */
int  gfci_fault_active(void);

/* Diagnostic capture from a single CAL self-test run. All fields are
 * populated before gfci_self_test() returns; pass NULL to skip capture.
 * Time fields are ms-from-CAL-pulse-start (so first_edge_ms == 1 means
 * PE2 went LOW on the first 10 ms poll after pulse assert). */
typedef struct {
    int8_t   rc;                   /* mirrors return: 0 PASS, -1/-2/-3 fail */
    uint8_t  pe3_idle_level;       /* 0 LOW, 1 HIGH — captured at start */
    uint8_t  saw_assert;           /* 1 if PE2 went LOW anywhere in pulse+recover */
    uint8_t  saw_release;          /* 1 if PE2 returned HIGH after asserting */
    uint16_t first_edge_ms;        /* 0 = never; ms-since-pulse-start of first assert */
    uint16_t release_edge_ms;      /* 0 = never; ms-since-pulse-start of release */
} gfci_cal_diag_t;

/* Wire-size lock: this struct is memcpy'd into event_record.reserved[] on
 * persisted GFCI_SELF_TEST fault entries (so the FC41D-side "Dump Fault
 * Log" survives the FC41D being power-cycled with the MCU) AND embedded
 * after the legacy 8 bytes of the EVT_BOOT_COMPLETE payload. Both
 * decoders parse it field-by-field at fixed offsets, so any change here
 * is a wire-protocol change. Keep it 8 bytes. */
_Static_assert(sizeof(gfci_cal_diag_t) == 8, "gfci_cal_diag_t must be 8 B");

/* Polarity-agnostic CAL self-test pulse. Drives PE3 to the inverse
 * of its idle level for ~500 ms, polls PE2 for an assertion edge, then
 * restores PE3 and waits up to ~1000 ms for PE2 to release.
 *   0  = PASS
 *  -1  = no sense edge during CAL pulse
 *  -2  = sense stuck-low after CAL release
 *  -3  = sense already asserted at start (live fault?)
 * `diag` may be NULL; when non-NULL its fields are populated regardless
 * of pass/fail so the boot path can report what was actually observed.
 * Note: contains ~1500 ms of busy-wait (pulse + recover). Acceptable
 * because wdg_kick() runs per-poll; runs once at boot before the
 * scheduler starts. Bench-use only. */
int  gfci_self_test(gfci_cal_diag_t *diag);

#endif /* OPENEVCHARGER_HAL_GFCI_H */
