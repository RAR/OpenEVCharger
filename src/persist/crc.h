#ifndef OPENEVCHARGER_PERSIST_CRC_H
#define OPENEVCHARGER_PERSIST_CRC_H

#include <stddef.h>
#include <stdint.h>

/* IEEE 802.3 CRC32 (polynomial 0xEDB88320, reflected). Standard
 * crc32 used by zlib / Ethernet / W25Q config records. Initial
 * value 0xFFFFFFFF, final XOR 0xFFFFFFFF. Software-only — no
 * peripheral CRC unit. ~50 cycles/byte. */
uint32_t crc32(const void *data, size_t len);

#endif
