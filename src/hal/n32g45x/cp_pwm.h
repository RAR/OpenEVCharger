#ifndef OPENEVCHARGER_BOARDS_NEXCYBER_HAL_CP_PWM_H
#define OPENEVCHARGER_BOARDS_NEXCYBER_HAL_CP_PWM_H

#include <stdint.h>

/* M3 J1772 control-pilot PWM driver for the Nations N32G45x.
 *
 * Mirrors the surface of src/hal/cp_pwm.h on the rippleon target
 * (init / set_idle_high / set_state_f / set_advertise_amps).
 *
 * Pad: PA8 = TIM1_CH1 (Nations default AF, no remap needed — unlike
 * rippleon which uses GPIO_TIMER0_FULL_REMAP to route TIMER0_CH3 to
 * PE13). The Nations N32G45x default-AF for PA8 is TIM1_CH1, so the
 * pad is wired straight to the timer output once AFIO + TIM1 clocks
 * are on and the pad is in AF_PP mode (already done in M2 GPIO HAL).
 *
 * Inverting-vs-non-inverting CP buffer status is bench-blocked on
 * the Nexcyber PCB. This implementation defaults to PWM2 mode (non-
 * inverting) — pin HIGH when CNT < CCR. If bench probe of CP vs PA8
 * shows inverting behaviour (CP +12 V when PA8 is LOW), flip
 * NEXCYBER_CP_PWM_INVERTING to 1 to recompile in PWM1 mode. */
void cp_pwm_init(void);
void cp_pwm_set_idle_high(void);
void cp_pwm_set_state_f(void);
void cp_pwm_set_advertise_amps(uint8_t amps);

#endif
