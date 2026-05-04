/* Tiny host-side stubs for firmware-only HAL symbols that some
 * persist modules pull in via printk(). Tests don't care about the
 * output; we just need the link to resolve.
 *
 * Override at runtime by setting g_host_stubs_capture = 1; the most
 * recent printk() call is then captured into g_host_stubs_last_msg
 * (truncated to 255 chars). Unused for now but cheap to keep around. */

#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>

int  g_host_stubs_capture = 0;
char g_host_stubs_last_msg[256];

int printk(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = 0;
    if (g_host_stubs_capture) {
        n = vsnprintf(g_host_stubs_last_msg, sizeof g_host_stubs_last_msg,
                      fmt, ap);
    } else {
        /* Drop on the floor — host tests don't read firmware logs. */
        char sink[256];
        n = vsnprintf(sink, sizeof sink, fmt, ap);
    }
    va_end(ap);
    return n;
}

void uart_init(void) {}

size_t uart_write(const void *buf, size_t len)
{
    (void)buf; (void)len;
    return len;
}
