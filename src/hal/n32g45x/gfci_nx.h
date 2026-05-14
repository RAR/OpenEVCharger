#ifndef OPENEVCHARGER_HAL_N32G45X_GFCI_H
#define OPENEVCHARGER_HAL_N32G45X_GFCI_H

#include <stdint.h>
#include <stdbool.h>

/* GFCI CAL pulse driver — PB0 → ULN2003 → small relay coil that
 * injects a known residual-current test pulse through the GFCI CT.
 * The GFCI subsystem (module + CT, see project_nexcyber_gfci_subsystem
 * memory) should detect the test pulse and assert a fault, which the
 * MCU then sees as a brief deflection on the GFCI analog sense.
 *
 * Bench-confirmed 2026-05-09: pulsing PB0 HIGH for 500 ms produced an
 * audible relay click. Polarity assumed active-HIGH (HIGH at MCU →
 * ULN2003 sinks coil → relay closes); confirm with a coil-side scope
 * during M5+ integration. */

void gfci_init(void);

/* Pulse the CAL relay HIGH for `pulse_ms` milliseconds (typically
 * 200-500 ms — see rippleon's gfci.c for the bench-tuned value of
 * 500 ms pulse + 1000 ms recover). Caller chooses the delay strategy
 * via the delay_ms callback. */
void gfci_cal_pulse(uint32_t pulse_ms, void (*delay_ms)(uint32_t));

#endif
