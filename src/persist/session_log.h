#ifndef OPENBHZD_PERSIST_SESSION_LOG_H
#define OPENBHZD_PERSIST_SESSION_LOG_H

#include <stddef.h>
#include <stdint.h>

#define SESSION_LOG_BASE                  0x044000U
#define SESSION_LOG_SECTORS               8U
#define SESSION_LOG_SECTOR_SIZE           4096U
#define SESSION_LOG_REGION_SIZE           (SESSION_LOG_SECTORS * SESSION_LOG_SECTOR_SIZE)
#define SESSION_LOG_RECORD_SIZE           32U
#define SESSION_LOG_RECORDS_PER_SECTOR    (SESSION_LOG_SECTOR_SIZE / SESSION_LOG_RECORD_SIZE)
#define SESSION_LOG_TOTAL_RECORDS         (SESSION_LOG_SECTORS * SESSION_LOG_RECORDS_PER_SECTOR)

/* 32 bytes total. Layout per spec § 6 with the addition of a u16
 * boot_count for cross-reboot ring tiebreak (matches event_record). */
struct __attribute__((packed)) session_record {
    uint32_t start_ts;
    uint32_t end_ts;
    uint32_t mwh_delivered;
    uint8_t  end_reason;
    uint8_t  j1772_max_state_seen;
    uint16_t fault_count;
    uint16_t max_temp_dC;
    uint16_t boot_count;
    uint8_t  reserved[10];
    uint16_t crc16;
};
_Static_assert(sizeof(struct session_record) == 32, "session_record must be 32 B");

int session_log_init(void);
void session_log_set_boot_count(uint16_t boot_count);
int session_log_append(struct session_record *rec);
int session_log_read_nth(uint32_t n, struct session_record *out_rec);
uint32_t session_log_head_slot(void);

#endif
