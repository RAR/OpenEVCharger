#ifndef OPENBHZD_CORE_PIN_MAP_H
#define OPENBHZD_CORE_PIN_MAP_H

/* Canonical pin assignments for OpenBHZD on the Rippleon ROC001 board.
 *
 * Source: rippleon/docs/mcu-re/pinout.md (canonical table). Every pin
 * marked load-bearing in that table appears here. Reverse direction is
 * also true — if you add an entry here, update pinout.md to match.
 *
 * Layout convention per pin:
 *   PIN_<role>_PORT  -> GPIOx peripheral handle
 *   PIN_<role>_PIN   -> GPIO_PIN_n
 *   PIN_<role>_RCU   -> RCU_GPIOx
 *   (per-pin role notes inline)
 */

#include "gd32f20x.h"

/* ----- Heartbeat LED ----- */
#define PIN_HEARTBEAT_PORT      GPIOD
#define PIN_HEARTBEAT_PIN       GPIO_PIN_4
#define PIN_HEARTBEAT_RCU       RCU_GPIOD

/* ----- Debug UART: USART1 PA9 TX / PA10 RX (no remap) ----- */
#define PIN_USART1_TX_PORT      GPIOA
#define PIN_USART1_TX_PIN       GPIO_PIN_9
#define PIN_USART1_RX_PORT      GPIOA
#define PIN_USART1_RX_PIN       GPIO_PIN_10
#define PIN_USART1_RCU          RCU_GPIOA

/* ----- ADC analog inputs (rank order matches DMA buffer layout) ----- */
#define PIN_ADC_AC_PORT         GPIOA
#define PIN_ADC_AC_PIN          GPIO_PIN_2     /* rank 0, ch 2  */
#define PIN_ADC_NTC1_PORT       GPIOA
#define PIN_ADC_NTC1_PIN        GPIO_PIN_3     /* rank 1, ch 3  */
#define PIN_ADC_CT_PORT         GPIOC
#define PIN_ADC_CT_PIN          GPIO_PIN_0     /* rank 2, ch 10 */
#define PIN_ADC_LCT_PORT        GPIOC
#define PIN_ADC_LCT_PIN         GPIO_PIN_1     /* rank 3, ch 11 */
#define PIN_ADC_CP_PORT         GPIOA
#define PIN_ADC_CP_PIN          GPIO_PIN_4     /* rank 4, ch 4  */
#define PIN_ADC_CC_PORT         GPIOA
#define PIN_ADC_CC_PIN          GPIO_PIN_7     /* rank 5, ch 7  */
#define PIN_ADC_PE_PORT         GPIOC
#define PIN_ADC_PE_PIN          GPIO_PIN_5     /* rank 6, ch 15 */
/* PB0: still labelled NTC2 in pin map. A 2026-05-03 morning probe
 * briefly suggested PB0 was the contactor closed-feedback (jumped
 * 0 → 524 raw on PE12 toggle), but a more thorough afternoon
 * sequence showed PB0 stays in the 565-686 raw range across ALL
 * combinations of PE12 + PB12 / open + closed contactor. Likely an
 * AC-mains-presence sense (rectified L1 upstream of the contactor),
 * NOT relay-state. There is no known closed-feedback sense on this
 * board — weld / stuck-open detection is currently blind, gated off
 * via OPENBHZD_RELAY_FEEDBACK_KNOWN until a real sense is found. */
#define PIN_ADC_NTC2_PORT       GPIOB
#define PIN_ADC_NTC2_PIN        GPIO_PIN_0     /* rank 7, ch 8; AC-presence */
#define PIN_ADC_UNUSED_PORT     GPIOB
#define PIN_ADC_UNUSED_PIN      GPIO_PIN_1     /* rank 8, ch 9  */
#define PIN_ADC_BTN_PORT        GPIOC
#define PIN_ADC_BTN_PIN         GPIO_PIN_3     /* rank 9, ch 13 */
                                /* rank 10 = VREFINT (no GPIO)  */

/* ----- Configured-but-unused analog inputs (high-Z) ----- */
#define PIN_ADC_NC0_PORT        GPIOA
#define PIN_ADC_NC0_PIN         GPIO_PIN_0
#define PIN_ADC_NC1_PORT        GPIOA
#define PIN_ADC_NC1_PIN         GPIO_PIN_1
#define PIN_ADC_NC2_PORT        GPIOC
#define PIN_ADC_NC2_PIN         GPIO_PIN_2

/* ----- Power switching outputs (single-writer = safety_task; M2 init only) ----- */
#define PIN_RELAY_MAIN_PORT     GPIOE
#define PIN_RELAY_MAIN_PIN      GPIO_PIN_12    /* DPDT main contactor */
#define PIN_RELAY_AUX_PORT      GPIOE
#define PIN_RELAY_AUX_PIN       GPIO_PIN_0     /* aux SPST */

/* PB12: HARDWARE SAFETY-OPEN LATCH OUTPUT. Bench-confirmed
 * 2026-05-03 (afternoon sequence):
 *   - Idle LOW: pilot relay open, no effect on main contactor.
 *   - Driven HIGH while PE12 HIGH (contactor closed): forces the
 *     main contactor open via a hardware latch (LOUD release click).
 *   - LOW again does NOT re-close — the latch must be reset by
 *     dropping PE12 LOW first, then PE12 HIGH again.
 * This is a UL2231-style redundant force-open path: any fault
 * condition that asserts PB12 will hardware-latch the contactor open
 * even if the PE12 driver has stuck HIGH. OpenBHZD currently leaves
 * PB12 LOW (don't assert) but configures it as output PP; future
 * fault paths can assert it for redundant open. Stock firmware READS
 * PB12 too (bidirectional / transistor open-collector sense), which
 * is why driving it externally during probing fired the alarm. */
#define PIN_RELAY_FORCE_OPEN_PORT  GPIOB
#define PIN_RELAY_FORCE_OPEN_PIN   GPIO_PIN_12

/* ----- GFCI ----- */
#define PIN_GFCI_CAL_PORT       GPIOE
#define PIN_GFCI_CAL_PIN        GPIO_PIN_3     /* active-low at MCU; idle low (CAL inactive) */

/* ----- Buzzer ----- */
#define PIN_BUZZER_PORT         GPIOB
#define PIN_BUZZER_PIN          GPIO_PIN_2

/* ----- W25Q SPI3 ----- */
#define PIN_W25Q_SCK_PORT       GPIOB
#define PIN_W25Q_SCK_PIN        GPIO_PIN_3     /* AF push-pull */
#define PIN_W25Q_MISO_PORT      GPIOB
#define PIN_W25Q_MISO_PIN       GPIO_PIN_4     /* input float */
#define PIN_W25Q_MOSI_PORT      GPIOB
#define PIN_W25Q_MOSI_PIN       GPIO_PIN_5     /* AF push-pull */
#define PIN_W25Q_CS_PORT        GPIOB
#define PIN_W25Q_CS_PIN         GPIO_PIN_6     /* GPIO push-pull, idle HIGH (deasserted) */

/* ----- U11 PGA gain-select bits ----- */
#define PIN_U11_G0_PORT         GPIOB
#define PIN_U11_G0_PIN          GPIO_PIN_9
#define PIN_U11_G1_PORT         GPIOD
#define PIN_U11_G1_PIN          GPIO_PIN_15

/* ----- FC41D Wi-Fi/BLE control (held OFF in M2) ----- */
#define PIN_FC41D_VEN_PORT      GPIOE
#define PIN_FC41D_VEN_PIN       GPIO_PIN_1     /* supply enable; idle 0 = module off */
#define PIN_FC41D_CEN_PORT      GPIOD
#define PIN_FC41D_CEN_PIN       GPIO_PIN_0     /* chip-enable; idle 0 */
#define PIN_FC41D_WAKE_PORT     GPIOD
#define PIN_FC41D_WAKE_PIN      GPIO_PIN_1     /* wake-up out; idle 0 */

/* ----- J1772 PWM (configured AF in M2; TIM1 idle until M3) ----- */
#define PIN_CP_PWM_PORT         GPIOE
#define PIN_CP_PWM_PIN          GPIO_PIN_13    /* TIM1_CH3 full-remap; AF push-pull */

/* ----- WS2812 DIN (left as input float in M2; M9 owns it) ----- */
#define PIN_WS2812_PORT         GPIOA
#define PIN_WS2812_PIN          GPIO_PIN_15

/* ----- On-board button + DIP switches ----- */
#define PIN_BTN_PC9_PORT        GPIOC
#define PIN_BTN_PC9_PIN         GPIO_PIN_9     /* input pull-up, active-low */
#define PIN_DIP1_PORT           GPIOD
#define PIN_DIP1_PIN            GPIO_PIN_13
#define PIN_DIP2_PORT           GPIOD
#define PIN_DIP2_PIN            GPIO_PIN_12
#define PIN_DIP3_PORT           GPIOD
#define PIN_DIP3_PIN            GPIO_PIN_11
#define PIN_DIP4_PORT           GPIOD
#define PIN_DIP4_PIN            GPIO_PIN_10

/* ----- Misc straps to log at boot ----- */
#define PIN_STRAP_PB7_PORT      GPIOB
#define PIN_STRAP_PB7_PIN       GPIO_PIN_7
#define PIN_STRAP_PB8_PORT      GPIOB
#define PIN_STRAP_PB8_PIN       GPIO_PIN_8
#define PIN_STRAP_PE2_PORT      GPIOE
#define PIN_STRAP_PE2_PIN       GPIO_PIN_2
#define PIN_STRAP_PB14_PORT     GPIOB
#define PIN_STRAP_PB14_PIN      GPIO_PIN_14

#endif /* OPENBHZD_CORE_PIN_MAP_H */
