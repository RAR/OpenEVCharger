#ifndef OPENEVCHARGER_PERSIST_PINGPONG_H
#define OPENEVCHARGER_PERSIST_PINGPONG_H

#include <stddef.h>
#include <stdint.h>

/* Two-slot ping-pong over W25Q. Each "logical record" lives in one of
 * two 4 KB sectors. At rest exactly one slot is CRC-valid; the other is
 * erased.
 *
 * Record convention (enforced by this helper, no offsets passed in):
 *   bytes [0]              = version (caller-managed)
 *   bytes [1..3]           = pad
 *   bytes [4..7]           = u32 monotonic_counter (managed by helper)
 *   bytes [8..size-5]      = caller payload
 *   bytes [size-4..size-1] = u32 crc32 (covers bytes 0..size-5)
 *
 * record_size MUST be in [12, 256] (one W25Q page). All records in this
 * codebase are 32 B. */

/* Read the newer-valid slot's bytes into out_buf.
 *   0  → out_buf populated, *out_slot = 0 (A) or 1 (B)
 *   1  → both slots invalid; out_buf zeroed
 *  <0  → argument error */
int pingpong_load(uint32_t addr_a, uint32_t addr_b,
                  void *out_buf, size_t record_size,
                  uint8_t *out_slot, uint32_t *out_counter);

/* Erase the *other* slot (relative to current valid one), program the
 * supplied record there, verify-read, then erase the prior slot. The
 * helper updates the counter (= prior + 1, or 1 on first write) and
 * CRC inline; caller leaves those bytes uninitialised.
 *
 *   0  → success, *out_slot = slot just written, *out_counter = its value
 *  <0  → W25Q error or verify mismatch */
int pingpong_store(uint32_t addr_a, uint32_t addr_b,
                   void *record, size_t record_size,
                   uint8_t *out_slot, uint32_t *out_counter);

#endif
