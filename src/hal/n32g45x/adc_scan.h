#ifndef OPENEVCHARGER_BOARDS_NEXCYBER_HAL_ADC_SCAN_H
#define OPENEVCHARGER_BOARDS_NEXCYBER_HAL_ADC_SCAN_H

#include <stdint.h>

/* M3 ADC HAL — first cut.
 *
 * Mirrors the surface of src/hal/adc_scan.h on the rippleon target
 * (`adc_scan_init`, `adc_scan_latest`, `adc_scan_rank`) so any future
 * board-shared module can reach across without #ifdefs.
 *
 * BIG CAVEAT: the Nations N32G45x has 4 ADCs with channel-to-pin
 * mappings that are NOT STM32F1-compatible. From n32g45x_adc.h:
 *
 *   ADC1: PA0/PA1/PA6/PA3/PF4/PC0/PC1/PC2/PC3/PF2/PA2   (ch1-11)
 *   ADC2: PA4/PA5/PB1/PA7/PC4/PC0/PC1/PC2/PC3/PF2/PA2/PC5/PB2 (ch1-13)
 *   ADC3: PB11/PE9/PE13/PE12/PB13/PE8/PD10-14/PB0/PE7/PE10/PE11
 *   ADC4: PE14/PE15/PB12/PB14/PB15/PE8/PD10-14/PD8/PD9
 *
 * Nexcyber's 10 AIN-configured pads (PA4/PA5/PA6/PA7/PB0/PB1/PB2/
 * PC0/PC1/PC4/PC5) span ADC1/ADC2/ADC3. PA6 is ADC1-only; PB0 is
 * ADC3-only; the rest live on ADC2 (some shared with ADC1).
 *
 * This first cut uses **ADC1 only**, scanning the 3 AIN pads accessible
 * on it (PA6/PC0/PC1) plus VrefInt for calibration. That's enough to
 * exercise the ADC HAL surface, validate the DMA→SRAM ring, and read
 * back a reference voltage. Future revisions will add ADC2 (the bulk
 * of channels, including the bench-decoded CP_RAW/CC/I_L1/GFCI/I_L2
 * slots) and ADC3 (PB0).
 *
 * Channel-to-role mapping is bench-blocked. The SRAM ADC cache
 * decoded at boards/nexcyber/pin_map.h (slots 0x750-0x75A) tells us
 * which SIGNAL each cache halfword holds, but until we flash an M3
 * build and correlate via J1772 state walk + scope probing, we don't
 * know which PHYSICAL PIN feeds each role. Ranks are named by pin
 * here for that reason.
 */

#define ADC_RANK_PA6      0   /* ADC1 ch3 */
#define ADC_RANK_PC0      1   /* ADC1 ch6 */
#define ADC_RANK_PC1      2   /* ADC1 ch7 */
#define ADC_RANK_VREFINT  3   /* ADC1 ch18 — internal; calibration reference */
#define ADC_RANKS         4

/* ADC2 single-shot diagnostic scan — covers the AIN pads that ADC1
 * can't reach. Filled by adc2_diag_scan() (called periodically from
 * the heartbeat task). Channel-to-pin mapping per Nations N32G45x
 * SPL header (NOT STM32F1-compatible):
 *
 *   slot  pin   ADC2 ch   notes
 *   ────  ────  ───────   ─────────────────────────────────
 *   0     PA4    1        candidate voltage sense
 *   1     PA5    2        candidate voltage sense
 *   2     PB1    3        candidate voltage sense
 *   3     PA7    4        candidate voltage sense
 *   4     PC4    5        candidate CC / NTC
 *   5     PC5   12        candidate NTC / GFCI sense
 *   6     PB2   13        candidate aux
 *
 * The user can peek `adc2_diag_buf` (address surfaced via `nm
 * openevcharger.elf | grep adc2_diag_buf`) at different J1772 CP
 * states; the slot whose raw value swings ~0x09AC → 0x06EC across
 * state A → state C is the CP_RAW pin we've been chasing. */

#define ADC2_DIAG_RANKS   7

/* Bench-confirmed 2026-05-11 5-point cal:
 *   CP_mV ≈ raw * 1000 / 187
 * (raw from ADC2 ch13 = PB2; slot 6 in adc2_diag_buf) */
#define ADC2_DIAG_SLOT_CP_RAW   6
#define ADC2_CP_CAL_DIVISOR     187     /* raw / 187 = volts */

void adc2_diag_scan(void);   /* fills internal adc2_diag_buf */

/* Snapshot helper for callers that want a stable copy. */
void adc2_diag_latest(uint16_t out[ADC2_DIAG_RANKS]);

/* Latest CP raw value (PB2 ADC reading) — for direct sampling. */
uint16_t adc2_cp_raw(void);

/* Calibrated CP voltage in mV (signed). Saturates non-negatively
 * because the level-shifter floors at CP ≈ 0 V. */
int32_t adc2_cp_mv(void);

/* Initialise ADC1 + DMA1 channel 1 for circular scan. GPIO pads must
 * already be in analog mode (gpio_init_all()). Safe to call once at
 * boot before vTaskStartScheduler(). */
void adc_scan_init(void);

/* Snapshot the latest 4 samples atomically into out[]. ~tens of cycles. */
void adc_scan_latest(uint16_t out[ADC_RANKS]);

/* Most recent DMA'd value for one rank. Use adc_scan_latest() if you
 * need rank coherence. Returns 0 for out-of-range rank. */
uint16_t adc_scan_rank(unsigned rank);

#endif
