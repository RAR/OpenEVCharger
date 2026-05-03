#include "cp_pwm.h"
#include "gd32f20x.h"

/* TIMER0 clock = 120 MHz (APB2 = 60 MHz × 2 because APB2 prescaler != 1
 * in the vendor's default 120 MHz clock tree). Prescaler 119 → 1 µs tick.
 * ARR 999 → 1000 µs period = 1 kHz.
 *
 * PWM0 mode: output HIGH while counter < CCR. CCR semantics here are
 * "ticks-of-pin-HIGH time per cycle". Whether pin-HIGH corresponds to
 * CP-HIGH or CP-LOW depends on the on-board level shifter polarity,
 * which is bench-determined per SKU (see CP_READBACK_INVERTED in
 * adc_inject.c). The PWM driver itself stays polarity-agnostic. */
#define CP_PWM_PSC      119U
#define CP_PWM_ARR      999U

#define CP_PWM_CCR_HIGH  (CP_PWM_ARR + 1U)   /* counter always < CCR → pin always HIGH */
#define CP_PWM_CCR_LOW   0U                  /* counter never < 0   → pin always LOW */

void cp_pwm_init(void)
{
    rcu_periph_clock_enable(RCU_TIMER0);
    rcu_periph_clock_enable(RCU_AF);

    gpio_pin_remap_config(GPIO_TIMER0_FULL_REMAP, ENABLE);

    timer_deinit(TIMER0);

    timer_parameter_struct tp;
    timer_struct_para_init(&tp);
    tp.prescaler         = CP_PWM_PSC;
    tp.alignedmode       = TIMER_COUNTER_EDGE;
    tp.counterdirection  = TIMER_COUNTER_UP;
    tp.period            = CP_PWM_ARR;
    tp.clockdivision     = TIMER_CKDIV_DIV1;
    tp.repetitioncounter = 0;
    timer_init(TIMER0, &tp);

    /* TRGO source = update event → drives ADC0 injected trigger in M3.2 */
    timer_master_output_trigger_source_select(TIMER0, TIMER_TRI_OUT_SRC_UPDATE);

    timer_oc_parameter_struct oc;
    timer_channel_output_struct_para_init(&oc);
    oc.outputstate    = TIMER_CCX_ENABLE;
    oc.outputnstate   = TIMER_CCXN_DISABLE;
    oc.ocpolarity     = TIMER_OC_POLARITY_HIGH;
    oc.ocnpolarity    = TIMER_OCN_POLARITY_HIGH;
    oc.ocidlestate    = TIMER_OC_IDLE_STATE_LOW;
    oc.ocnidlestate   = TIMER_OCN_IDLE_STATE_LOW;
    timer_channel_output_config(TIMER0, TIMER_CH_2, &oc);

    timer_channel_output_mode_config(TIMER0, TIMER_CH_2, TIMER_OC_MODE_PWM0);
    timer_channel_output_shadow_config(TIMER0, TIMER_CH_2, TIMER_OC_SHADOW_DISABLE);

    timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_2, CP_PWM_CCR_HIGH);

    timer_auto_reload_shadow_enable(TIMER0);
    timer_primary_output_config(TIMER0, ENABLE);

    timer_enable(TIMER0);
}

void cp_pwm_set_idle_high(void)
{
    timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_2, CP_PWM_CCR_HIGH);
}

void cp_pwm_set_state_f(void)
{
    timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_2, CP_PWM_CCR_LOW);
}

void cp_pwm_set_advertise_amps(uint8_t amps)
{
    if (amps < 6 || amps > 80) {
        cp_pwm_set_idle_high();
        return;
    }
    uint32_t duty_pct;
    if (amps <= 51) {
        duty_pct = ((uint32_t)amps * 6U) / 10U;
    } else {
        duty_pct = ((uint32_t)amps * 10U) / 25U + 64U;
    }
    if (duty_pct > 96U) duty_pct = 96U;
    uint32_t ccr = duty_pct * 10U;
    timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_2, ccr);
}
