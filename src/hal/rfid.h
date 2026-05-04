#ifndef OPENBHZD_HAL_RFID_H
#define OPENBHZD_HAL_RFID_H

#include <stddef.h>
#include <stdint.h>

/* RFID/NFC reader port — USART1 remap PD5 (TX) / PD6 (RX), 115200 8N1
 * full-duplex. Bench-decoded protocol (see docs/mcu-re/rfid-protocol.md):
 *
 *   MCU  → module: AA D0 D1 01 00 00 4C   (7 B fixed keepalive, ~3 Hz)
 *   module → MCU:  AA D1 D0 02 LL 00 [UID*LL] STATE
 *                  └ idle (LL=0): 7 B, STATE=0x4C
 *                  └ card  (LL=4): 11 B, STATE varies (e.g. 0x98)
 *
 * The module is silent without our keepalive — this is request/response
 * with mirrored src/dst pairs. */

void   rfid_init(void);
size_t rfid_rx_pop(uint8_t *out, size_t cap);
void   rfid_send_keepalive(void);

#endif
