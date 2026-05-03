#ifndef OPENBHZD_HAL_CLOCK_H
#define OPENBHZD_HAL_CLOCK_H

/* Replace the SDK's broken `system_clock_120m_hxtal` PLL chain with a
 * direct HXTAL → PREDV0÷2 → PLL×30 → 120 MHz config. The vendor
 * chain produces ~38.4 MHz instead of the advertised 120 MHz (see
 * feedback memory `feedback_gd32f20x_120m_hxtal_sdk_bug`). Switching
 * to a clean chain lets timer-derived bit timing (ws2812 in
 * particular) drop its ARR/CCR scaling kludge.
 *
 * Call from main() immediately after SystemInit. Updates the global
 * `SystemCoreClock` variable so any SPL peripheral init done after
 * this point computes baud / prescalers from the real 120 MHz value.
 *
 * Gated on build flag OPENBHZD_REAL_120M_PLL — when 0, this is a
 * no-op (legacy SDK behavior preserved). */
void clock_real_120m_init(void);

/* Print the status captured by clock_real_120m_init. Call after
 * uart_init so the boot trace shows "clock: switched to 120 MHz" or
 * a failure stage. No-op when OPENBHZD_REAL_120M_PLL=0. */
void clock_log_status(void);

#endif
