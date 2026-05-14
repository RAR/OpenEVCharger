#ifndef OPENEVCHARGER_HAL_OEVC_HAL_STUB_H
#define OPENEVCHARGER_HAL_OEVC_HAL_STUB_H

/* Marks a HAL function that exists only so the production firmware target
 * for a board links — it is NOT a working implementation. Calling one at
 * runtime traps with interrupts disabled so a debugger breaks in cleanly.
 *
 * Greppable: `grep -rn OEVC_HAL_STUB src/hal/<chip>/` lists everything a
 * board still owes a real implementation. A board with zero hits is
 * feature-complete against the shared HAL interface.
 *
 * The production target for a board carrying these is a compile/link gate
 * only (see docs/superpowers/specs/2026-05-14-multi-mcu-board-structure-design.md);
 * the bench-harness target is what actually runs on hardware. */
#define OEVC_HAL_STUB() do { __asm volatile("cpsid i"); for (;;) {} } while (0)

#endif
