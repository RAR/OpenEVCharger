#include "adc_inject.h"
#include "gd32f20x.h"

static volatile uint16_t s_cp_raw = 0;
static volatile int32_t  s_cp_mv  = 0;

void adc_inject_init(void)
{
    /* ADC0 already enabled by adc_scan_init(). The injected group uses
     * separate registers so we configure it on top of the running scan. */

    adc_channel_length_config(ADC0, ADC_INSERTED_CHANNEL, 1U);
    adc_inserted_channel_config(ADC0, 0U, ADC_CHANNEL_4, ADC_SAMPLETIME_28POINT5);

    adc_external_trigger_source_config(ADC0, ADC_INSERTED_CHANNEL,
                                       ADC0_1_EXTTRIG_INSERTED_T0_TRGO);
    adc_external_trigger_config(ADC0, ADC_INSERTED_CHANNEL, ENABLE);

    adc_interrupt_enable(ADC0, ADC_INT_EOIC);

    /* NVIC priority 5 = configMAX_SYSCALL_INTERRUPT_PRIORITY. We don't
     * call any FreeRTOS API from this ISR, but staying at MAX_SYSCALL
     * keeps the option open for the future (xTaskNotifyFromISR etc.). */
    nvic_irq_enable(ADC0_1_IRQn, 5U, 0U);
}

uint16_t cp_high_raw(void) { return s_cp_raw; }
int32_t  cp_high_mv(void)  { return s_cp_mv; }

void ADC0_1_IRQHandler(void)
{
    if (RESET != adc_interrupt_flag_get(ADC0, ADC_INT_FLAG_EOIC)) {
        adc_interrupt_flag_clear(ADC0, ADC_INT_FLAG_EOIC);

        uint16_t raw = adc_inserted_data_read(ADC0, ADC_INSERTED_CHANNEL_0);
        s_cp_raw = raw;

        /* Empirical two-point calibration on the bench Rippleon ROC001:
         *   raw=0    → CP = -12 V  (PA4 saturated low when buffer at -12 V)
         *   raw=1462 → CP = +12 V  (scope-verified 2026-05-02)
         * The spec assumed the divider would map ±12 V onto the full
         * 0..3.3 V ADC range (raw 0..4095); on this PCB the swing only
         * reaches ~1.18 V at +12 V. Linear interpolation between the
         * two anchors. M5.b will replace this with a calibration record
         * stored in W25Q so other units can have their own anchors. */
        int32_t mv = (int32_t)raw * 24000 / 1462 - 12000;
        if (mv >  12000) mv =  12000;
        if (mv < -12000) mv = -12000;
        s_cp_mv = mv;
    }
}
