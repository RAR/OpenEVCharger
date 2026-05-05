#ifndef OPENEVCHARGER_PERSIST_CRC16_H
#define OPENEVCHARGER_PERSIST_CRC16_H

#include <stddef.h>
#include <stdint.h>

/* CRC16-CCITT-FALSE: polynomial 0x1021, init 0xFFFF, no final XOR.
 * Used for the 32-byte event_log / session_log records (CRC32 is
 * overkill at 32 bytes; CRC16 leaves 30 bytes of payload).
 * Software bit-banged; ~25 cycles/byte at 120 MHz. */
uint16_t crc16_ccitt(const void *data, size_t len);

#endif
