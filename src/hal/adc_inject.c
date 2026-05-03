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

        /* Empirical three-point calibration on the bench Rippleon ROC001
         * (scope-verified 2026-05-02):
         *   raw=0    → CP saturated at <= ~0 V (negative rail saturates)
         *   raw=1003 → CP = +8.46 V  (2.2 kΩ across CP↔PE, state B)
         *   raw=1462 → CP = +12 V    (open, state A)
         * Slope in the linear region: (12000 - 8460) / (1462 - 1003)
         * = 3540/459 ≈ 7.71 mV/raw. Anchor at +12 V.
         *
         * The negative rail saturates the read-back at raw=0, so any CP
         * below ~0 V reads as cp_mv = +728 mV (and clamps to 0/state-E
         * after the band map). That's acceptable because state F is
         * firmware-driven via CCR=0; we don't need the read-back to
         * tell us we're in state F.
         *
         * M5.b will move these anchors into the W25Q calibration record
         * so per-unit calibration becomes a runtime concept. */
        int32_t mv = ((int32_t)raw - 1462) * 3540 / 459 + 12000;
        if (mv >  12000) mv =  12000;
        if (mv < -12000) mv = -12000;
        s_cp_mv = mv;
    }
}
