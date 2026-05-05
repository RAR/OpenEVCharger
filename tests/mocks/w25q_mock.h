#ifndef OPENEVCHARGER_TESTS_W25Q_MOCK_H
#define OPENEVCHARGER_TESTS_W25Q_MOCK_H

/* Host-side mock implementation of hal/w25q.h.
 *
 * Backed by an 8 MB simulated flash array initialised to 0xFF. Models
 * the NOR semantics that pingpong / boot_config rely on:
 *   - read  → straight memcpy
 *   - program → AND-into (bit-clear-only); errors on oversized or
 *     page-crossing writes.
 *   - erase_sector → memset 0xFF over the 4 KB sector containing addr.
 *
 * Exposes a few test-only helpers for setting up scenarios. */

#include <stddef.h>
#include <stdint.h>

#define W25Q_MOCK_FLASH_SIZE   (8U * 1024U * 1024U)

#ifdef __cplusplus
extern "C" {
#endif

/* Resets the simulated flash to all-0xFF and clears the JEDEC-init flag
 * so the next w25q_init() returns 0 cleanly. Call between cases. */
void w25q_mock_reset(void);

/* Direct test-only access for poking specific bytes — used to emulate
 * single-bit corruption (e.g. flipping a CRC byte). The mock honours
 * NOR semantics (1→0 only) when invoked with `nor_semantics=1`; pass 0
 * to write any byte regardless (e.g. to "un-erase" a sector for
 * bench-style fault injection). */
void w25q_mock_poke(uint32_t addr, uint8_t value, int nor_semantics);

/* Read a byte directly without going through w25q_read(). Useful in
 * assertions to verify what's on flash. */
uint8_t w25q_mock_peek(uint32_t addr);

/* Returns 1 if the given sector (4 KB) is fully blank (all 0xFF), 0
 * otherwise. */
int w25q_mock_sector_is_blank(uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif
