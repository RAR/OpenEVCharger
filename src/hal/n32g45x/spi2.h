#ifndef OPENEVCHARGER_BOARDS_NEXCYBER_HAL_SPI2_H
#define OPENEVCHARGER_BOARDS_NEXCYBER_HAL_SPI2_H

#include <stdint.h>

/* M3 hardware SPI2 driver for the BL0939 metering link.
 *
 * Pads (bench-confirmed 2026-05-11 — see pin_map.h):
 *   PB12  → BL0939 NSS  (software-driven via OUT_PP)
 *   PB13  → BL0939 SCK  (AF_PP)
 *   PB14  ← BL0939 MISO (AF input)
 *   PB15  → BL0939 MOSI (AF_PP)
 *
 * Mode 1 (CPOL=0, CPHA=1) to match BL0939's "shift on rising,
 * sample on falling" specification. ~562 kHz SCK (APB1 / 64 at
 * the default 144 MHz sysclk chain) — well under the BL0939's
 * 900 kHz max. NSS is software so each transfer is wrapped in
 * spi2_cs_assert / spi2_cs_deassert by the caller. */

void spi2_init(void);
uint8_t spi2_xfer(uint8_t tx);
void spi2_cs_assert(void);
void spi2_cs_deassert(void);

#endif
