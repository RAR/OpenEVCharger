#include "boot_count.h"
#include "crc.h"
#include "../hal/w25q.h"
#include <string.h>

#define BOOT_COUNT_SECTOR  0x04C000U

/* 32-byte record. Layout matches the spec's "boot_count + last_fault"
 * description but only `count` is used in M5 — `last_fault_id` will be
 * populated by M6's safety supervisor. */
struct __attribute__((packed)) boot_count_record {
    uint8_t  version;
    uint8_t  pad0[3];
    uint32_t count;
    uint16_t last_fault_id;
    uint8_t  reserved[18];
    uint32_t crc32;
};

uint32_t boot_count_increment(void)
{
    struct boot_count_record rec;
    w25q_read(BOOT_COUNT_SECTOR, &rec, sizeof rec);

    uint32_t computed = crc32(&rec, sizeof rec - sizeof(uint32_t));
    uint32_t cur = (rec.version == 1 && rec.crc32 == computed) ? rec.count : 0;

    memset(&rec, 0, sizeof rec);
    rec.version = 1;
    rec.count   = cur + 1;
    rec.crc32   = crc32(&rec, sizeof rec - sizeof(uint32_t));

    if (w25q_erase_sector(BOOT_COUNT_SECTOR) != 0) return 0xFFFFFFFFu;
    if (w25q_program(BOOT_COUNT_SECTOR, &rec, sizeof rec) != 0) return 0xFFFFFFFFu;

    return rec.count;
}
