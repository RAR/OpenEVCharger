#include "uart.h"
#include "core/pin_map.h"
#include "gd32f20x.h"
#include <stdarg.h>

static volatile uint8_t s_uart_ready = 0;

/* ARM semihosting tee — when an OpenOCD/gdb debugger is attached and has
 * `arm semihosting enable`d, every uart_write() call also surfaces in the
 * debugger console. Lets us see printk output without a physical UART
 * probe.
 *
 * IMPORTANT: BKPT 0xAB halts the chip forever if no host is actively
 * servicing it. The DHCSR.C_DEBUGEN gate is unreliable: openocd does NOT
 * clear that bit on `shutdown`, so it stays set across openocd
 * disconnects and a subsequent printk freezes the MCU. Behavior observed
 * after multiple monitor cycles 2026-05-03: heartbeat LED stops because
 * io_task gets stuck in uart_write() during its first printk after
 * openocd exited.
 *
 * Therefore default OFF. Enable for monitor-attached bench sessions
 * with `cmake -DOPENBHZD_SEMIHOSTING=1`. */
#ifndef OPENBHZD_SEMIHOSTING
#define OPENBHZD_SEMIHOSTING 0
#endif

#define DHCSR_ADDR    0xE000EDF0u
#define DHCSR_DEBUGEN 0x00000001u

static int debugger_attached(void)
{
    return (*(volatile uint32_t *)DHCSR_ADDR) & DHCSR_DEBUGEN;
}

static void semihost_write(const void *buf, size_t len)
{
#if OPENBHZD_SEMIHOSTING
    if (!debugger_attached() || len == 0) return;
    struct { int handle; const char *buf; int len; } args =
        { 2 /* stderr */, (const char *)buf, (int)len };
    register int r0 __asm__("r0") = 0x05;       /* SYS_WRITE */
    register const void *r1 __asm__("r1") = &args;
    __asm volatile ("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
#else
    (void)buf;
    (void)len;
    (void)debugger_attached;
#endif
}

void uart_init(void)
{
    rcu_periph_clock_enable(PIN_LOG_UART_RCU);
    rcu_periph_clock_enable(RCU_AF);
    rcu_periph_clock_enable(RCU_UART3);

    /* UART3 default pins (PC10 TX / PC11 RX). No remap needed. */
    gpio_init(PIN_LOG_UART_TX_PORT, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ,
              PIN_LOG_UART_TX_PIN);
    gpio_init(PIN_LOG_UART_RX_PORT, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ,
              PIN_LOG_UART_RX_PIN);

    usart_deinit(UART3);
    usart_baudrate_set(UART3, 115200U);
    usart_word_length_set(UART3, USART_WL_8BIT);
    usart_stop_bit_set(UART3, USART_STB_1BIT);
    usart_parity_config(UART3, USART_PM_NONE);
    usart_hardware_flow_rts_config(UART3, USART_RTS_DISABLE);
    usart_hardware_flow_cts_config(UART3, USART_CTS_DISABLE);
    usart_receive_config(UART3, USART_RECEIVE_ENABLE);
    usart_transmit_config(UART3, USART_TRANSMIT_ENABLE);
    usart_enable(UART3);

    s_uart_ready = 1;
}

static void uart_putc(char c)
{
    while (RESET == usart_flag_get(UART3, USART_FLAG_TBE)) { }
    usart_data_transmit(UART3, (uint8_t)c);
}

size_t uart_write(const void *buf, size_t len)
{
    semihost_write(buf, len);
    if (!s_uart_ready) return len;
    const char *p = (const char *)buf;
    for (size_t i = 0; i < len; ++i) {
        if (p[i] == '\n') uart_putc('\r');
        uart_putc(p[i]);
    }
    return len;
}

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
        int width = 0; char pad = ' ';
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
