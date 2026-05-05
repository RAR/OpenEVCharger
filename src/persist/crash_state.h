#ifndef OPENEVCHARGER_PERSIST_CRASH_STATE_H
#define OPENEVCHARGER_PERSIST_CRASH_STATE_H

#include <stdint.h>

#define CRASH_STATE_SLOT_A    0x04D000U
#define CRASH_STATE_SLOT_B    0x04E000U
#define CRASH_STATE_VERSION   1U
#define CRASH_LOOP_THRESHOLD  5U      /* fast_restart_count >= this → safe-fail */

/* 32 bytes total. Same envelope (counter @ 4, CRC @ end). */
struct __attribute__((packed)) crash_state {
    uint8_t  version;                   /* 1 */
    uint8_t  pad0[3];
    uint32_t monotonic_counter;         /* helper-managed */
    uint8_t  fast_restart_count;        /* boots without 60s of uptime */
    uint8_t  pad1[3];
    uint8_t  reserved[16];
    uint32_t crc32;                     /* helper-managed */
};
_Static_assert(sizeof(struct crash_state) == 32, "crash_state must be 32 B");

/* Load + increment fast_restart_count + persist. Sets safe_fail flag
 * if count >= threshold. Call once from main() pre-scheduler. */
int crash_state_boot_increment(void);

/* Reset fast_restart_count to 0 + persist. Called by persist_task when
 * io_task posts the "alive past 60 s" request. */
int crash_state_reset_alive(void);

/* Safe-fail flag accessor (for M6's safety_task). */
int crash_state_is_safe_fail(void);

#endif
