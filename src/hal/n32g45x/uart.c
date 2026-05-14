/* boards/nexcyber/hal/uart.c — Minimal UART bring-up + printk for M0.
 *
 * USART1 on PA9 (TX) / PA10 (RX), 115200 8N1. These are the same pads
 * the Tuya WBR2 Wi-Fi module sits on (see boards/nexcyber/pin_map.h).
 * For bench bring-up, jumper a USB-UART probe to PA9 with the WBR2
 * module offline — or scope the line and let our printk traffic
 * interleave with the WBR2's TLV bursts (they're easy to tell apart
 * by framing).
 *
 * No FreeRTOS dependency in this file — M0 runs before the scheduler.
 * The timestamp prefix + format extensions in src/hal/uart.c (rippleon)
 * get pulled in alongside the M1 FreeRTOS scaffold; this is just the
 * bare-metal subset needed to confirm the chip is alive.
 *
 * Matches the interface in src/hal/uart.h (uart_init / uart_write /
 * printk) so the board-independent code paths can call them without
 * #ifdefs once the rest of the HAL ports.
 */

#include "hal/uart.h"
#include "n32g45x.h"
#include <stdarg.h>

static volatile uint8_t s_uart_ready = 0;

void uart_init(void)
{
    /* GPIOA + USART1 + AFIO need their bus clocks enabled before we
     * touch their registers. The Nations SPL uses APB2 for all of
     * GPIOA/B/C/D/E/F/G + AFIO + USART1 + TIM1; APB1 holds the
     * remaining peripherals. */
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_AFIO
                            | RCC_APB2_PERIPH_GPIOA
                            | RCC_APB2_PERIPH_USART1,
                            ENABLE);

    /* PA9 = USART1 TX → AF push-pull. PA10 = USART1 RX → floating
     * input. STM32F1-family pinmux model — no per-pin AF register,
     * the peripheral wins the pad as soon as it's enabled and the
     * GPIO is in the matching AF mode. */
    GPIO_InitType io = {0};
    io.Pin        = GPIO_PIN_9;
    io.GPIO_Mode  = GPIO_Mode_AF_PP;
    io.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitPeripheral(GPIOA, &io);

    io.Pin        = GPIO_PIN_10;
    io.GPIO_Mode  = GPIO_Mode_IN_FLOATING;
    io.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitPeripheral(GPIOA, &io);

    USART_InitType u = {0};
    u.BaudRate            = 115200;
    u.WordLength          = USART_WL_8B;
    u.StopBits            = USART_STPB_1;
    u.Parity              = USART_PE_NO;
    u.Mode                = USART_MODE_RX | USART_MODE_TX;
    u.HardwareFlowControl = USART_HFCTRL_NONE;
    USART_Init(USART1, &u);
    USART_Enable(USART1, ENABLE);

    s_uart_ready = 1;
}

static void uart_putc(char c)
{
    /* Wait for TXDE (transmit data empty) before stuffing a new byte
     * into the DR. Idle case completes in microseconds at 115200. */
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXDE) == RESET) { }
    USART_SendData(USART1, (uint8_t)c);
}

size_t uart_write(const void *buf, size_t len)
{
    if (!s_uart_ready) return len;
    const char *p = (const char *)buf;
    for (size_t i = 0; i < len; ++i) {
        if (p[i] == '\n') uart_putc('\r');
        uart_putc(p[i]);
    }
    return len;
}

/* Tiny printf — same %s/%d/%u/%x/%c/%% subset as src/hal/uart.c's
 * printk on the rippleon target, but without the timestamp prefix
 * (no FreeRTOS or system_time available in M0). */
static char *itoa_u(uint32_t v, char *buf, int base, int width, char pad)
{
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) {
        uint32_t d = v % (uint32_t)base;
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
        v /= (uint32_t)base;
    }
    while (n < width) tmp[n++] = pad;
    while (n--) *buf++ = tmp[n];
    return buf;
}

int printk(const char *fmt, ...)
{
    if (!s_uart_ready) return 0;
    char out[160];
    char *o = out;
    char *end = out + sizeof(out) - 1;
    va_list ap;
    va_start(ap, fmt);
    while (*fmt && o < end) {
        if (*fmt != '%') { *o++ = *fmt++; continue; }
        ++fmt;
        int width = 0;
        char pad = ' ';
        if (*fmt == '0') { pad = '0'; ++fmt; }
        while (*fmt >= '0' && *fmt <= '9') { width = width*10 + (*fmt - '0'); ++fmt; }
        switch (*fmt) {
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) { *o++ = '-'; v = -v; }
            o = itoa_u((uint32_t)v, o, 10, width, pad);
            break;
        }
        case 'u': o = itoa_u(va_arg(ap, unsigned), o, 10, width, pad); break;
        case 'x': o = itoa_u(va_arg(ap, unsigned), o, 16, width, pad); break;
        case 'c': *o++ = (char)va_arg(ap, int); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && o < end) *o++ = *s++;
            break;
        }
        case '%': *o++ = '%'; break;
        default:  *o++ = '%'; if (o < end) *o++ = *fmt; break;
        }
        ++fmt;
    }
    va_end(ap);
    return (int)uart_write(out, (size_t)(o - out));
}
