#include "adc_inject.h"
#include "gd32f20x.h"
#include "../persist/calibration.h"

static volatile uint16_t s_cp_high_raw = 0;
static volatile int32_t  s_cp_high_mv  = 0;
static volatile uint16_t s_cp_low_raw  = 0;
static volatile int32_t  s_cp_low_mv   = 0;

static int32_t convert_mv(uint16_t raw)
{
    /* Three-point calibration anchors live in the W25Q calibration
     * record (M5.b.2). Read once per ISR; defaults match this bench
     * unit's M3.4.5 fit (anchor=1462, slope=3540/459) until the
     * record loads from flash. The fit was solved against the
     * positive-half CP swing only — applying it to a negative-half
     * sample (cp_low when the diode pulls CP to -12 V) gives a
     * meaningless mV figure. cp_low_raw is the diagnostic-grade
     * value until the 5-point fit lands. */
    int32_t anchor = calibration_cp_anchor_raw();
    int32_t num    = calibration_cp_slope_num();
    int32_t den    = calibration_cp_slope_den();

    int32_t mv = ((int32_t)raw - anchor) * num / den + 12000;
    if (mv >  12000) mv =  12000;
    if (mv < -12000) mv = -12000;
    return mv;
}

void adc_inject_init(void)
{
    /* ADC0 already enabled by adc_scan_init() — its injected group
     * captures cp_high every period via TIMER0 update event (TRGO).
     * Sample time 239.5 ADC clocks @ 10 MHz ≈ 24 µs S&H window —
     * matters because the read divider's RC response is slow on the
     * unloaded (state A/B) CP wire and a short S&H starves the cap
     * on the rising edge, capping cp_high at ~9.9 V. 24 µs is well
     * within the 1 ms PWM period. */
    adc_channel_length_config(ADC0, ADC_INSERTED_CHANNEL, 1U);
    adc_inserted_channel_config(ADC0, 0U, ADC_CHANNEL_4, ADC_SAMPLETIME_239POINT5);
    adc_external_trigger_source_config(ADC0, ADC_INSERTED_CHANNEL,
                                       ADC0_1_EXTTRIG_INSERTED_T0_TRGO);
    adc_external_trigger_config(ADC0, ADC_INSERTED_CHANNEL, ENABLE);
    adc_interrupt_enable(ADC0, ADC_INT_EOIC);

    /* ADC1 dedicated to cp_low. Triggered by TIMER0 CH3 compare event
     * (cp_pwm.c keeps CH3 compare = (CCR+ARR)/2 = mid-LOW phase). The
     * earlier attempt at runtime ETSIC swap on ADC0 alone never
     * latched the second phase — likely a GD32F205 nuance with
     * mid-flight ETSIC mutation. Two ADCs sampling the same PA4 pad
     * is the spec-clean architecture. */
    rcu_periph_clock_enable(RCU_ADC1);
    adc_deinit(ADC1);
    adc_data_alignment_config(ADC1, ADC_DATAALIGN_RIGHT);
    adc_resolution_config(ADC1, ADC_RESOLUTION_12B);
    adc_special_function_config(ADC1, ADC_SCAN_MODE,       DISABLE);
    adc_special_function_config(ADC1, ADC_CONTINUOUS_MODE, DISABLE);
    adc_channel_length_config(ADC1, ADC_INSERTED_CHANNEL, 1U);
    adc_inserted_channel_config(ADC1, 0U, ADC_CHANNEL_4, ADC_SAMPLETIME_239POINT5);
    adc_external_trigger_source_config(ADC1, ADC_INSERTED_CHANNEL,
                                       ADC0_1_EXTTRIG_INSERTED_T0_CH3);
    adc_external_trigger_config(ADC1, ADC_INSERTED_CHANNEL, ENABLE);
    adc_interrupt_enable(ADC1, ADC_INT_EOIC);
    adc_enable(ADC1);
    for (volatile int i = 0; i < 10000; ++i) { }
    adc_calibration_enable(ADC1);

    /* NVIC priority 5 = configMAX_SYSCALL_INTERRUPT_PRIORITY. ADC0 +
     * ADC1 share IRQ ADC0_1_IRQn; ISR disambiguates via flag reads. */
    nvic_irq_enable(ADC0_1_IRQn, 5U, 0U);
}

uint16_t cp_high_raw(void) { return s_cp_high_raw; }
int32_t  cp_high_mv(void)  { return s_cp_high_mv; }
uint16_t cp_low_raw(void)  { return s_cp_low_raw; }
int32_t  cp_low_mv(void)   { return s_cp_low_mv; }

void ADC0_1_IRQHandler(void)
{
    if (RESET != adc_interrupt_flag_get(ADC0, ADC_INT_FLAG_EOIC)) {
        adc_interrupt_flag_clear(ADC0, ADC_INT_FLAG_EOIC);
        uint16_t raw = adc_inserted_data_read(ADC0, ADC_INSERTED_CHANNEL_0);
        s_cp_high_raw = raw;
        s_cp_high_mv  = convert_mv(raw);
    }
    if (RESET != adc_interrupt_flag_get(ADC1, ADC_INT_FLAG_EOIC)) {
        adc_interrupt_flag_clear(ADC1, ADC_INT_FLAG_EOIC);
        uint16_t raw = adc_inserted_data_read(ADC1, ADC_INSERTED_CHANNEL_0);
        s_cp_low_raw = raw;
        s_cp_low_mv  = convert_mv(raw);
    }
}
