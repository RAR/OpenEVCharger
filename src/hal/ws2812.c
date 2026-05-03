#include "ws2812.h"
#include "../core/pin_map.h"
#include "gd32f20x.h"

#define WS_PERIOD_TICKS   74U   /* ARR — 75 timer ticks total = 1.25 µs */
#define WS_T0H_TICKS      24U   /* T0H = 0.40 µs */
#define WS_T1H_TICKS      48U   /* T1H = 0.80 µs */

#define WS_BITS_PER_LED   24U   /* G8 R8 B8 */
#define WS_RESET_PADDING  60U   /* 60 × 1.25 µs = 75 µs latch (spec ≥ 50 µs) */

#define WS_FRAME_BITS     (OPENBHZD_WS2812_LEDS * WS_BITS_PER_LED)
#define WS_BUF_HALFWORDS  (WS_FRAME_BITS + WS_RESET_PADDING)

/* GRB order is what the WS2812 wire protocol uses. We expose RGB to
 * callers and pack into GRB on set_pixel. */
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
    load_byte_at(p +  0, g);
    load_byte_at(p +  8, r);
    load_byte_at(p + 16, b);
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
    gpio_pin_remap_config(GPIO_TIMER1_PARTIAL_REMAP1, ENABLE);
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
    oc.ocpolarity   = TIMER_OC_POLARITY_HIGH;
    oc.ocnpolarity  = TIMER_OCN_POLARITY_HIGH;
    oc.ocidlestate  = TIMER_OC_IDLE_STATE_LOW;
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
