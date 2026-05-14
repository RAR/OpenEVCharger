/* boards/nexcyber/hal/cp_pwm.c — M3 J1772 control-pilot PWM driver.
 *
 * TIM1_CH1 → PA8 default AF, 1 kHz PWM, configurable duty.
 *
 * Clock math (SDK default chain — HSE 8 MHz × PLL ×18 → SYSCLK 144 MHz,
 * APB2 prescaler = /2 → APB2 clk = 72 MHz, TIM1 clock = APB2 × 2 = 144
 * MHz since the APB2 prescaler is != 1):
 *
 *   Prescaler 143 → 144 MHz / 144 = 1 MHz tick → 1 µs / tick
 *   Period (ARR) 999 → 1000 ticks per period = 1 kHz ✓
 *
 * Once clock_real_120m_init() actually relocks to 120 MHz, the PSC
 * needs to drop to 119. Conditional defined below for both rates so
 * the eventual clock-reconfig commit is a single-line change here.
 *
 * PWM mode default = PWM2 (non-inverting): output is HIGH while
 * CNT < CCR. CCR=999 → 99.9% HIGH; CCR=0 → 0% HIGH (always LOW).
 *
 * If the Nexcyber's CP buffer turns out to invert (like rippleon's
 * does — pin HIGH = CP -12 V on that PCB), flip the
 * NEXCYBER_CP_PWM_INVERTING build flag to switch to PWM1 mode.
 * Bench validation needed before declaring this production-ready.
 */

#include "hal/cp_pwm.h"
#include "n32g45x.h"

/* PWM mode semantic (STM32F1/Nations TIM, upcounting, HIGH polarity):
 *   PWM1: OCxREF=1 if CNT<CCR, else 0  →  pin HIGH while CNT<CCR
 *   PWM2: OCxREF=0 if CNT<CCR, else 1  →  pin HIGH while CNT>=CCR (inverted duty)
 *
 * Bench observation 2026-05-11: PWM2 mode + CCR=ARR+1 produced CP -12 V
 * → the Nexcyber CP buffer is NON-INVERTING (pin LOW → CP LOW). In
 * PWM2 + CCR=ARR+1, CNT is always < CCR, so OCxREF=0 → pin LOW → CP
 * LOW → -12 V. We want pin HIGH for CP +12 V idle, which means PWM1
 * mode with CCR=ARR+1 (CNT always < CCR → OCxREF=1 → pin HIGH).
 *
 * Defaulting to PWM1 mode now. Build flag NEXCYBER_CP_PWM_INVERTING=1
 * stays as an escape hatch in case a future PCB revision has an
 * inverting buffer (set =1 to use PWM2 mode then). Naming kept for
 * continuity even though "inverting" now means "swap PWM mode" not
 * "swap buffer polarity". */

#ifndef NEXCYBER_CP_PWM_INVERTING
#define NEXCYBER_CP_PWM_INVERTING 0
#endif

/* Pick PSC based on the active sysclk. Default chain is 144 MHz. */
#ifndef NEXCYBER_CP_PWM_TIM1_CLOCK_HZ
#define NEXCYBER_CP_PWM_TIM1_CLOCK_HZ 144000000U
#endif

#define CP_PWM_PSC      ((NEXCYBER_CP_PWM_TIM1_CLOCK_HZ / 1000000U) - 1U)  /* → 1 µs tick */
#define CP_PWM_ARR      999U                                                /* 1 kHz */
#define CP_PWM_TICK_HZ  (NEXCYBER_CP_PWM_TIM1_CLOCK_HZ / (CP_PWM_PSC + 1U))

#if NEXCYBER_CP_PWM_INVERTING
/* PWM2 mode for inverting CP buffer (none observed yet — escape hatch).
 * CCR=ARR+1: CNT always < CCR → OCxREF=0 → pin LOW → CP +12 V (inverter).
 * CCR=0:     CNT > CCR always → OCxREF=1 → pin HIGH → CP -12 V. */
#  define CP_PWM_OC_MODE     TIM_OCMODE_PWM2
#  define CP_PWM_CCR_IDLE_HI (CP_PWM_ARR + 1U)
#  define CP_PWM_CCR_STATE_F 0U
#else
/* PWM1 mode for non-inverting buffer (Nexcyber bench-confirmed 2026-05-11).
 * CCR=ARR+1: CNT always < CCR → OCxREF=1 → pin HIGH → CP +12 V.
 * CCR=0:     CNT >= CCR always → OCxREF=0 → pin LOW → CP -12 V. */
#  define CP_PWM_OC_MODE     TIM_OCMODE_PWM1
#  define CP_PWM_CCR_IDLE_HI (CP_PWM_ARR + 1U)
#  define CP_PWM_CCR_STATE_F 0U
#endif

void cp_pwm_init(void)
{
    /* TIM1 + AFIO on APB2. GPIOA also (PA8 owns TIM1_CH1). All three
     * may already be enabled by earlier init steps; bit-OR is no-op. */
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_TIM1
                            | RCC_APB2_PERIPH_AFIO
                            | RCC_APB2_PERIPH_GPIOA,
                            ENABLE);

    /* Time-base config. */
    TIM_DeInit(TIM1);

    TIM_TimeBaseInitType tb;
    TIM_InitTimBaseStruct(&tb);
    tb.Prescaler = CP_PWM_PSC;
    tb.CntMode   = TIM_CNT_MODE_UP;
    tb.Period    = CP_PWM_ARR;
    tb.ClkDiv    = TIM_CLK_DIV1;
    tb.RepetCnt  = 0;
    TIM_InitTimeBase(TIM1, &tb);

    /* OC1 = main PWM output → PA8. */
    OCInitType oc;
    TIM_InitOcStruct(&oc);
    oc.OcMode       = CP_PWM_OC_MODE;
    oc.OutputState  = TIM_OUTPUT_STATE_ENABLE;
    oc.OutputNState = TIM_OUTPUT_NSTATE_DISABLE;
    oc.Pulse        = CP_PWM_CCR_IDLE_HI;
    oc.OcPolarity   = TIM_OC_POLARITY_HIGH;
    oc.OcNPolarity  = TIM_OCN_POLARITY_HIGH;
    oc.OcIdleState  = TIM_OC_IDLE_STATE_RESET;
    oc.OcNIdleState = TIM_OCN_IDLE_STATE_RESET;
    TIM_InitOc1(TIM1, &oc);

    /* Preload shadow registers so a CCR update happens on the next
     * update event instead of mid-period (clean glitch-free duty
     * transitions). */
    TIM_ConfigArPreload(TIM1, ENABLE);

    /* MAIN-OUTPUT enable. TIM1/TIM8 advanced timers gate their PWM
     * outputs behind the MOE bit; without this the OCx pin stays low
     * even with the time-base running. */
    TIM_EnableCtrlPwmOutputs(TIM1, ENABLE);

    /* Start the counter. */
    TIM_Enable(TIM1, ENABLE);
}

void cp_pwm_set_idle_high(void)
{
    TIM_SetCmp1(TIM1, CP_PWM_CCR_IDLE_HI);
}

void cp_pwm_set_state_f(void)
{
    TIM_SetCmp1(TIM1, CP_PWM_CCR_STATE_F);
}

void cp_pwm_set_advertise_amps(uint8_t amps)
{
    if (amps < 6 || amps > 80) {
        cp_pwm_set_idle_high();
        return;
    }
    /* J1772 / IEC 61851-1 duty-to-amperage map:
     *   6 <= A <= 51:  duty% = A / 0.6   (so A = duty × 0.6)
     *   51 < A <= 80:  duty% = A / 2.5 + 64
     *
     * Same formula as rippleon (src/hal/cp_pwm.c). The lower branch
     * needs amps × 10 / 6 (= amps / 0.6) to map 48 A → 80 % duty
     * (NOT amps × 6 / 10 which inverts the formula). */
    uint32_t duty_pct;
    if (amps <= 51) {
        duty_pct = ((uint32_t)amps * 10U) / 6U;
    } else {
        duty_pct = ((uint32_t)amps * 10U) / 25U + 64U;
    }
    if (duty_pct > 96U) duty_pct = 96U;

    /* CCR = duty_pct * (ARR+1) / 100. With ARR=999 → CCR = duty_pct * 10. */
    uint32_t ccr = duty_pct * (CP_PWM_ARR + 1U) / 100U;
    TIM_SetCmp1(TIM1, (uint16_t)ccr);
}
