#include "pingpong.h"
#include "crc.h"
#include "../drivers/w25q.h"
#include <string.h>

#define COUNTER_OFF   4U

static uint32_t le32_load(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void le32_store(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v        & 0xFFu);
    p[1] = (uint8_t)((v >>  8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Read sector at `addr` into `buf` and check CRC. Returns 1 if valid,
 * 0 if invalid (blank or CRC mismatch). */
static int read_and_validate(uint32_t addr, uint8_t *buf, size_t size,
                             uint32_t *out_counter)
{
    w25q_read(addr, buf, size);

    uint32_t stored_crc = le32_load(buf + size - 4U);
    uint32_t expected   = crc32(buf, size - 4U);
    if (stored_crc != expected) return 0;

    *out_counter = le32_load(buf + COUNTER_OFF);
    return 1;
}

int pingpong_load(uint32_t addr_a, uint32_t addr_b,
                  void *out_buf, size_t record_size,
                  uint8_t *out_slot, uint32_t *out_counter)
{
    if (record_size < 12U || record_size > 256U) return -1;

    uint8_t buf_a[256], buf_b[256];
    uint32_t cnt_a = 0, cnt_b = 0;

    int va = read_and_validate(addr_a, buf_a, record_size, &cnt_a);
    int vb = read_and_validate(addr_b, buf_b, record_size, &cnt_b);

    if (va && (!vb || cnt_a >= cnt_b)) {
        memcpy(out_buf, buf_a, record_size);
        if (out_slot)    *out_slot = 0;
        if (out_counter) *out_counter = cnt_a;
        return 0;
    }
    if (vb) {
        memcpy(out_buf, buf_b, record_size);
        if (out_slot)    *out_slot = 1;
        if (out_counter) *out_counter = cnt_b;
        return 0;
    }
    memset(out_buf, 0, record_size);
    return 1;
}

int pingpong_store(uint32_t addr_a, uint32_t addr_b,
                   void *record, size_t record_size,
                   uint8_t *out_slot, uint32_t *out_counter)
{
    if (record_size < 12U || record_size > 256U) return -1;

    uint8_t *r = (uint8_t *)record;

    uint8_t scratch[256];
    uint32_t cnt_a = 0, cnt_b = 0;
    int va = read_and_validate(addr_a, scratch, record_size, &cnt_a);
    int vb = read_and_validate(addr_b, scratch, record_size, &cnt_b);

    int      cur_slot    = -1;          /* -1 = none */
    uint32_t cur_counter = 0;
    if (va && (!vb || cnt_a >= cnt_b)) { cur_slot = 0; cur_counter = cnt_a; }
    else if (vb)                       { cur_slot = 1; cur_counter = cnt_b; }

    int target_slot = (cur_slot == 0) ? 1 : 0;   /* 0 if none → A first */
    uint32_t target_addr = (target_slot == 0) ? addr_a : addr_b;
    uint32_t other_addr  = (target_slot == 0) ? addr_b : addr_a;

    /* Stamp counter + CRC into caller's buffer. */
    uint32_t new_counter = cur_counter + 1U;
    le32_store(r + COUNTER_OFF, new_counter);
    uint32_t crc = crc32(r, record_size - 4U);
    le32_store(r + record_size - 4U, crc);

    if (w25q_erase_sector(target_addr) != 0) return -2;
    if (w25q_program(target_addr, r, record_size) != 0) return -3;

    /* Verify-read. */
    w25q_read(target_addr, scratch, record_size);
    if (memcmp(scratch, r, record_size) != 0) return -5;

    /* Erase the prior slot (no-op if it was already blank). */
    if (cur_slot != -1) {
        if (w25q_erase_sector(other_addr) != 0) return -6;
    }

    if (out_slot)    *out_slot = (uint8_t)target_slot;
    if (out_counter) *out_counter = new_counter;
    return 0;
}
