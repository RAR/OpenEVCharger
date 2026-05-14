#ifndef OPENEVCHARGER_HAL_GPIO_H
#define OPENEVCHARGER_HAL_GPIO_H

#include <stdint.h>

/* One-shot bulk GPIO config for every load-bearing pin in pin_map.h.
 * Idempotent. Must run after uart_init() so any failure can printk(),
 * but before any task touches a pin. main() calls it once. */
void gpio_init_all(void);

/* Read-and-log straps (DIPs + miscellaneous strap inputs). Called once
 * after gpio_init_all() to printk a single line summarising boot config. */
void gpio_log_straps(void);

/* DIP4 held LOW at boot: "FC41D flash mode" — main() skips
 * comms_task_create() so uart5_init() never runs and the UART4 wires
 * (PC12 TX / PD2 RX, also FC41D's UART1 P10/P11) stay tri-stated. The
 * FC41D's serial bootloader can then handshake without contention.
 * Caller must read this after gpio_init_all() (pull-up settled).
 * Returns 1 if held (active-low), 0 if released. */
int  gpio_dip4_held(void);

/* Write/read a single GPIO pin by (port, pin) handle. The port/pin values
 * come from the board's pin_map.h PIN_*_PORT / PIN_*_PIN macros — opaque
 * here, interpreted by the per-chip implementation. level: non-zero = high. */
void gpio_pin_write(uint32_t port, uint16_t pin, int level);
int  gpio_pin_read(uint32_t port, uint16_t pin);

#endif
