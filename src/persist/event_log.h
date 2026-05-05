#ifndef OPENEVCHARGER_PERSIST_EVENT_LOG_H
#define OPENEVCHARGER_PERSIST_EVENT_LOG_H

#include <stddef.h>
#include <stdint.h>

#define EVENT_LOG_BASE                  0x004000U
#define EVENT_LOG_SECTORS               64U
#define EVENT_LOG_SECTOR_SIZE           4096U
#define EVENT_LOG_REGION_SIZE           (EVENT_LOG_SECTORS * EVENT_LOG_SECTOR_SIZE)
#define EVENT_LOG_RECORD_SIZE           32U
#define EVENT_LOG_RECORDS_PER_SECTOR    (EVENT_LOG_SECTOR_SIZE / EVENT_LOG_RECORD_SIZE)
#define EVENT_LOG_TOTAL_RECORDS         (EVENT_LOG_SECTORS * EVENT_LOG_RECORDS_PER_SECTOR)

/* 32 bytes total. Layout per spec § 6. CRC16 covers bytes 0..29. */
struct __attribute__((packed)) event_record {
    uint32_t timestamp;
    uint16_t boot_count;
    uint16_t fault_id;
    uint8_t  j1772_state;
    uint8_t  evse_state;
    int16_t  cp_mv;
    uint16_t cc_amps;
    uint16_t ntc1_dC;
    uint16_t ntc2_dC;
    uint16_t active_amps_x10;
    uint8_t  reserved[10];
    uint16_t crc16;
};
_Static_assert(sizeof(struct event_record) == 32, "event_record must be 32 B");

/* Scan the entire log to find the head (next-write slot). Idempotent;
 * safe to call once at boot. Returns 0 always — corruption is logged
 * but not fatal. */
int event_log_init(void);

/* Caller (main) supplies the current boot_count after M5's
 * boot_count_increment() runs. Used by event_log_append() to stamp
 * each record. */
void event_log_set_boot_count(uint16_t boot_count);

/* Append a record. Caller fills all fields EXCEPT boot_count and crc16
 * — the helper sets boot_count from event_log_set_boot_count() and
 * computes crc16. Returns 0 on success, <0 on W25Q error. */
int event_log_append(struct event_record *rec);

/* Debug: read the n-th record from the start of the log into out_rec.
 * Returns 0 on valid CRC, 1 on blank slot, <0 on out-of-range. */
int event_log_read_nth(uint32_t n, struct event_record *out_rec);

/* Debug: current next-write slot (0..EVENT_LOG_TOTAL_RECORDS-1). */
uint32_t event_log_head_slot(void);

#endif
