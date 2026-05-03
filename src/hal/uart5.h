#ifndef OPENBHZD_HAL_UART5_H
#define OPENBHZD_HAL_UART5_H

/* "UART5" per spec § 5 / pinout doc; on GD32F2 vendor SPL this is
 * UART4 (PC12 TX, PD2 RX). 115200 8N1, no flow control. RX bytes
 * are pushed by ISR into a FreeRTOS stream buffer that comms_task
 * drains. TX is blocking (poll-on-TBE) — fine at 115200 (~87 µs/byte
 * × max 64 bytes/frame ≈ 5.6 ms; below safety_task's 20 ms tick). */

#include <stddef.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "stream_buffer.h"

/* Initialise UART4 + bind a stream buffer for RX. The caller owns
 * the stream buffer (created via xStreamBufferCreate); the ISR pushes
 * incoming bytes into it. Pass a buffer of e.g. 256 bytes — well above
 * one max-size TLV frame (64 bytes) so back-pressure is unlikely. */
void uart5_init(StreamBufferHandle_t rx);

/* Blocking write. Returns bytes sent (always == len in current
 * implementation; no timeout). Safe to call from any task; not
 * reentrant — wrap in a mutex if multiple producers. */
size_t uart5_send(const void *buf, size_t len);

#endif /* OPENBHZD_HAL_UART5_H */
