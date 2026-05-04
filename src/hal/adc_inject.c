#include "adc_inject.h"
#include "gd32f20x.h"
#include "../persist/calibration.h"

static volatile uint16_t s_cp_raw = 0;
static volatile int32_t  s_cp_mv  = 0;

void adc_inject_init(void)
{
    /* ADC0 already enabled by adc_scan_init(). The injected group uses
     * separate registers so we configure it on top of the running scan. */

    adc_channel_length_config(ADC0, ADC_INSERTED_CHANNEL, 1U);
    /* Sample time: 239.5 ADC clocks @ 10 MHz ≈ 24 µs S&H window. The
     * 28.5-cycle (~3 µs) default was sampling at the PWM rising edge
     * before CP had settled in the no-load case (bench: bare wires,
     * no plug). State C with an 882 Ω clamp settles in ~1 µs because
     * the clamp limits the rise excursion (-12V → +6V); state A or B
     * with no clamp must swing the full -12V → +12V into nothing
     * but the read divider's capacitance, which is much slower. The
     * symptom: cp_high reads ~9.9 V (= 82 % of true peak) once the
     * advertise PWM starts, and never recovers to 12 V even when
     * the wire is fully disconnected — trapping the classifier in
     * state B. 24 µs is well within the PWM period (1 ms) and lets
     * the S&H cap track the rising edge of CP all the way to peak. */
    adc_inserted_channel_config(ADC0, 0U, ADC_CHANNEL_4, ADC_SAMPLETIME_239POINT5);

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

        /* Three-point calibration anchors live in the W25Q calibration
         * record (M5.b.2). Read once per ISR; defaults match this bench
         * unit's M3.4.5 fit (anchor=1462, slope=3540/459) until the
         * record loads from flash. */
        int32_t anchor = calibration_cp_anchor_raw();
        int32_t num    = calibration_cp_slope_num();
        int32_t den    = calibration_cp_slope_den();

        int32_t mv = ((int32_t)raw - anchor) * num / den + 12000;
        if (mv >  12000) mv =  12000;
        if (mv < -12000) mv = -12000;
        s_cp_mv = mv;
    }
}
