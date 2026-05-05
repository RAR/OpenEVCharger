#ifndef OPENEVCHARGER_HAL_UART_H
#define OPENEVCHARGER_HAL_UART_H

#include <stddef.h>
#include <stdint.h>

/* Initialise USART1 at 115200 8N1 on PA9/PA10. Idempotent: safe to call
 * before the scheduler starts. After this returns, printk() is usable. */
void uart_init(void);

/* Synchronous, busy-wait TX. Safe to call from ISR or task context. Drops
 * the call cleanly (returns 0) if uart_init() hasn't run yet. Returns the
 * number of bytes written (always == len when uart is up). */
size_t uart_write(const void *buf, size_t len);

/* printf-shaped console. Format spec is a small subset:
 *   %s, %d, %u, %x, %02x, %08x, %c, %% — enough for diagnostic output.
 * Writes synchronously; never blocks on the kernel.
 * Returns bytes actually emitted. */
int printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
