#include "event_log.h"
#include "crc16.h"
#include "../drivers/w25q.h"
#include "../hal/uart.h"
#include <string.h>

static uint32_t s_head_slot   = 0;
static uint16_t s_boot_count  = 0;
static int      s_initialized = 0;

static uint32_t slot_to_addr(uint32_t slot)
{
    return EVENT_LOG_BASE + slot * EVENT_LOG_RECORD_SIZE;
}

/* 1=valid CRC, 0=blank (all 0xFF), -1=corrupt (CRC fail, non-blank). */
static int classify_record(const struct event_record *rec)
{
    const uint8_t *b = (const uint8_t *)rec;
    int all_ff = 1;
    for (size_t i = 0; i < sizeof *rec; ++i) {
        if (b[i] != 0xFFu) { all_ff = 0; break; }
    }
    if (all_ff) return 0;

    uint16_t expected = crc16_ccitt(rec, sizeof *rec - 2U);
    if (rec->crc16 == expected) return 1;
    return -1;
}

int event_log_init(void)
{
    uint32_t valid = 0, corrupt = 0, blank = 0;
    uint32_t latest_boot = 0;
    uint32_t latest_ts   = 0;
    uint32_t latest_slot = 0;
    int      seen_valid  = 0;

    /* Read one sector (4 KB, 128 records) at a time to amortise
     * SPI read overhead. Static to avoid blowing main's stack. */
    static uint8_t sector_buf[EVENT_LOG_SECTOR_SIZE];

    for (uint32_t s = 0; s < EVENT_LOG_SECTORS; ++s) {
        w25q_read(EVENT_LOG_BASE + s * EVENT_LOG_SECTOR_SIZE,
                  sector_buf, EVENT_LOG_SECTOR_SIZE);

        for (uint32_t r = 0; r < EVENT_LOG_RECORDS_PER_SECTOR; ++r) {
            const struct event_record *rec =
                (const struct event_record *)(sector_buf + r * EVENT_LOG_RECORD_SIZE);
            int c = classify_record(rec);
            if (c == 0) { ++blank; continue; }
            if (c < 0) { ++corrupt; continue; }
            ++valid;

            uint32_t key_boot = rec->boot_count;
            uint32_t key_ts   = rec->timestamp;
            uint32_t slot     = s * EVENT_LOG_RECORDS_PER_SECTOR + r;
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

    if (seen_valid) {
        s_head_slot = (latest_slot + 1U) % EVENT_LOG_TOTAL_RECORDS;
    } else {
        s_head_slot = 0;
    }

    printk("event_log: scan complete: valid=%u corrupt=%u blank=%u head=0x%06x slot=%u\n",
           (unsigned)valid, (unsigned)corrupt, (unsigned)blank,
           (unsigned)slot_to_addr(s_head_slot), (unsigned)s_head_slot);

    s_initialized = 1;
    return 0;
}

void event_log_set_boot_count(uint16_t boot_count)
{
    s_boot_count = boot_count;
}

int event_log_append(struct event_record *rec)
{
    if (!s_initialized || rec == NULL) return -1;

    rec->boot_count = s_boot_count;
    rec->crc16 = crc16_ccitt(rec, sizeof *rec - 2U);

    uint32_t addr = slot_to_addr(s_head_slot);

    /* About to write to slot 0 of a sector? Erase if non-blank. */
    if (s_head_slot % EVENT_LOG_RECORDS_PER_SECTOR == 0U) {
        struct event_record probe;
        w25q_read(addr, &probe, sizeof probe);
        const uint8_t *b = (const uint8_t *)&probe;
        int needs_erase = 0;
        for (size_t i = 0; i < sizeof probe; ++i) {
            if (b[i] != 0xFFu) { needs_erase = 1; break; }
        }
        if (needs_erase) {
            if (w25q_erase_sector(addr) != 0) return -2;
        }
    }

    if (w25q_program(addr, rec, sizeof *rec) != 0) return -3;

    s_head_slot = (s_head_slot + 1U) % EVENT_LOG_TOTAL_RECORDS;
    return 0;
}

int event_log_read_nth(uint32_t n, struct event_record *out_rec)
{
    if (n >= EVENT_LOG_TOTAL_RECORDS || out_rec == NULL) return -1;
    w25q_read(slot_to_addr(n), out_rec, sizeof *out_rec);
    return classify_record(out_rec) == 1 ? 0 : 1;
}

uint32_t event_log_head_slot(void)
{
    return s_head_slot;
}
