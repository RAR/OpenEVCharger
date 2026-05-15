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

/* --- metering (measured by MeterIC chip via MeterIC_new) ----------------- */
#define OFF_VRMS_MEAS      0x0000      /* u16 LE, raw/10 = volts */
#define OFF_IRMS_MEAS      0x0004      /* u16 LE, raw/10 = amps  */
#define OFF_POWER_MEAS     0x000c      /* u32 LE, raw/1000 = watts (TBD) */

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
