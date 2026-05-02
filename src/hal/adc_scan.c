#include "adc_scan.h"
#include "gd32f20x.h"

/* DMA destination buffer. The map-file address is what matters for the
 * halt-and-peek validation step. */
static volatile uint16_t s_adc_buf[ADC_RANKS];

void adc_scan_init(void)
{
    /* Clocks: SPL "DMA0" == hardware DMA1; SPL "ADC0" == ADC1 instance. */
    rcu_periph_clock_enable(RCU_DMA0);
    rcu_periph_clock_enable(RCU_ADC0);

    /* ADCCLK = APB2 / 6 = 60 MHz / 6 = 10 MHz (within 14 MHz max) */
    rcu_adc_clock_config(RCU_CKADC_CKAPB2_DIV6);

    /* ADC0 in scan + continuous mode, 11 ranks. */
    adc_deinit(ADC0);
    adc_mode_config(ADC_MODE_FREE);
    adc_data_alignment_config(ADC0, ADC_DATAALIGN_RIGHT);
    adc_resolution_config(ADC0, ADC_RESOLUTION_12B);

    adc_special_function_config(ADC0, ADC_SCAN_MODE,       ENABLE);
    adc_special_function_config(ADC0, ADC_CONTINUOUS_MODE, ENABLE);

    adc_channel_length_config(ADC0, ADC_REGULAR_CHANNEL, ADC_RANKS);

    adc_regular_channel_config(ADC0,  0, ADC_CHANNEL_2,  ADC_SAMPLETIME_239POINT5); /* PA2  */
    adc_regular_channel_config(ADC0,  1, ADC_CHANNEL_3,  ADC_SAMPLETIME_239POINT5); /* PA3  */
    adc_regular_channel_config(ADC0,  2, ADC_CHANNEL_10, ADC_SAMPLETIME_239POINT5); /* PC0  */
    adc_regular_channel_config(ADC0,  3, ADC_CHANNEL_11, ADC_SAMPLETIME_239POINT5); /* PC1  */
    adc_regular_channel_config(ADC0,  4, ADC_CHANNEL_4,  ADC_SAMPLETIME_239POINT5); /* PA4  */
    adc_regular_channel_config(ADC0,  5, ADC_CHANNEL_7,  ADC_SAMPLETIME_239POINT5); /* PA7  */
    adc_regular_channel_config(ADC0,  6, ADC_CHANNEL_15, ADC_SAMPLETIME_239POINT5); /* PC5  */
    adc_regular_channel_config(ADC0,  7, ADC_CHANNEL_8,  ADC_SAMPLETIME_239POINT5); /* PB0  */
    adc_regular_channel_config(ADC0,  8, ADC_CHANNEL_9,  ADC_SAMPLETIME_239POINT5); /* PB1  */
    adc_regular_channel_config(ADC0,  9, ADC_CHANNEL_13, ADC_SAMPLETIME_239POINT5); /* PC3  */
    adc_regular_channel_config(ADC0, 10, ADC_CHANNEL_17, ADC_SAMPLETIME_239POINT5); /* VREFINT */

    /* Enable internal VREFINT & temp-sensor channel (sets ADC_CTL1_TSVREN). */
    adc_tempsensor_vrefint_enable();

    /* Software-trigger as the regular external trigger. Continuous mode
     * self-loops after the first conversion. */
    adc_external_trigger_source_config(ADC0, ADC_REGULAR_CHANNEL,
                                       ADC0_1_2_EXTTRIG_REGULAR_NONE);
    adc_external_trigger_config(ADC0, ADC_REGULAR_CHANNEL, ENABLE);

    /* DMA1 channel 0 → s_adc_buf, circular. */
    dma_parameter_struct cfg;
    dma_struct_para_init(&cfg);
    cfg.periph_addr  = (uint32_t)&ADC_RDATA(ADC0);
    cfg.periph_width = DMA_PERIPHERAL_WIDTH_16BIT;
    cfg.periph_inc   = DMA_PERIPH_INCREASE_DISABLE;
    cfg.memory_addr  = (uint32_t)s_adc_buf;
    cfg.memory_width = DMA_MEMORY_WIDTH_16BIT;
    cfg.memory_inc   = DMA_MEMORY_INCREASE_ENABLE;
    cfg.direction    = DMA_PERIPHERAL_TO_MEMORY;
    cfg.number       = ADC_RANKS;
    cfg.priority     = DMA_PRIORITY_HIGH;
    dma_deinit(DMA0, DMA_CH0);
    dma_init(DMA0, DMA_CH0, &cfg);
    dma_circulation_enable(DMA0, DMA_CH0);
    dma_memory_to_memory_disable(DMA0, DMA_CH0);
    dma_channel_enable(DMA0, DMA_CH0);

    /* Hand ADC0 to DMA, calibrate, and start. */
    adc_dma_mode_enable(ADC0);
    adc_enable(ADC0);

    /* Settle ~10 µs before calibration. */
    for (volatile int i = 0; i < 10000; ++i) { }
    adc_calibration_enable(ADC0);

    /* In continuous mode, the software trigger fires the first conversion
     * and the ADC self-clocks afterwards. */
    adc_software_trigger_enable(ADC0, ADC_REGULAR_CHANNEL);
}

void adc_scan_latest(uint16_t out[ADC_RANKS])
{
    __disable_irq();
    for (unsigned i = 0; i < ADC_RANKS; ++i) out[i] = s_adc_buf[i];
    __enable_irq();
}

uint16_t adc_scan_rank(unsigned rank)
{
    if (rank >= ADC_RANKS) return 0;
    return s_adc_buf[rank];
}
