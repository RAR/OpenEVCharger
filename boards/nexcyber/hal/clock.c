/* boards/nexcyber/hal/clock.c — Nations N32G45x clock bring-up (M0).
 *
 * SDK's startup_n32g45x_gcc.S calls SystemInit() from
 * third_party/N32G45x_Firmware_Library/cmsis/variants/n32g45x/system_n32g45x.c
 * before main() runs. With the SDK defaults (HSE_VALUE=8 MHz,
 * SYSCLK_SRC=HSE_PLL, SYSCLK_FREQ=144 MHz) this brings the chip up to
 * 144 MHz from the 8 MHz HXTAL via PLL ×18 and leaves SystemCoreClock
 * populated.
 *
 * For M0 we accept the SDK default and only log the active rate. A
 * later milestone reworks this to 120 MHz to match the rippleon
 * target (consistent timer / FreeRTOS-tick math) once the HXTAL
 * frequency is confirmed at the bench.
 *
 * Matches the interface in src/hal/clock.h so eventually a shared
 * src/main.c can call clock_real_120m_init() / clock_log_status()
 * regardless of which board it's targeting.
 */

#include "hal/clock.h"
#include "hal/uart.h"
#include "n32g45x.h"

extern uint32_t SystemCoreClock;

static int s_clock_init_status = -1;

void clock_real_120m_init(void)
{
    /* SDK SystemInit() already ran from the reset handler. M0
     * accepts the SDK-selected rate (144 MHz on the default
     * HSE_PLL chain) and just marks status as OK.
     *
     * A future milestone will reconfigure to 120 MHz here once
     * the HXTAL value is bench-confirmed (it is 8 MHz per the
     * SDK default, but the Nexcyber PCB hasn't been cross-
     * checked against a scope yet). */
    s_clock_init_status = 0;
}

void clock_log_status(void)
{
    if (s_clock_init_status == 0) {
        printk("clock: SDK default %u Hz (M0 bring-up)\n",
               (unsigned)SystemCoreClock);
    } else {
        printk("clock: status=%d\n", s_clock_init_status);
    }
}
