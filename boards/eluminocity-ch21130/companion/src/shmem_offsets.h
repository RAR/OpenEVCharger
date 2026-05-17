/* Delta EVMU30 SysV shared-memory layout — SINGLE SOURCE OF TRUTH.
 *
 * RE-derived from static analysis of the unstripped, debug-symbol-bearing
 * ARMv5 producer binaries (Pri_Comm, main, Adc, MeterIC_new, ErrorHandle).
 * Full report + per-byte evidence: docs/06-shmem-RE-from-binaries.md.
 *
 * Endianness: multi-byte values are little-endian unless tagged BE.
 * (The producer code marshals u16/u32 byte-by-byte; reading byte-wise
 *  and combining is the only safe path because ARMv5 traps unaligned
 *  multi-byte loads at certain CP15 configs.)
 */
#ifndef SHMEM_OFFSETS_H
#define SHMEM_OFFSETS_H

#define SHMEM_KEY          0x153E      /* MeterSMKey  */
#define SHMEM_SIZE         0x40000     /* MeterSMSize, 256 KiB */

/* --- metering: stock chip→shmem fields (Pri_Comm INPUT, do not consume) -- */
/* What stock MeterIC_new writes for Pri_Comm to ship to the STM32 —
 * NOT human-unit V/I/P. 0x0000 and 0x0004 are both voltage-derived
 * (0x0004 = 0x0000 / 10); there is no Irms field in this compact
 * triple. Power at 0x000c is `power_raw / 100` (centi-units of the
 * chip's raw scale, not actual watts).
 *
 * Real cooked V/I/P/E for our web is in the BRIDGE block below — these
 * stock fields exist only so Pri_Comm keeps working when our meter
 * personality replaces stock MeterIC_new. */
#define OFF_STOCK_VRMS_CHIP    0x0000  /* u16 LE, vrms_raw / Vgain (Pri_Comm input) */
#define OFF_STOCK_VRMS_DECI    0x0004  /* u16 LE, OFF_STOCK_VRMS_CHIP / 10 (Pri_Comm input) */
#define OFF_STOCK_POWER_CHIP   0x000c  /* u32 LE, power_raw / 100 (Pri_Comm input) */

/* --- metering raw-readings telemetry (40-byte block at 0x015b..0x017e) --- */
/* Stock MeterIC_new + our meter personality write the chip's raw
 * VRMS/IRMS/POWER/ENERGY readings here. Consumers: stock `main` +
 * ErrorHandle scan these. Web uses the cooked BRIDGE block below. */
#define OFF_METER_RAW_VRMS     0x015b  /* u32 LE raw chip VRMS reading */
#define OFF_METER_RAW_IRMS     0x015f  /* u32 LE raw chip IRMS reading */
#define OFF_METER_RAW_POWER    0x0163  /* u32 LE raw chip POWER reading */
#define OFF_METER_RAW_ENERGY   0x0167  /* u32 LE raw chip ENERGY reading */

/* --- bridge-owned cooked metering (our personality writes; web reads) ---- */
/* Region 0x0500..0x050f is unowned by any stock daemon per the
 * shmem matrix (docs/14). Meter personality applies Vgain/Wgain plus
 * the empirical voltage scale (60.0; bench-fit against 120 V mains
 * 2026-05-16) and writes integer fixed-point so the web does no math.
 *
 * Formulas (bench-validated 2026-05-16):
 *   voltage_v = (vrms_raw  / Vgain) / 60.0      → centi-volts × 100
 *   power_w   = (power_raw / Wgain)
 *   current_a = power_w / voltage_v  (resistive-load valid)
 *   energy_wh = (energy_raw / Wgain) (= kWh × 1000) */
#define OFF_BRIDGE_VOLTAGE_CV  0x0500  /* u32 LE, centi-volts (V × 100) */
#define OFF_BRIDGE_CURRENT_MA  0x0504  /* u32 LE, milli-amps (A × 1000) */
#define OFF_BRIDGE_POWER_W     0x0508  /* u32 LE, integer watts */
#define OFF_BRIDGE_ENERGY_WH   0x050c  /* u32 LE, integer watt-hours (kWh × 1000) */

/* --- connector / state cluster ------------------------------------------- */
#define OFF_USER_STATE     0x0a00      /* u8  Green-LED / OCPP user state
                                          0=idle, 1=auth/ready, 2=charging */
#define OFF_RED_LED        0x0a01      /* u8  Red-LED state 0=off,1=solid,2=flash */
#define OFF_PRI_STATE      0x0a07      /* u8  Pri_Comm digested EVSE state
                                          (opaque enum; seen values 0,2,3,5) */
#define OFF_PILOT_STATE    0x0a08      /* u8  J1772 pilot classifier (Adc:PilotState)
                                          0=A,1=B,2=C,3=D,4=transient,5=F */
#define OFF_STM32_FAULT    0x0a0b      /* u8  STM32 link-fault bits
                                          bit 0x10 = UART tx/rx timeout */
#define OFF_PILOT_DUTY     0x0a10      /* u8  Pilot PWM duty % (clamped >=10) */
#define OFF_RATED_AMPS     0x0a24      /* u8  Configured/rated ampacity (A) */

/* --- the real alarm bitmap (NOT 0x0138 — that one is process-health) ----- */
#define OFF_ALARM_BITMAP   0x0a74      /* u32 LE, 32 alarm bits — see
                                          FAULT_NAMES[] in charger_state.c */

#endif /* SHMEM_OFFSETS_H */
