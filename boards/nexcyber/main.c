/* boards/nexcyber/main.c — M0 bring-up entry point.
 *
 * Brings the clock + log UART up and emits a heartbeat. No FreeRTOS,
 * no tasks, no peripherals beyond the log UART. The point is to get
 * a "chip is alive at the right rate" trace on the first flash so
 * any subsequent regression after a HAL change is obvious.
 *
 * M1 will switch to the shared src/main.c with the FreeRTOS
 * scheduler once enough of src/hal/, src/core/, and src/persist/
 * compile under the Nations SPL. Today (M0) those files are
 * board-specific to rippleon (they #include "gd32f20x.h" directly)
 * so they're excluded from the nexcyber build at the CMake level.
 *
 * See boards/nexcyber/README.md for the porting milestone roadmap.
 */

#include "hal/clock.h"
#include "hal/uart.h"

/* Newlib's __libc_init_array references _init/_fini; we have no C++
 * static ctors so empty stubs are fine. Same idiom as src/main.c on
 * the rippleon target. */
void _init(void) {}
void _fini(void) {}

static void busy_delay(volatile uint32_t cycles)
{
    while (cycles--) { __asm__ volatile ("nop"); }
}

int main(void)
{
    clock_real_120m_init();
    uart_init();
    clock_log_status();
    printk("openevcharger nexcyber: M0 bring-up\n");

    uint32_t beat = 0;
    while (1) {
        printk("beat %u\n", (unsigned)beat++);
        /* ~1 s at 144 MHz w/ a 4-cycle nop loop. Tuned by feel —
         * tight enough to feel alive on a serial console, loose
         * enough that the UART has time to drain. */
        busy_delay(36000000u);
    }
}
