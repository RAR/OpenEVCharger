#ifndef OPENEVCHARGER_BOARDS_NEXCYBER_HAL_NEXTION_H
#define OPENEVCHARGER_BOARDS_NEXCYBER_HAL_NEXTION_H

#include <stddef.h>
#include <stdint.h>

/* M3 Nextion HMI link — raw transport only.
 *
 * USART2 (PA2 TX / PA3 RX) at 9600 8N1 to match the stock-fw default.
 * Speed can be bumped to 115200 later by sending "bauds=115200" +
 * terminator and re-initing the MCU side. DMA1 channel 6 streams
 * RX bytes into a 128-byte circular buffer (mirrors what stock fw
 * does — see project_nexcyber_dma_peripheral_map memory for layout).
 *
 * Protocol summary (Nextion docs):
 *   - Every command sent to the display ends with 3 × 0xFF.
 *   - Touch events come back as 0x65 + page + element + press +
 *     0xFF 0xFF 0xFF.
 *   - String / numeric replies are 0x70 (string) or 0x71 (int u32 LE)
 *     followed by data + 0xFF 0xFF 0xFF.
 *
 * This file owns the wire — init, TX, raw RX. The protocol parser
 * (frame splitter, event dispatch) lives in a higher layer that
 * hasn't been written yet (M5+). For now callers can read raw bytes
 * via nextion_rx_drain() and decode inline.
 *
 * No FreeRTOS dependency. TX is blocking polled; RX is DMA-fed so
 * the CPU only touches it when nextion_rx_drain() runs.
 */

void nextion_init(void);

/* Send a command — caller-provided text + 3 × 0xFF terminator. Common
 * commands seen in stock firmware: "page setting", "page nogun",
 * "page chargeing", "page waittime", and element-level updates of
 * the form "t0.txt=\"...\"" or "t0.pic=N". Blocking TX. */
void nextion_send_cmd(const char *cmd);

/* Drain pending RX bytes into out[]. Returns the number of bytes
 * copied (0..max). Non-blocking. Caller is responsible for framing
 * (look for the 0xFF 0xFF 0xFF terminator). */
size_t nextion_rx_drain(uint8_t *out, size_t max);

#endif
