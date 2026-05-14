#include "session_log.h"
#include "crc16.h"
#include "../drivers/w25q.h"
#include "../hal/uart.h"
#include <string.h>

static uint32_t s_head_slot   = 0;
static uint16_t s_boot_count  = 0;
static int      s_initialized = 0;

static uint32_t slot_to_addr(uint32_t slot)
{
    return SESSION_LOG_BASE + slot * SESSION_LOG_RECORD_SIZE;
}

static int classify(const struct session_record *rec)
{
    const uint8_t *b = (const uint8_t *)rec;
    int all_ff = 1;
    for (size_t i = 0; i < sizeof *rec; ++i) {
        if (b[i] != 0xFFu) { all_ff = 0; break; }
    }
    if (all_ff) return 0;
    uint16_t expected = crc16_ccitt(rec, sizeof *rec - 2U);
    return (rec->crc16 == expected) ? 1 : -1;
}

int session_log_init(void)
{
    uint32_t valid = 0, corrupt = 0, blank = 0;
    uint32_t latest_boot = 0;
    uint32_t latest_ts   = 0;
    uint32_t latest_slot = 0;
    int      seen_valid  = 0;

    static uint8_t sector_buf[SESSION_LOG_SECTOR_SIZE];

    for (uint32_t s = 0; s < SESSION_LOG_SECTORS; ++s) {
        w25q_read(SESSION_LOG_BASE + s * SESSION_LOG_SECTOR_SIZE,
                  sector_buf, SESSION_LOG_SECTOR_SIZE);
        for (uint32_t r = 0; r < SESSION_LOG_RECORDS_PER_SECTOR; ++r) {
            const struct session_record *rec =
                (const struct session_record *)(sector_buf + r * SESSION_LOG_RECORD_SIZE);
            int c = classify(rec);
            if (c == 0) { ++blank; continue; }
            if (c < 0) { ++corrupt; continue; }
            ++valid;

            uint32_t key_boot = rec->boot_count;
            uint32_t key_ts   = rec->start_ts;
            uint32_t slot     = s * SESSION_LOG_RECORDS_PER_SECTOR + r;
            if (!seen_valid ||
                key_boot > latest_boot ||
                (key_boot == latest_boot && key_ts >= latest_ts)) {
                latest_boot = key_boot;
                latest_ts   = key_ts;
                latest_slot = slot;
                seen_valid  = 1;
            }
        }
    }

    s_head_slot = seen_valid
        ? (latest_slot + 1U) % SESSION_LOG_TOTAL_RECORDS
        : 0;

    printk("session_log: scan complete: valid=%u corrupt=%u blank=%u head=0x%06x slot=%u\n",
           (unsigned)valid, (unsigned)corrupt, (unsigned)blank,
           (unsigned)slot_to_addr(s_head_slot), (unsigned)s_head_slot);

    s_initialized = 1;
    return 0;
}

void session_log_set_boot_count(uint16_t boot_count)
{
    s_boot_count = boot_count;
}

int session_log_append(struct session_record *rec)
{
    if (!s_initialized || rec == NULL) return -1;

    rec->boot_count = s_boot_count;
    rec->crc16 = crc16_ccitt(rec, sizeof *rec - 2U);

    uint32_t addr = slot_to_addr(s_head_slot);

    if (s_head_slot % SESSION_LOG_RECORDS_PER_SECTOR == 0U) {
        struct session_record probe;
        w25q_read(addr, &probe, sizeof probe);
        const uint8_t *b = (const uint8_t *)&probe;
        int needs_erase = 0;
        for (size_t i = 0; i < sizeof probe; ++i) {
            if (b[i] != 0xFFu) { needs_erase = 1; break; }
        }
        if (needs_erase && w25q_erase_sector(addr) != 0) return -2;
    }

    if (w25q_program(addr, rec, sizeof *rec) != 0) return -3;

    s_head_slot = (s_head_slot + 1U) % SESSION_LOG_TOTAL_RECORDS;
    return 0;
}

int session_log_read_nth(uint32_t n, struct session_record *out_rec)
{
    if (n >= SESSION_LOG_TOTAL_RECORDS || out_rec == NULL) return -1;
    w25q_read(slot_to_addr(n), out_rec, sizeof *out_rec);
    return classify(out_rec) == 1 ? 0 : 1;
}

uint32_t session_log_head_slot(void) { return s_head_slot; }
