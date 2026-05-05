#ifndef OPENEVCHARGER_HAL_WS2812_H
#define OPENEVCHARGER_HAL_WS2812_H

#include <stddef.h>
#include <stdint.h>

/* Single-wire addressable LED strip driver on PA15 = TIMER1_CH0
 * (partial-remap-1).  TIMER1_UP triggers DMA0_CH1 which copies one
 * halfword per timer-update event into TIMER1_CH0_CCR (u16 transfer
 * width).
 *
 * Default protocol: SK6812 RGBW (32 bits per LED, GRBW byte order,
 * T0H = 0.30 µs, T1H = 0.60 µs, period = 1.25 µs).  This is what the
 * bench unit's strip actually uses — bench-confirmed 2026-05-03 by
 * scoping stock firmware (4.4 ms burst length = 110 LEDs × 32 bits ×
 * 1.25 µs).
 *
 * Override the protocol with:
 *   cmake -DOPENEVCHARGER_LED_PROTOCOL_WS2812B=1   (24-bit GRB, T0H=0.4 / T1H=0.8)
 *   cmake -DOPENEVCHARGER_LED_PROTOCOL_UCS1903=1   (24-bit RGB, 400 kHz)
 *   cmake -DOPENEVCHARGER_LED_PROTOCOL_APA106=1    (24-bit RGB, asym timing)
 *
 * Override the LED count with:
 *   cmake -DOPENEVCHARGER_WS2812_LEDS=N
 *
 * Caller fills pixels with ws2812_set_pixel(); ws2812_show() kicks
 * DMA + returns immediately.  Frame finishes ~ N × bits_per_led ×
 * 1.25 µs + 75 µs latch later (≈ 4.5 ms for 110 SK6812 RGBW LEDs).
 * Don't call ws2812_show() faster than ~30 Hz; pre-check the previous
 * DMA via ws2812_busy(). */

#ifndef OPENEVCHARGER_WS2812_LEDS
#define OPENEVCHARGER_WS2812_LEDS  134U
#endif

void ws2812_init(void);
int  ws2812_busy(void);
void ws2812_set_pixel(unsigned idx, uint8_t r, uint8_t g, uint8_t b);
void ws2812_clear(void);
void ws2812_show(void);

#endif /* OPENEVCHARGER_HAL_WS2812_H */
