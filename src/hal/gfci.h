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

#endif /* OPENEVCHARGER_HAL_GFCI_H */
