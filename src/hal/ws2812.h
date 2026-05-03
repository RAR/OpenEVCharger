#ifndef OPENBHZD_HAL_WS2812_H
#define OPENBHZD_HAL_WS2812_H

#include <stddef.h>
#include <stdint.h>

/* WS2812B NRZ driver on PA15 = TIMER1_CH0 (partial-remap-1).
 *
 * Timer clock = 60 MHz (APB1 timer ×2). Period = 75 ticks = 1.25 µs.
 * "0" bit: CCR=24 (T0H=0.4 µs). "1" bit: CCR=48 (T1H=0.8 µs).
 * Reset frame: 50+ trailing zero-CCR ticks (≥ 50 µs latch).
 *
 * DMA1 channel (TIMER1_UP) copies one halfword per timer-update event
 * into TIMER1_CH0_CCR. Buffer is u16 to keep transfer width consistent.
 *
 * LED count is fixed at compile time. Bench-confirmed 2026-05-03:
 * the on-board strip has 134 LEDs (binary-narrowed from N=8 → 60 →
 * 144 → 134 by lighting the strip and counting the dark tail).
 * Override via:
 *   cmake -DOPENBHZD_WS2812_LEDS=N
 *
 * Caller fills pixels with ws2812_set_pixel(); ws2812_show() kicks
 * DMA + returns immediately. The frame finishes ~ N * 30 µs + 75 µs
 * later (≈ 4.1 ms for 134 LEDs). Don't call ws2812_show() faster
 * than ~30 Hz; pre-checks the previous DMA via ws2812_busy(). */

#ifndef OPENBHZD_WS2812_LEDS
#define OPENBHZD_WS2812_LEDS  134U
#endif

void ws2812_init(void);
int  ws2812_busy(void);
void ws2812_set_pixel(unsigned idx, uint8_t r, uint8_t g, uint8_t b);
void ws2812_clear(void);
void ws2812_show(void);

#endif /* OPENBHZD_HAL_WS2812_H */
