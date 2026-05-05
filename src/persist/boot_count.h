#ifndef OPENEVCHARGER_PERSIST_BOOT_COUNT_H
#define OPENEVCHARGER_PERSIST_BOOT_COUNT_H

#include <stdint.h>

/* Read the current boot_count from W25Q, increment, write back, return
 * the new (post-increment) value. On invalid/blank read, returns 1.
 * On W25Q error (erase or program timeout), returns 0xFFFFFFFF.
 *
 * Called once from main() before tasks start. */
uint32_t boot_count_increment(void);

#endif
