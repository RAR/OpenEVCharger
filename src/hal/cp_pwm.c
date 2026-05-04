#include "cp_pwm.h"
#include "gd32f20x.h"

/* TIMER0 clock = 120 MHz (APB2 = 60 MHz × 2 because APB2 prescaler != 1
 * in the vendor's default 120 MHz clock tree). Prescaler 119 → 1 µs tick.
 * ARR 999 → 1000 µs period = 1 kHz.
 *
 * The on-board CP buffer INVERTS: MCU pin HIGH = CP -12 V (scope-verified
 * 2026-05-02 on the bench Rippleon ROC001 unit). To keep the user-facing
 * CCR semantic correct (CCR = ticks of CP-HIGH time per period), the
 * channel runs in PWM1 mode (output LOW while counter < CCR) so that
 * pin-LOW ticks = CP-HIGH ticks via the inverter. */
#define CP_PWM_PSC      119U
#define CP_PWM_ARR      999U

#define CP_PWM_CCR_HIGH  (CP_PWM_ARR + 1U)   /* CNT never > CCR → pin always LOW → CP +12 V */
#define CP_PWM_CCR_LOW   0U                  /* CNT always > 0  → pin always HIGH → CP -12 V */

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

    timer_channel_output_mode_config(TIMER0, TIMER_CH_2, TIMER_OC_MODE_PWM1);
    timer_channel_output_shadow_config(TIMER0, TIMER_CH_2, TIMER_OC_SHADOW_DISABLE);

    timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_2, CP_PWM_CCR_HIGH);

    /* CH3 is configured for compare-only — its internal CC3IF event
     * drives the ADC1 inject trigger (T0_CH3 source). The compare
     * value is set by cp_pwm_set_advertise_amps to (CCR+ARR)/2 (mid-
     * LOW phase) so adc_inject can sample CP during the negative
     * excursion. CCxE = ENABLE because some timer revisions gate
     * the trigger pulse on output-enable; the GPIO output goes to
     * PE14 (full-remap) which isn't routed anywhere on this PCB,
     * so enabling the output is harmless. */
    timer_oc_parameter_struct oc3;
    timer_channel_output_struct_para_init(&oc3);
    oc3.outputstate    = TIMER_CCX_ENABLE;
    oc3.outputnstate   = TIMER_CCXN_DISABLE;
    oc3.ocpolarity     = TIMER_OC_POLARITY_HIGH;
    oc3.ocnpolarity    = TIMER_OCN_POLARITY_HIGH;
    oc3.ocidlestate    = TIMER_OC_IDLE_STATE_LOW;
    oc3.ocnidlestate   = TIMER_OCN_IDLE_STATE_LOW;
    timer_channel_output_config(TIMER0, TIMER_CH_3, &oc3);
    timer_channel_output_mode_config(TIMER0, TIMER_CH_3, TIMER_OC_MODE_PWM1);
    timer_channel_output_shadow_config(TIMER0, TIMER_CH_3, TIMER_OC_SHADOW_DISABLE);
    timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_3, CP_PWM_ARR - 1U);

    timer_auto_reload_shadow_enable(TIMER0);
    timer_primary_output_config(TIMER0, ENABLE);

    timer_enable(TIMER0);
}

static void update_ch3_for_low_sample(uint32_t ccr)
{
    /* Mid-LOW-phase compare. Clamped so CH3 always fires within the
     * period — if duty ≈ 100% (CCR > ARR) we still fire near period
     * end and the cp_low sample matches HIGH-phase value (no LOW
     * window exists), which is correct telemetry. */
    uint32_t mid = (ccr + CP_PWM_ARR) / 2U;
    if (mid > CP_PWM_ARR - 1U) mid = CP_PWM_ARR - 1U;
    timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_3, mid);
}

void cp_pwm_set_idle_high(void)
{
    timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_2, CP_PWM_CCR_HIGH);
    update_ch3_for_low_sample(CP_PWM_CCR_HIGH);
}

void cp_pwm_set_state_f(void)
{
    timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_2, CP_PWM_CCR_LOW);
    update_ch3_for_low_sample(CP_PWM_CCR_LOW);
}

void cp_pwm_set_advertise_amps(uint8_t amps)
{
    if (amps < 6 || amps > 80) {
        cp_pwm_set_idle_high();
        return;
    }
    /* J1772 / IEC 61851-1 PWM-duty ↔ advertised current map:
     *   duty_pct =  10..85 %   →  amps = duty × 0.6   (so duty = amps / 0.6)
     *   duty_pct =  85..96 %   →  amps = (duty − 64) × 2.5
     *
     * Earlier formula `amps * 6 / 10` was the INVERSE of the spec —
     * it produced amps × 0.6 instead of amps / 0.6, advertising 48 A
     * as 28 % duty instead of 80 %. Bench-confirmed via multimeter
     * (no-load CP averaging −5.9 V instead of the expected +7 V at
     * 80 % high; an EV would have charged at ~17 A, not 48 A).
     *
     * Use *10/6 (= /0.6) for the lower branch. The upper branch
     * (`amps * 10 / 25 + 64` = amps/2.5 + 64) was already correct. */
    uint32_t duty_pct;
    if (amps <= 51) {
        duty_pct = ((uint32_t)amps * 10U) / 6U;
    } else {
        duty_pct = ((uint32_t)amps * 10U) / 25U + 64U;
    }
    if (duty_pct > 96U) duty_pct = 96U;
    uint32_t ccr = duty_pct * 10U;
    timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_2, ccr);
    update_ch3_for_low_sample(ccr);
}
