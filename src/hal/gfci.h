#ifndef OPENEVCHARGER_HAL_GFCI_H
#define OPENEVCHARGER_HAL_GFCI_H

#include <stdint.h>

/* GFCI fault sense on PE2 + continuous CAL refresh on PE3.
 *
 * Architecture (bench-RE'd against stock V1.0.063, locked 2026-05-24):
 *   The external GFCI/CCID module needs CONTINUOUS refresh on its
 *   CAL input or it asserts permanent fault as a fail-safe. The
 *   refresh cycle is ~6 s of armed-state + ~1 s of pulse, repeated
 *   forever. During each refresh pulse the chip briefly pulls TRIP
 *   LOW (~300 ms) as a "still alive + CT connected" handshake.
 *
 *   PE3 (CAL drive) goes through an inverting level shifter:
 *     MCU PE3 LOW   → CAL HIGH at chip   (chip's armed-idle state)
 *     MCU PE3 HIGH  → CAL LOW at chip    (refresh pulse)
 *     MCU PE3 float → CAL slowly rises HIGH via RC
 *
 *   PE2 (fault sense) is active-LOW at the MCU side:
 *     PE2 HIGH = chip idle, no fault
 *     PE2 LOW  = chip asserting fault — either handshake OR real
 *                leak. Distinguish via gfci_in_handshake_window().
 *
 * boot path:
 *   1. gpio_init_all() configures PE3 as floating INPUT, PE2 as IPU
 *   2. gfci_init() defensively re-asserts PE2 IPU
 *   3. gfci_self_test() runs the boot init dance + cycles until the
 *      first handshake is observed (PASS) or 4 cycles expire (FAIL)
 *   4. After PASS, gfci_refresh_task is spawned to keep the cycle
 *      running for the rest of the boot.
 *
 * Single-reader rule: only safety_task should call gfci_fault_active().
 * Other tasks observe the fault via system_state's fault_active_bits. */

void gfci_init(void);

/* Returns 1 if PE2 reads LOW (= chip asserting fault — either real
 * leak OR the brief refresh-cycle handshake), 0 if PE2 reads HIGH
 * (= idle / no fault). Caller (safety_task) must check
 * gfci_in_handshake_window() to disambiguate. */
int  gfci_fault_active(void);

/* Returns non-zero while gfci_refresh_task is in the refresh-pulse
 * window plus its tail (~1.5 s per cycle). safety_task masks any
 * PE2 LOW that occurs during this window — it's the chip's expected
 * handshake, not a real fault. */
int  gfci_in_handshake_window(void);

/* Boot init dance + first-handshake validation. Returns:
 *    0   PASS — observed at least one chip handshake within 4 cycles
 *   -1   FAIL — no handshake during the ~31 s boot test window
 *   -3   sense already asserted at entry (live fault, can't test)
 *
 * Contains ~3 s of init dance + up to 4 × ~7.5 s refresh cycles.
 * Per-poll wdg_kick keeps inside the 1 s IWDG. Drives PE3 actively
 * via gpio_init mode changes; safe to call only before the refresh
 * task starts. */
int  gfci_self_test(void);

/* Continuous-refresh task entry. Spawn ONCE after gfci_self_test
 * returns PASS. Owns PE3 (drives the 6 s + 1 s cycle forever) and
 * the s_handshake_window flag. Never returns. Stack: 256 words. */
void gfci_refresh_task(void *arg);

#endif /* OPENEVCHARGER_HAL_GFCI_H */
