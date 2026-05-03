#ifndef OPENBHZD_HAL_SPI3_H
#define OPENBHZD_HAL_SPI3_H

#include <stdint.h>

/* Initialise SPI3 (= SPL SPI2) at 15 MHz, mode 0, 8-bit MSB-first.
 * GPIO pads (PB3/4/5/6) must already be configured by gpio_init_all().
 * Idempotent. After this returns, spi3_xfer() is usable. */
void spi3_init(void);

/* Synchronous full-duplex byte exchange. CS handling is the caller's
 * responsibility — see spi3_cs_assert/deassert. */
uint8_t spi3_xfer(uint8_t tx);

void spi3_cs_assert(void);
void spi3_cs_deassert(void);

#endif
