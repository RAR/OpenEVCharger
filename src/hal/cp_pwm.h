#ifndef OPENEVCHARGER_HAL_CP_PWM_H
#define OPENEVCHARGER_HAL_CP_PWM_H

#include <stdint.h>

/* Initialise TIMER0 + AFIO full-remap + PE13 = TIMER_CH_2 PWM at 1 kHz.
 * After this returns, the timer is running and CP is at +12 V (idle).
 * Idempotent. Must run after gpio_init_all() (which configures PE13 AF). */
void cp_pwm_init(void);

/* Set PWM duty to "always HIGH" — CP idle at +12 V (J1772 state-A advertise). */
void cp_pwm_set_idle_high(void);

/* Set PWM duty to "always LOW" — CP at -12 V (J1772 state-F, EVSE not ready). */
void cp_pwm_set_state_f(void);

/* Set advertised current via the J1772 duty cycle formula:
 *   6 <= A <= 51:  duty% = A * 0.6
 *   51 < A <= 80:  duty% = A / 2.5 + 64
 * Caller is responsible for clamping `amps` to a safe maximum. */
void cp_pwm_set_advertise_amps(uint8_t amps);

#endif
