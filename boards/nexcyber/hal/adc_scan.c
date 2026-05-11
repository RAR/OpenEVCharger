/* boards/nexcyber/hal/adc_scan.c — M3 ADC HAL first cut for the N32G45x.
 *
 * ADC1 + DMA1 ch1 in scan + continuous mode. 4 ranks: PA6 (ch3),
 * PC0 (ch6), PC1 (ch7), VrefInt (ch18). See adc_scan.h for the
 * rationale and the bench-blocked work to extend to ADC2/ADC3.
 *
 * Surface mirrors src/hal/adc_scan.c on the rippleon target — same
 * function names, same atomic snapshot pattern, just GD32 → Nations
 * SPL spelling differences and ADC channel count (4 vs 11).
 *
 * No FreeRTOS dependency. The DMA hardware streams samples into
 * s_adc_buf without CPU involvement after init; FreeRTOS task code
 * just reads the buffer when it needs a value.
 */

#include "hal/adc_scan.h"
#include "n32g45x.h"

/* DMA destination buffer. The map-file address is what matters for
 * the halt-and-peek validation step on the bench (open the .map,
 * grep s_adc_buf, halt the MCU via openocd, peek the address,
 * confirm 4 halfwords being updated). */
static volatile uint16_t s_adc_buf[ADC_RANKS];

void adc_scan_init(void)
{
    /* Nations N32G45x deviation from STM32F1: the ADC1-4 peripherals
     * live on the AHB bus (RCC_AHB_PERIPH_ADCx), not on APB2 as the
     * F1-compatible peripheral map would suggest. DMA1 is also on AHB
     * (same as F1). GPIOs + AFIO stay on APB2.
     *
     * RCC_EnableAxxPeriphClk is bit-OR so re-enabling already-on
     * peripherals is a no-op — GPIOA/C were already turned on by
     * uart_init() / gpio_init_all(); enabling here keeps the function
     * idempotent. */
    RCC_EnableAHBPeriphClk(RCC_AHB_PERIPH_ADC1
                           | RCC_AHB_PERIPH_DMA1,
                           ENABLE);

    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOA
                            | RCC_APB2_PERIPH_GPIOC,
                            ENABLE);

    /* ADC clock prescaler. With AHB at ~120 MHz (post clock_real_120m_init),
     * /6 = 20 MHz — above the 14 MHz F1 max but the N32G45x part
     * supports higher ADC clocks (RM lists 28 MHz typical max). Pick
     * /8 = 15 MHz for safety margin. Sample time 239.5 cycles → total
     * conversion ≈ 17 µs per channel; 4 channels ≈ 68 µs per full scan
     * → ~14.7 kHz scan rate, more than fast enough for J1772 / safety. */
    RCC_ConfigAdcHclk(RCC_ADCHCLK_DIV8);

    /* ADC1 in scan + continuous mode, regular sequence length = 4. */
    ADC_DeInit(ADC1);

    ADC_InitType cfg;
    ADC_InitStruct(&cfg);
    cfg.WorkMode        = ADC_WORKMODE_INDEPENDENT;
    cfg.MultiChEn       = ENABLE;            /* scan mode (multi-channel) */
    cfg.ContinueConvEn  = ENABLE;            /* self-loops after first trigger */
    cfg.ExtTrigSelect   = ADC_EXT_TRIGCONV_NONE;
    cfg.DatAlign        = ADC_DAT_ALIGN_R;
    cfg.ChsNumber       = ADC_RANKS;
    ADC_Init(ADC1, &cfg);

    /* Channel assignments. Rank values are 1-based in the SPL despite
     * the underlying RSQR fields being 0-indexed — see n32g45x_adc.c
     * ADC_ConfigRegularChannel for the off-by-one fix. */
    ADC_ConfigRegularChannel(ADC1, ADC_CH_3,  1, ADC_SAMP_TIME_239CYCLES5); /* PA6 */
    ADC_ConfigRegularChannel(ADC1, ADC_CH_6,  2, ADC_SAMP_TIME_239CYCLES5); /* PC0 */
    ADC_ConfigRegularChannel(ADC1, ADC_CH_7,  3, ADC_SAMP_TIME_239CYCLES5); /* PC1 */
    ADC_ConfigRegularChannel(ADC1, ADC_CH_18, 4, ADC_SAMP_TIME_239CYCLES5); /* VREFINT */

    /* Enable temp-sensor + VrefInt internal channel power. Mirrors
     * adc_tempsensor_vrefint_enable() on the GD32. */
    ADC_EnableTempSensorVrefint(ENABLE);

    /* DMA1 channel 1 → s_adc_buf, circular, 16-bit transfers. */
    DMA_DeInit(DMA1_CH1);

    DMA_InitType d;
    DMA_StructInit(&d);
    d.PeriphAddr     = (uint32_t)&ADC1->DAT;
    d.MemAddr        = (uint32_t)s_adc_buf;
    d.Direction      = DMA_DIR_PERIPH_SRC;
    d.BufSize        = ADC_RANKS;
    d.PeriphInc      = DMA_PERIPH_INC_DISABLE;
    d.DMA_MemoryInc  = DMA_MEM_INC_ENABLE;
    d.PeriphDataSize = DMA_PERIPH_DATA_SIZE_HALFWORD;
    d.MemDataSize    = DMA_MemoryDataSize_HalfWord;
    d.CircularMode   = DMA_MODE_CIRCULAR;
    d.Priority       = DMA_PRIORITY_HIGH;
    d.Mem2Mem        = DMA_M2M_DISABLE;
    DMA_Init(DMA1_CH1, &d);

    DMA_EnableChannel(DMA1_CH1, ENABLE);

    /* Hand ADC1 to DMA + power it up. */
    ADC_EnableDMA(ADC1, ENABLE);
    ADC_Enable(ADC1, ENABLE);

    /* Brief settle window (~10 µs at 120 MHz) before launching the
     * calibration cycle. The compiler can't optimise this loop out
     * because of the volatile counter. */
    for (volatile int i = 0; i < 1200; ++i) { }

    /* Calibrate. Mandatory on F1-style ADCs after power-up to subtract
     * the offset error from the converter. */
    ADC_StartCalibration(ADC1);
    while (ADC_GetCalibrationStatus(ADC1) == SET) { }

    /* In continuous mode the software trigger fires the first
     * conversion; the ADC self-clocks afterwards. */
    ADC_EnableSoftwareStartConv(ADC1, ENABLE);
}

void adc_scan_latest(uint16_t out[ADC_RANKS])
{
    __disable_irq();
    for (unsigned i = 0; i < ADC_RANKS; ++i) {
        out[i] = s_adc_buf[i];
    }
    __enable_irq();
}

uint16_t adc_scan_rank(unsigned rank)
{
    if (rank >= ADC_RANKS) return 0;
    return s_adc_buf[rank];
}

/* ──────────────────────────────────────────────────────────────────
 * ADC2 single-shot diagnostic scan
 * ──────────────────────────────────────────────────────────────────
 *
 * Used to identify which physical AIN pad on the Nexcyber PCB carries
 * CP_RAW, CC, I_L1/L2, GFCI sense, etc. Stock fw's SRAM cache decode
 * told us the *signal roles* per slot (pin_map.h SRAM cache notes),
 * but not which *physical pin* feeds each role. ADC1 scan above
 * (PA6/PC0/PC1) doesn't show any swing on CP changes — so the
 * relevant pins must live on ADC2/ADC3 only.
 *
 * This scanner runs ADC2 on-demand (no DMA) and stores results in
 * adc2_diag_buf which the bench operator peeks via SWD during a
 * J1772 state walk to identify CP. */

/* Channel sequence — matches adc_scan.h ADC2_DIAG_RANKS commentary. */
static const uint8_t s_adc2_diag_channels[ADC2_DIAG_RANKS] = {
    1,   /* PA4 */
    2,   /* PA5 */
    3,   /* PB1 */
    4,   /* PA7 */
    5,   /* PC4 */
    12,  /* PC5 */
    13,  /* PB2 */
};

/* Exposed via SWD — `nm openevcharger.elf | grep adc2_diag_buf` for
 * address. Not declared `static` so the symbol survives in the .map. */
volatile uint16_t adc2_diag_buf[ADC2_DIAG_RANKS];

static int s_adc2_inited = 0;

static void adc2_init_oneshot(void)
{
    /* Same RCC + clock prescaler as ADC1; ADC2 lives in the same
     * AHB-side cluster. */
    RCC_EnableAHBPeriphClk(RCC_AHB_PERIPH_ADC2, ENABLE);

    ADC_DeInit(ADC2);
    ADC_InitType cfg;
    ADC_InitStruct(&cfg);
    cfg.WorkMode        = ADC_WORKMODE_INDEPENDENT;
    cfg.MultiChEn       = DISABLE;            /* single-channel mode */
    cfg.ContinueConvEn  = DISABLE;            /* one conversion per trigger */
    cfg.ExtTrigSelect   = ADC_EXT_TRIGCONV_NONE;
    cfg.DatAlign        = ADC_DAT_ALIGN_R;
    cfg.ChsNumber       = 1;
    ADC_Init(ADC2, &cfg);

    ADC_Enable(ADC2, ENABLE);
    for (volatile int i = 0; i < 1200; ++i) { }
    ADC_StartCalibration(ADC2);
    while (ADC_GetCalibrationStatus(ADC2) == SET) { }

    s_adc2_inited = 1;
}

static uint16_t adc2_read_channel(uint8_t ch)
{
    /* Program a single-rank regular sequence for the requested
     * channel, software-trigger, poll for ENDC, read DAT. */
    ADC_ConfigRegularChannel(ADC2, ch, 1, ADC_SAMP_TIME_239CYCLES5);
    ADC_EnableSoftwareStartConv(ADC2, ENABLE);
    while (ADC_GetFlagStatus(ADC2, ADC_FLAG_ENDC) == RESET) { }
    ADC_ClearFlag(ADC2, ADC_FLAG_ENDC);
    return (uint16_t)(ADC_GetDat(ADC2) & 0xFFFF);
}

void adc2_diag_scan(void)
{
    if (!s_adc2_inited) {
        adc2_init_oneshot();
    }
    for (unsigned i = 0; i < ADC2_DIAG_RANKS; ++i) {
        adc2_diag_buf[i] = adc2_read_channel(s_adc2_diag_channels[i]);
    }
}

void adc2_diag_latest(uint16_t out[ADC2_DIAG_RANKS])
{
    __disable_irq();
    for (unsigned i = 0; i < ADC2_DIAG_RANKS; ++i) {
        out[i] = adc2_diag_buf[i];
    }
    __enable_irq();
}
