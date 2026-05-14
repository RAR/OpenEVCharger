/* Delta EVMU30 SysV shared-memory layout — SINGLE SOURCE OF TRUTH.
 *
 * RE-derived from decode_sharemem.py + docs/02 + docs/04. Every value here is
 * BENCH-VERIFY-PENDING (milestones M0/M1). All reads are single bytes per the
 * decoder. If a value is wrong, this is the only file to change.
 */
#ifndef SHMEM_OFFSETS_H
#define SHMEM_OFFSETS_H

#define SHMEM_KEY          0x153E      /* MeterSMKey  */
#define SHMEM_SIZE         0x40000     /* MeterSMSize, 256 KiB */

#define OFF_CONNECTOR_STATE 0x0a00     /* u8  — see CONNECTOR_STATE_* */
#define OFF_FAULT_FLAGS     0x0a07     /* u8  — coarse fault flags */
#define OFF_HEARTBEAT       0x0a08     /* u8  — Pri_Comm heartbeat counter */
#define OFF_STM32_LINK      0x0a0b     /* u8  — 1 = inter-MCU link OK */
#define OFF_VRMS            0x0a10     /* u8  — line voltage, raw */
#define OFF_IRMS            0x0a24     /* u8  — line current, raw */
#define OFF_FW_UPGRADE_GATE 0x0a63     /* u8  — firmware-upgrade gate flag */

#define OFF_ALARM_BITMAP    0x0138     /* 32 bytes — SHRMEM_TRAP_ALARM */
#define ALARM_BITMAP_LEN    32

#endif /* SHMEM_OFFSETS_H */
