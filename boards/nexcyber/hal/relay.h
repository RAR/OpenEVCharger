#ifndef OPENEVCHARGER_BOARDS_NEXCYBER_HAL_RELAY_H
#define OPENEVCHARGER_BOARDS_NEXCYBER_HAL_RELAY_H

#include <stdint.h>
#include <stdbool.h>

/* Contactor driver for the Nexcyber EVSE.
 *
 * Two-pin model (bench-confirmed 2026-05-11):
 *   PA1 = CLOSE PULSE — one-shot at session start, external SR latch
 *         holds the line contactors after release.
 *   PA0 = HOLD/PERMIT — must be asserted HIGH for the entire charging
 *         session; release → external latch reset → contactors open.
 *
 * Safety: relay_close() asserts PA0 HIGH then pulses PA1 for the
 * configured close-pulse duration. relay_open() drops PA0 LOW; the
 * external latch resets and contactors open. PA1 is always released
 * (LOW) at steady state — only fires during the close transition.
 *
 * v1 default close-pulse width: 50 ms (TBD bench-tune; rapid-SWD
 * sample across a charge-start event will pin the actual stock-fw
 * pulse width).
 *
 * Important — call relay_init() AFTER gpio_init_all() so the pads
 * are already configured OUT_PP. relay_init() just zeroes ODR. */

void relay_init(void);

/* Close: assert PA0 HIGH first (so the SR latch's reset isn't blocked),
 * then pulse PA1 HIGH for `pulse_ms` milliseconds. Caller chooses the
 * delay strategy (busy-wait, vTaskDelay, etc.) via the delay_ms callback.
 *
 * Returns true on success (relay command issued). No readback verify
 * (M5+ adds contactor-feedback detection via the BL0939 current
 * channels). */
bool relay_close(uint32_t pulse_ms, void (*delay_ms)(uint32_t));

/* Open: drop PA0 LOW. The external SR latch resets and contactors open.
 * Idempotent. */
void relay_open(void);

/* Snapshot current driver state (not the external latch state). */
bool relay_hold_asserted(void);

#endif
