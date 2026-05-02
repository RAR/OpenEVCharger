#ifndef OPENBHZD_HAL_GPIO_H
#define OPENBHZD_HAL_GPIO_H

#include <stdint.h>

/* One-shot bulk GPIO config for every load-bearing pin in pin_map.h.
 * Idempotent. Must run after uart_init() so any failure can printk(),
 * but before any task touches a pin. main() calls it once. */
void gpio_init_all(void);

/* Read-and-log straps (DIPs + miscellaneous strap inputs). Called once
 * after gpio_init_all() to printk a single line summarising boot config. */
void gpio_log_straps(void);

#endif
