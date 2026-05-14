#ifndef OPENEVCHARGER_HAL_BOARD_INIT_H
#define OPENEVCHARGER_HAL_BOARD_INIT_H

/* Board/chip-specific bring-up steps that are not portable across MCUs
 * (vendor clock fix-ups, debug-pin remaps, AF clock enables, companion-
 * module power sequencing). main() calls these instead of inlining raw
 * vendor-SPL code. Implemented per-chip in src/hal/<chip>/board_init.c.
 *
 * NOTE on decomposition: the pre-Task-10 src/main.c ran its raw vendor
 * code at THREE non-contiguous points (clock fix-up before uart_init;
 * debug-pin/AF remap after the RTC block; FC41D power-up in the comms
 * branch). Folding all of it into one hook would reorder the bl-call
 * stream in main() and break the codegen-neutrality gate, so the hooks
 * are kept 1:1 with the original call sites. */

/* Vendor clock fix-up. Call FIRST in main(), before uart_init() —
 * matches the original `clock_real_120m_init()` call site. */
void board_early_init(void);

/* Debug-pin / AF-clock remap so later AF peripheral inits land on the
 * right pads. Call after the RTC block, before gpio_init_all() —
 * matches the original rcu_periph_clock_enable()/gpio_pin_remap_config()
 * call site. */
void board_debug_pins_init(void);

/* Power-sequence the FC41D Wi-Fi companion module: assert its supply
 * enable, wait, then release its chip-enable so its firmware boots and
 * starts answering on the comms UART. Called from the normal-mode
 * (non-DIP4) branch of main() just before comms_task_create(). */
void board_fc41d_release(void);

#endif
