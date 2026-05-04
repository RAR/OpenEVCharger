#ifndef OPENBHZD_HAL_BL0939_H
#define OPENBHZD_HAL_BL0939_H

#include <stdint.h>

/* Bit-banged 3-wire SPI driver for the BL0939 metering IC at U11.
 *
 * Wiring (bench-confirmed 2026-05-04 — see core/pin_map.h for the
 * full chain):
 *   PB9   →  BL0939 pin 13 (SCLK)
 *   PD15  →  BL0939 pin 14 (RX/SDI)
 *   PD14  ←  BL0939 pin 15 (TX/SDO)
 *
 * GD32F205 has no SPI peripheral mapping for this triple, so the
 * interface is software-clocked. BL0939 max SPI clock is 900 kHz;
 * we run conservatively with explicit per-bit delays.
 *
 * Framing: see datasheet § 3.1 (read/write timing). The exact frame
 * structure (header byte, address byte, data + checksum) is NOT YET
 * implemented — that arrives in a follow-up commit once the
 * datasheet section is available. This module currently exposes
 * only the byte-level transfer primitive + a sanity-test entry
 * point so the wiring can be scoped on the bench.
 *
 * Single-owner rule: only one task should drive SCLK/SDI at a time.
 * At present that's the safety_task (eventually) — for now it's the
 * smoke test invoked once at boot. */

/* Initialize PB9/PD15 as output PP (idle low for SCLK, idle high for
 * SDI) and PD14 as input pull-up (BL0939 SDO is open-drain per
 * datasheet § 1.7.1, so we provide the pull-up). Idempotent. */
void bl0939_init(void);

/* Clock one byte out via SDI while clocking one byte in from SDO.
 * SPI mode 1 (CPOL=0, CPHA=1) — sample on falling edge of SCLK,
 * shift on rising edge. MSB first. Returns the captured byte from
 * SDO. Caller responsible for any chip-select equivalent (BL0939
 * has no CS pin; transactions are framed by SPI inactivity gap per
 * the datasheet's fault-tolerance section). */
uint8_t bl0939_xfer_byte(uint8_t out);

/* Smoke-test entry point. Sends a fixed byte pattern out SDI and
 * logs the raw bytes captured on SDO via printk. Doesn't interpret
 * results — it's strictly for confirming SCLK/SDI/SDO toggle on
 * the bench scope and that we get something other than 0x00 / 0xFF
 * back. Run once at boot before the safety_task starts owning the
 * pins. */
void bl0939_smoke_test(void);

#endif /* OPENBHZD_HAL_BL0939_H */
