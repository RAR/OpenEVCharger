#ifndef OPENBHZD_HAL_ADC_SCAN_H
#define OPENBHZD_HAL_ADC_SCAN_H

#include <stdint.h>

#define ADC_RANK_AC      0
#define ADC_RANK_NTC1    1
#define ADC_RANK_CT      2
#define ADC_RANK_LCT     3
#define ADC_RANK_CP      4
#define ADC_RANK_CC      5
#define ADC_RANK_PE      6
#define ADC_RANK_NTC2    7
#define ADC_RANK_UNUSED  8
#define ADC_RANK_BTN     9
#define ADC_RANK_VREF    10
#define ADC_RANKS        11

/* Initialise ADC0 + DMA1 channel 0 for 11-rank circular scan.
 * GPIO pads must already be in analog mode (gpio_init_all()). */
void adc_scan_init(void);

/* Snapshot the latest 11 samples atomically into out[]. Cost ~50 cycles. */
void adc_scan_latest(uint16_t out[ADC_RANKS]);

/* Most recently DMA'd value for one rank. Use adc_scan_latest() if
 * you need rank coherence. */
uint16_t adc_scan_rank(unsigned rank);

#endif
