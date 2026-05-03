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
 * LED count is fixed at compile time. The bench has 4–8 LEDs per
 * spec § 7; default 8 covers either. Override via
 *   cmake -DOPENBHZD_WS2812_LEDS=N
 *
 * Caller fills pixels with ws2812_set_pixel(); ws2812_show() kicks
 * DMA + returns immediately. The frame finishes ~ N * 30 µs + 60 µs
 * later. Don't call ws2812_show() faster than ~30 Hz; pre-checks
 * the previous DMA via ws2812_busy(). */

#ifndef OPENBHZD_WS2812_LEDS
#define OPENBHZD_WS2812_LEDS  8U
#endif

void ws2812_init(void);
int  ws2812_busy(void);
void ws2812_set_pixel(unsigned idx, uint8_t r, uint8_t g, uint8_t b);
void ws2812_clear(void);
void ws2812_show(void);

#endif /* OPENBHZD_HAL_WS2812_H */
