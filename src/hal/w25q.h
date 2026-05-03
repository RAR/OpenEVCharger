#ifndef OPENBHZD_HAL_W25Q_H
#define OPENBHZD_HAL_W25Q_H

#include <stddef.h>
#include <stdint.h>

#define W25Q_PAGE_SIZE       256U
#define W25Q_SECTOR_SIZE    4096U

/* Initialise W25Q. Reads + caches JEDEC ID. SPI3 must already be up.
 * Returns 0 on success, -1 if JEDEC reads as all-zero or all-one. */
int w25q_init(void);

/* JEDEC ID as { mfr<<16 | type<<8 | capacity }. 0 if init failed.
 * W25Q64JV = 0x00EF4017. */
uint32_t w25q_jedec_id(void);

/* Read `len` bytes starting at flash address `addr`. */
void w25q_read(uint32_t addr, void *buf, size_t len);

/* Erase the 4 KB sector containing `addr`. Blocks until done.
 * Returns 0 on success, -1 on timeout. */
int w25q_erase_sector(uint32_t addr);

/* Program up to 256 bytes at `addr`. Caller MUST not cross a 256-byte
 * page boundary. Blocks until done. */
int w25q_program(uint32_t addr, const void *buf, size_t len);

#endif
