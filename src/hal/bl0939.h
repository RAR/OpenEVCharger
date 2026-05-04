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
 * we run conservatively below that.
 *
 * SPI mode 1 (CPOL=0, CPHA=1): SCLK idles LOW, master shifts data
 * on rising edge, samples on falling edge. MSB first. 8-bit
 * transfers. Half-duplex (no chip-select).
 *
 * Frame format per datasheet § 3.1.2:
 *   Write: 0xA5 + ADDR + DATA_H + DATA_M + DATA_L + CHECKSUM   (48 bits)
 *   Read:  0x55 + ADDR  →  DATA_H + DATA_M + DATA_L + CHECKSUM (48 bits)
 *   Checksum = ~((header + ADDR + DATA_H + DATA_M + DATA_L) & 0xFF)
 *   All data is 3-byte / 24-bit; smaller registers are MSB-padded
 *   with zero.
 *
 * Single-owner rule: only one task should drive SCLK/SDI at a time. */

/* Initialize PB9/PD15 as output PP and PD14 as input pull-up. */
void bl0939_init(void);

/* Send a 6-byte 0xFF burst — datasheet § 3.1.5 soft-reset. Resets
 * the BL0939's SPI state machine to "expecting header byte". Call
 * before the first read/write if there's any chance of half-frame
 * state from a prior boot or transient. */
void bl0939_soft_reset(void);

/* Read a 24-bit register. Returns 0 on success, -1 on checksum
 * mismatch. Output is right-aligned in *val (high byte = bits
 * [23:16] of the BL0939 register). */
int bl0939_read_register(uint8_t addr, uint32_t *val);

/* Write a 24-bit value to a register. The lower 24 bits of `val`
 * are sent (high byte first). Returns 0 always (no readback to
 * verify). */
int bl0939_write_register(uint8_t addr, uint32_t val);

/* One-shot bench probe. Soft-resets, then reads a few representative
 * registers (IA_FAST_RMS_CTRL default 0xFFFF, V_RMS, IA_RMS, IB_RMS)
 * and prints raw values + checksum verdict via printk. Run once at
 * boot before any owning task starts using the link. */
void bl0939_smoke_test(void);

/* --- Periodic poll cache ------------------------------------------ */

/* Snapshot of the BL0939 RMS / power registers. All fields are 24-bit
 * raw values from the chip; signed registers (A_WATT, B_WATT) are
 * sign-extended into int32_t. checksum_fail counts read failures since
 * boot — useful for the link-health binary sensor on the FC41D side.
 * `valid` is 0 before the first successful poll round.
 *
 * Updated atomically (struct copy under irq-disable in writer) by
 * bl0939_poll(); readers via bl0939_get_readings() see a consistent
 * snapshot. */
struct bl0939_readings {
    uint32_t v_rms;            /* 0x06 — voltage RMS, unsigned 24-bit */
    uint32_t ia_rms;           /* 0x04 — current A RMS, unsigned 24-bit */
    uint32_t ib_rms;           /* 0x05 — current B RMS, unsigned 24-bit */
    int32_t  a_watt;           /* 0x08 — channel A active power, signed 24-bit */
    uint32_t poll_count;       /* total poll cycles attempted */
    uint32_t checksum_fail;    /* total reads that failed checksum */
    uint8_t  valid;            /* 0 until first poll completes any read */
};

/* Run one poll cycle: read V_RMS, IA_RMS, IB_RMS, A_WATT and update
 * the cache. Bit-banged transfers take ~1 ms total at the current
 * 5 µs bit timing. Caller is responsible for cadence (BL0939 RMS
 * registers update at 400-800 ms; polling faster wastes cycles). */
void bl0939_poll(void);

/* Snapshot the latest readings into *out. Safe to call from any
 * task; uses irq-disable around the copy (struct < 32 bytes).
 * Returns *out unchanged if no poll has run yet (valid == 0). */
void bl0939_get_readings(struct bl0939_readings *out);

/* --- Register map (datasheet, partial) ------------------------------ */

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
#define BL0939_REG_IA_RMSOS         0x13
#define BL0939_REG_IB_RMSOS         0x14
#define BL0939_REG_A_WATTOS         0x15
#define BL0939_REG_B_WATTOS         0x16
#define BL0939_REG_WA_CREEP         0x17
#define BL0939_REG_MODE             0x18
#define BL0939_REG_TPS_CTRL         0x1B
#define BL0939_REG_TPS2_A           0x1C
#define BL0939_REG_TPS2_B           0x1D
#define BL0939_REG_IB_FAST_RMS_CTRL 0x1E

#endif /* OPENBHZD_HAL_BL0939_H */
