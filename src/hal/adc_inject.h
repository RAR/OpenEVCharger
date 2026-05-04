#ifndef OPENBHZD_HAL_ADC_INJECT_H
#define OPENBHZD_HAL_ADC_INJECT_H

#include <stdint.h>

/* Configure ADC0 injected channel group: 1 rank PA4, triggered by
 * TIMER0 TRGO (= TIMER0 update event = start of PWM cycle). On each
 * conversion completion, the EOC ISR latches `cp_high_raw` and the
 * derived `cp_high_mv` (signed mV at the CP wire after divider invert).
 * Must run AFTER adc_scan_init() and cp_pwm_init(). */
void adc_inject_init(void);

/* Latest CP HIGH-phase ADC raw value (12-bit, 0..4095). */
uint16_t cp_high_raw(void);

/* Latest CP HIGH-phase voltage in mV at the CP wire.
 * Range: -12000..+12000 (clamped). */
int32_t cp_high_mv(void);

/* CP LOW-phase counterparts. Sampled by alternating the ADC injected
 * trigger source between TIMER0 update (start of period = HIGH phase)
 * and TIMER0 CH3 (mid-LOW phase, compare value managed by cp_pwm.c).
 * Result: cp_high and cp_low each refresh at ~1 kHz pair rate.
 *
 * Bench note: the OEM read divider can't resolve negative voltages —
 * any CP < 0 V pegs PA4 near 0 raw, so the converted cp_low_mv is
 * largely informational until a 5-point negative-half cal lands
 * (M5.b 5-point fit, gates FAULT_DIODE_CHECK). */
uint16_t cp_low_raw(void);
int32_t  cp_low_mv(void);

#endif
