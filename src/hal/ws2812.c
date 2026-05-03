#include "ws2812.h"
#include "../core/pin_map.h"
#include "gd32f20x.h"

/* Strip protocol family.  All families share the single-wire DIN /
 * daisy-chain DOUT topology; they differ in bit timing, byte order,
 * and (for SK6812-RGBW) bit count.  Runtime keying is via the
 * OPENBHZD_LED_PROTOCOL_* build flags; default is WS2812B. */
#ifndef OPENBHZD_LED_PROTOCOL_UCS1903
#define OPENBHZD_LED_PROTOCOL_UCS1903 0
#endif
#ifndef OPENBHZD_LED_PROTOCOL_APA106
#define OPENBHZD_LED_PROTOCOL_APA106  0
#endif

#if OPENBHZD_LED_PROTOCOL_UCS1903
/* UCS1903 / TM1804: 400 kHz, RGB byte order.
 *   T0H = 0.5 µs, T1H = 2.0 µs, period = 2.5 µs, reset ≥24 µs. */
#define WS_PERIOD_TICKS  149U   /* 2.5 µs @ 60 MHz */
#define WS_T0H_TICKS      30U   /* 0.5 µs */
#define WS_T1H_TICKS     120U   /* 2.0 µs */
#define WS_PACK_RGB        1    /* R, G, B (vs WS2812B's G, R, B) */
#elif OPENBHZD_LED_PROTOCOL_APA106
/* APA106: 800 kHz-ish, RGB, asymmetric T1H = 1.36 µs.
 *   T0H = 0.35 µs, T1H = 1.36 µs, period = 1.71 µs, reset ≥50 µs. */
#define WS_PERIOD_TICKS  102U   /* 1.71 µs @ 60 MHz */
#define WS_T0H_TICKS      21U   /* 0.35 µs */
#define WS_T1H_TICKS      82U   /* 1.36 µs */
#define WS_PACK_RGB        1
#else
/* WS2812B default: 800 kHz, GRB byte order. */
#define WS_PERIOD_TICKS   74U   /* 1.25 µs @ 60 MHz */
#define WS_T0H_TICKS      24U   /* 0.40 µs */
#define WS_T1H_TICKS      48U   /* 0.80 µs */
#define WS_PACK_RGB        0
#endif

#define WS_BITS_PER_LED   24U   /* G8 R8 B8 */
#define WS_RESET_PADDING  60U   /* 60 × 1.25 µs = 75 µs latch (≥ 50 µs spec) */

#define WS_FRAME_BITS     (OPENBHZD_WS2812_LEDS * WS_BITS_PER_LED)
#define WS_BUF_HALFWORDS  (WS_FRAME_BITS + WS_RESET_PADDING)

/* Wire byte order: GRB (WS2812B spec). The bench strip showed
 * uniform "white-ish blue" with GRB, BGR, and GBR pack orders during
 * 2026-05-03 debug — the colour was insensitive to data, suggesting
 * the issue is electrical (level mismatch, buffer in the path) not
 * byte order. Reverting to spec default. */
static uint16_t s_buf[WS_BUF_HALFWORDS];
static volatile int s_dma_busy = 0;

static void load_byte_at(uint16_t *out, uint8_t v)
{
    /* MSB-first wire order. */
    for (unsigned i = 0; i < 8; ++i) {
        out[i] = (v & 0x80u) ? WS_T1H_TICKS : WS_T0H_TICKS;
        v <<= 1;
    }
}

void ws2812_set_pixel(unsigned idx, uint8_t r, uint8_t g, uint8_t b)
{
    if (idx >= OPENBHZD_WS2812_LEDS) return;
    uint16_t *p = &s_buf[idx * WS_BITS_PER_LED];
#if WS_PACK_RGB
    load_byte_at(p +  0, r);
    load_byte_at(p +  8, g);
    load_byte_at(p + 16, b);
#else
    load_byte_at(p +  0, g);
    load_byte_at(p +  8, r);
    load_byte_at(p + 16, b);
#endif
}

void ws2812_clear(void)
{
    for (unsigned i = 0; i < OPENBHZD_WS2812_LEDS; ++i) {
        ws2812_set_pixel(i, 0, 0, 0);
    }
}

int ws2812_busy(void)
{
    return s_dma_busy;
}

/* Vendor SPL channel-clear macro names differ across firmware lib
 * versions; spell out the manipulation directly. */
static void dma_arm_channel(void)
{
    DMA_CHCTL(DMA0, DMA_CH1) &= ~DMA_CHXCTL_CHEN;            /* disable to reload */
    DMA_CHMADDR(DMA0, DMA_CH1) = (uint32_t)s_buf;
    DMA_CHCNT(DMA0, DMA_CH1)   = (uint32_t)WS_BUF_HALFWORDS;
    DMA_CHCTL(DMA0, DMA_CH1)  |= DMA_CHXCTL_CHEN;
}

void ws2812_show(void)
{
    while (s_dma_busy) { /* wait — caller should ws2812_busy() */ }

    /* Tail-pad the buffer with WS_RESET_PADDING zeros each call so a
     * dropped/partial earlier transfer can't poison this one. */
    for (unsigned i = WS_FRAME_BITS; i < WS_BUF_HALFWORDS; ++i) s_buf[i] = 0;

    s_dma_busy = 1;
    dma_arm_channel();
    timer_enable(TIMER1);
}

/* DMA0 channel 1 is TIMER1_UP per GD32F20x RM Table 67. (DMA0 = first
 * controller, vendor CH1 = 0-indexed channel 1 = "channel 2" in 1-
 * indexed RM language.) */
void DMA0_Channel1_IRQHandler(void)
{
    if (dma_interrupt_flag_get(DMA0, DMA_CH1, DMA_INT_FLAG_FTF)) {
        dma_interrupt_flag_clear(DMA0, DMA_CH1, DMA_INT_FLAG_G);
        timer_disable(TIMER1);
        DMA_CHCTL(DMA0, DMA_CH1) &= ~DMA_CHXCTL_CHEN;
        s_dma_busy = 0;
    }
}

void ws2812_init(void)
{
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_AF);
    rcu_periph_clock_enable(RCU_TIMER1);
    rcu_periph_clock_enable(RCU_DMA0);

    /* PA15 is JTDI on reset (Cortex-M3 SWJ default), unusable as GPIO
     * or AF until JTAG is disabled. SWDPENABLE keeps SWD live (for
     * our openocd probe) but releases JTAG pins (PA15, PB3, PB4) for
     * peripheral use. Required before TIMER1 partial-remap-1 routes
     * its CH0 to PA15. */
    gpio_pin_remap_config(GPIO_SWJ_SWDPENABLE_REMAP, ENABLE);
    /* Vendor SPL is off-by-one from the RM's TIM2_REMAP table:
     *   GPIO_TIMER1_PARTIAL_REMAP0 (bits=01) = RM "partial 1" → CH0=PA15
     *   GPIO_TIMER1_PARTIAL_REMAP1 (bits=10) = RM "partial 2" → CH0=PA0
     * We want CH0 on PA15 → use REMAP0. Confirmed by reading
     * AFIO_PCFR0 after init: REMAP1 = 0x...0200 (bits=10 = partial2).
     * Bench probe 2026-05-03: switching to REMAP0 lit the strip. */
    gpio_pin_remap_config(GPIO_TIMER1_PARTIAL_REMAP0, ENABLE);
    gpio_init(PIN_WS2812_PORT, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ,
              PIN_WS2812_PIN);

    /* TIMER1 at 60 MHz timer clock, period 75 ticks = 1.25 µs. */
    timer_deinit(TIMER1);

    timer_parameter_struct tp;
    timer_struct_para_init(&tp);
    tp.prescaler         = 0u;     /* 60 MHz / 1 = 60 MHz */
    tp.alignedmode       = TIMER_COUNTER_EDGE;
    tp.counterdirection  = TIMER_COUNTER_UP;
    tp.period            = WS_PERIOD_TICKS;
    tp.clockdivision     = TIMER_CKDIV_DIV1;
    tp.repetitioncounter = 0;
    timer_init(TIMER1, &tp);

    timer_oc_parameter_struct oc;
    timer_channel_output_struct_para_init(&oc);
    oc.outputstate  = TIMER_CCX_ENABLE;
    oc.outputnstate = TIMER_CCXN_DISABLE;
    /* OPENBHZD_WS2812_INVERT=1 flips the OC polarity so the line idles
     * HIGH and pulses LOW. Use this if the bench has an inverting
     * level-shifter / buffer between PA15 and the strip's DIN pad —
     * which is a strong candidate for the "uniform white-ish blue"
     * symptom: an inverter would turn every bit-period into a
     * ~0.45 µs HIGH (from T0L=0.85 µs) which the strip likely treats
     * as timing-marginal junk. Idle-LOW polarity also flips: that
     * means we still need ws2812_clear() (all zeros) to look like the
     * inactive 50 µs latch from the strip's perspective — done by the
     * tail padding which is always zero. */
#if defined(OPENBHZD_WS2812_INVERT) && OPENBHZD_WS2812_INVERT
    oc.ocpolarity   = TIMER_OC_POLARITY_LOW;
    oc.ocidlestate  = TIMER_OC_IDLE_STATE_HIGH;
#else
    oc.ocpolarity   = TIMER_OC_POLARITY_HIGH;
    oc.ocidlestate  = TIMER_OC_IDLE_STATE_LOW;
#endif
    oc.ocnpolarity  = TIMER_OCN_POLARITY_HIGH;
    oc.ocnidlestate = TIMER_OCN_IDLE_STATE_LOW;
    timer_channel_output_config(TIMER1, TIMER_CH_0, &oc);

    timer_channel_output_mode_config(TIMER1, TIMER_CH_0, TIMER_OC_MODE_PWM0);
    timer_channel_output_shadow_config(TIMER1, TIMER_CH_0,
                                       TIMER_OC_SHADOW_ENABLE);
    timer_channel_output_pulse_value_config(TIMER1, TIMER_CH_0, 0u);

    timer_auto_reload_shadow_enable(TIMER1);
    timer_dma_enable(TIMER1, TIMER_DMA_UPD);

    /* DMA0 channel 1 = TIMER1_UP. Memory → peripheral, halfword,
     * memory-incr, no circular. */
    dma_parameter_struct dp;
    dma_struct_para_init(&dp);
    dp.periph_addr  = (uint32_t)&TIMER_CH0CV(TIMER1);
    dp.periph_inc   = DMA_PERIPH_INCREASE_DISABLE;
    dp.periph_width = DMA_PERIPHERAL_WIDTH_16BIT;
    dp.memory_addr  = (uint32_t)s_buf;
    dp.memory_inc   = DMA_MEMORY_INCREASE_ENABLE;
    dp.memory_width = DMA_MEMORY_WIDTH_16BIT;
    dp.direction    = DMA_MEMORY_TO_PERIPHERAL;
    dp.number       = WS_BUF_HALFWORDS;
    dp.priority     = DMA_PRIORITY_HIGH;
    dma_deinit(DMA0, DMA_CH1);
    dma_init(DMA0, DMA_CH1, &dp);
    dma_circulation_disable(DMA0, DMA_CH1);
    dma_memory_to_memory_disable(DMA0, DMA_CH1);
    dma_interrupt_enable(DMA0, DMA_CH1, DMA_INT_FTF);
    nvic_irq_enable(DMA0_Channel1_IRQn, 6U, 0U);

    /* Start with a black frame to silence any garbage on the line. */
    ws2812_clear();
    timer_primary_output_config(TIMER1, ENABLE);
    /* timer_enable() happens in ws2812_show() so we don't drive PA15
     * while the buffer is half-loaded. */
}
