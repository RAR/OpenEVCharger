#ifndef OPENBHZD_HAL_UART5_H
#define OPENBHZD_HAL_UART5_H

/* "UART5" per spec § 5 / pinout doc; on GD32F2 vendor SPL this is
 * UART4 (PC12 TX, PD2 RX). 115200 8N1, no flow control.
 *
 * Bench 2026-05-03: switched RX from FreeRTOS stream buffer to a
 * tiny lockless ring buffer because xStreamBufferSendFromISR was
 * provoking a chip reset on the first inbound byte (root cause TBD).
 * comms_task polls uart5_rx_pop() with a short tick delay. */

#include <stddef.h>
#include <stdint.h>

/* Initialise UART4 + arm RX interrupt. */
void uart5_init(void);

/* Drain up to `cap` bytes from the RX ring into `out`. Returns count
 * read (0 if empty). Single-consumer (comms_task) only. */
size_t uart5_rx_pop(uint8_t *out, size_t cap);

/* Blocking write. Returns bytes sent (always == len in current
 * implementation; no timeout). Safe to call from any task; not
 * reentrant — wrap in a mutex if multiple producers. */
size_t uart5_send(const void *buf, size_t len);

#endif /* OPENBHZD_HAL_UART5_H */
