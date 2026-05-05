#ifndef OPENEVCHARGER_PERSIST_RFID_AUTHLIST_H
#define OPENEVCHARGER_PERSIST_RFID_AUTHLIST_H

#include <stdint.h>
#include <stddef.h>

#define RFID_AUTHLIST_SLOT_A   0x005000U
#define RFID_AUTHLIST_SLOT_B   0x006000U
#define RFID_AUTHLIST_VERSION  1U
#define RFID_AUTHLIST_MAX      28U   /* 28 × 4B = 112 B fits in 128 B record */

/* 128 B ping-pong record (≤ 256 B page limit per pingpong helper).
 *   bytes [0]              version (1)
 *   bytes [1..3]           pad
 *   bytes [4..7]           u32 monotonic_counter (helper-managed)
 *   bytes [8]              u8 count (0..28)
 *   bytes [9..11]          pad
 *   bytes [12..123]        u32 uids[28] little-endian
 *   bytes [124..127]       u32 crc32 (helper-managed) */
struct __attribute__((packed)) rfid_authlist_record {
    uint8_t  version;
    uint8_t  pad0[3];
    uint32_t monotonic_counter;
    uint8_t  count;
    uint8_t  pad1[3];
    uint32_t uids[RFID_AUTHLIST_MAX];
    uint32_t crc32;
};
_Static_assert(sizeof(struct rfid_authlist_record) == 128,
               "rfid_authlist record must be 128 B");

/* Load list from W25Q into the in-RAM cache. If both slots are invalid,
 * writes an empty record to slot A. Returns 0 on success, <0 on error. */
int rfid_authlist_load(void);

/* Number of UIDs currently stored. */
uint8_t rfid_authlist_count(void);

/* Read the n-th UID (0..count-1). Returns 0 + writes *out, -1 if idx
 * out of range. */
int rfid_authlist_get_nth(uint8_t idx, uint32_t *out_uid);

/* 1 if uid is in the list, 0 otherwise. uid==0 is never a member. */
int rfid_authlist_contains(uint32_t uid);

/* Add uid. Idempotent (returns 0 if already present, 1 if newly added,
 * -1 on full list, -2 on persist error, -3 on uid==0). */
int rfid_authlist_add(uint32_t uid);

/* Remove uid. Returns 1 if removed, 0 if not present, <0 on persist
 * error. */
int rfid_authlist_remove(uint32_t uid);

/* Empty the list. Returns 0 on success, <0 on persist error. */
int rfid_authlist_clear(void);

#endif
