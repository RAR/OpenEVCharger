#ifndef OPENEVCHARGER_BOARDS_NEXCYBER_HAL_BL0939_H
#define OPENEVCHARGER_BOARDS_NEXCYBER_HAL_BL0939_H

#include <stdint.h>

/* M3 BL0939 metering driver for the Nexcyber AC EVSE.
 *
 * Protocol-level port of src/hal/bl0939.{c,h} on the rippleon target.
 * Differences:
 *   - Uses hardware SPI2 (via boards/nexcyber/hal/spi2.h) instead of
 *     rippleon's bit-banged 3-wire SPI. Both transports send the same
 *     wire format — header byte (0x55 read / 0xA5 write), 8-bit
 *     register address, 24-bit data, 8-bit checksum (~(sum & 0xFF)).
 *   - Drives chip-select (NSS on PB12) around each frame.
 *   - Drops the per-chassis frequency calibration dependency — uses
 *     a hardcoded TPS reference until calibration_bl0939_freq_const()
 *     ports to nexcyber (depends on persist subsystem reaching M7+).
 *
 * Surface mirrors rippleon: read_register / write_register /
 * soft_reset / smoke_test / poll / get_readings. */

/* Reset BL0939's SPI state machine via 6 × 0xFF (datasheet § 3.1.5).
 * Call once before the first read after power-on or any spi2_init(). */
void bl0939_soft_reset(void);

/* Read a 24-bit register. Returns 0 on success, -1 on checksum mismatch.
 * *val is right-aligned (high byte = bits [23:16] of the BL0939 reg). */
int bl0939_read_register(uint8_t addr, uint32_t *val);

/* Write a 24-bit register. Returns 0 always (no readback verify). */
int bl0939_write_register(uint8_t addr, uint32_t v);

/* One-shot bench probe: soft-reset, then dump a few representative
 * registers to printk so an operator can verify the SPI is alive
 * before any owning task starts polling. */
void bl0939_smoke_test(void);

/* Snapshot of the BL0939 RMS / power registers. Identical layout to
 * src/hal/bl0939.h on rippleon so the FC41D-side TLV decoder can
 * reuse the same publish path once the boards converge. */
struct bl0939_readings {
    uint32_t v_rms;          /* 0x06 — voltage RMS, unsigned 24-bit */
    uint32_t ia_rms;         /* 0x04 — current A RMS, unsigned 24-bit */
    uint32_t ib_rms;         /* 0x05 — current B RMS, unsigned 24-bit */
    int32_t  a_watt;         /* 0x08 — channel A active power, signed 24-bit */
    uint32_t v_period;       /* 0x0E — voltage zero-crossing period, raw */
    uint16_t v_freq_hz_x10;  /* line frequency × 10 Hz (derived) */
    uint32_t poll_count;     /* total poll cycles attempted */
    uint32_t checksum_fail;  /* total reads that failed checksum */
    uint8_t  valid;          /* 0 until first successful poll */
};

/* Run one poll cycle: read V_RMS, IA_RMS, IB_RMS, A_WATT, TPS1 and
 * update the cache. Each SPI byte at 562 kHz ≈ 14 µs → full poll
 * (5 regs × 6 bytes + 4 frame gaps) ≈ 700 µs. */
void bl0939_poll(void);

/* Snapshot the latest readings into *out. Safe from any task. */
void bl0939_get_readings(struct bl0939_readings *out);

/* --- Register map (datasheet § 3.2, partial) ----------------------- */

#define BL0939_REG_IA_FAST_RMS      0x00
#define BL0939_REG_IA_WAVE          0x01
#define BL0939_REG_IB_WAVE          0x02
#define BL0939_REG_V_WAVE           0x03
#define BL0939_REG_IA_RMS           0x04
#define BL0939_REG_IB_RMS           0x05
#define BL0939_REG_V_RMS            0x06
#define BL0939_REG_IB_FAST_RMS      0x07
#define BL0939_REG_A_WATT           0x08
#define BL0939_REG_B_WATT           0x09
#define BL0939_REG_CFA_CNT          0x0A
#define BL0939_REG_CFB_CNT          0x0B
#define BL0939_REG_A_CORNER         0x0C
#define BL0939_REG_B_CORNER         0x0D
#define BL0939_REG_TPS1             0x0E
#define BL0939_REG_TPS2             0x0F
#define BL0939_REG_IA_FAST_RMS_CTRL 0x10
#define BL0939_REG_TPS_CTRL         0x1B
#define BL0939_REG_IB_FAST_RMS_CTRL 0x1E

#endif
