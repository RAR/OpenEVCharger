#ifndef OPENEVCHARGER_BOARDS_NEXCYBER_PIN_MAP_H
#define OPENEVCHARGER_BOARDS_NEXCYBER_PIN_MAP_H

/* Canonical pin assignments for OpenEVCharger on the Nexcyber AC EVSE
 * (and the broader family of Tuya-based EVSEs sharing the same DP map
 * — see esphome/testcharger/NOTES.md "Nations N32G45x main MCU pinout"
 * for the underlying static-analysis source).
 *
 * Source of truth: esphome/testcharger/NOTES.md, derived from a
 * 2026-05-07 SWD dump (`stock-mcu-2026-05-07.bin`, sha256
 * d1d9c2e5a9d6c1e5f770390c26c302855940cdec69acdf60d356dac62d0dda00).
 * 27 of ~36 usable pins are nailed via firmware static analysis;
 * the remaining ~9 OUT_PP pins feed a ULN2003 Darlington driver
 * and need a mains-on bench wiggle to identify their loads.
 *
 * BOARD-SPECIFIC. Mirrors the convention in
 * src/core/pin_map.h (Rippleon) — selected at build time via
 * -DOPENEVCHARGER_BOARD=nexcyber.
 *
 *   PIN_<role>_PORT   -> GPIOx peripheral handle (Nations naming)
 *   PIN_<role>_PIN    -> GPIO_PIN_n
 *   PIN_<role>_RCC    -> RCC_APB2_PERIPH_GPIOx (Nations clock-enable bit)
 *
 * NOTE on naming difference vs. the rippleon header: the GD32F20x SPL
 * spells the clock-enable token `RCU_GPIOx`; Nations N32G45x SPL
 * spells it `RCC_APB2_PERIPH_GPIOx`. Where role labels map across
 * boards (e.g. PIN_HEARTBEAT, PIN_LOG_UART_*), we keep the same
 * `PIN_<role>` name even though the per-pin token differs — the
 * board-specific HAL implementations consume them.
 */

#include "n32g45x.h"

/* =========================================================================
 *  Communication interfaces (from firmware static analysis — confirmed)
 * =========================================================================
 */

/* USART1 (PA9 TX / PA10 RX) — Tuya WBR2 Wi-Fi/BLE module link, 115200 8N1
 * (Tuya MCU protocol; per esphome/testcharger/testcharger.yaml). The
 * stock fw uses 9600 by default; ESPHome side runs at 115200 once we
 * reflash the WBR2 with our LibreTiny-based image. */
#define PIN_WBR2_TX_PORT        GPIOA
#define PIN_WBR2_TX_PIN         GPIO_PIN_9
#define PIN_WBR2_RX_PORT        GPIOA
#define PIN_WBR2_RX_PIN         GPIO_PIN_10
#define PIN_WBR2_RCC            RCC_APB2_PERIPH_GPIOA

/* USART2 (PA2 TX / PA3 RX) — Nextion HMI display.
 * Nextion command syntax: `page X⏎`, `t0.txt="..."⏎`, `t0.pic=N⏎`,
 * each frame terminated `\xFF\xFF\xFF`. Page strings observed in the
 * firmware: `setting`, `nogun`, `chargeing`, `waittime`. */
#define PIN_NEXTION_TX_PORT     GPIOA
#define PIN_NEXTION_TX_PIN      GPIO_PIN_2
#define PIN_NEXTION_RX_PORT     GPIOA
#define PIN_NEXTION_RX_PIN      GPIO_PIN_3
#define PIN_NEXTION_RCC         RCC_APB2_PERIPH_GPIOA

/* USART3 (PB10 TX / PB11 RX) — TBD.
 * 8 literal-pool refs (heaviest of the three USARTs in the firmware).
 * Strongest hypotheses:
 *   - BL0939 metering chip on the UART variant (4800 8N1, single-wire
 *     TX from the chip with continuous frames). SPI2 below is also
 *     wired up, so this could be a dual-link arrangement.
 *   - Stock-firmware debug log.
 * TODO bench: scope PB11 during stock boot — if continuous 4800 baud
 * stream → BL0939; if bursty / no traffic → debug log.
 */
#define PIN_USART3_TX_PORT      GPIOB
#define PIN_USART3_TX_PIN       GPIO_PIN_10
#define PIN_USART3_RX_PORT      GPIOB
#define PIN_USART3_RX_PIN       GPIO_PIN_11
#define PIN_USART3_RCC          RCC_APB2_PERIPH_GPIOB

/* SPI2 (PB12-15) — fully configured in the stock firmware, very likely
 * the BL0939 metering link (SPI variant, ≤ 900 kHz). NSS is software-
 * driven (PB12 OUT_PP rather than AF). Confirms in Phase 2 by scoping
 * SCK at boot. */
#define PIN_BL0939_NSS_PORT     GPIOB
#define PIN_BL0939_NSS_PIN      GPIO_PIN_12     /* SW chip-select */
#define PIN_BL0939_SCK_PORT     GPIOB
#define PIN_BL0939_SCK_PIN      GPIO_PIN_13     /* AF push-pull */
#define PIN_BL0939_MISO_PORT    GPIOB
#define PIN_BL0939_MISO_PIN     GPIO_PIN_14     /* AF input */
#define PIN_BL0939_MOSI_PORT    GPIOB
#define PIN_BL0939_MOSI_PIN     GPIO_PIN_15     /* AF push-pull */
#define PIN_BL0939_RCC          RCC_APB2_PERIPH_GPIOB

/* =========================================================================
 *  J1772 control pilot — TIM1_CH1 PWM out
 * =========================================================================
 */

/* PA8 = TIM1_CH1 default-AF (matches both STM32F1 and N32G45x AF maps).
 * 1 kHz, 10% / 33% / etc duty per J1772; ±12 V swing handled by external
 * level-shifting / op-amp stages.
 * Naming kept consistent with rippleon's PIN_CP_PWM_*. */
#define PIN_CP_PWM_PORT         GPIOA
#define PIN_CP_PWM_PIN          GPIO_PIN_8
#define PIN_CP_PWM_RCC          RCC_APB2_PERIPH_GPIOA

/* =========================================================================
 *  ADC analog inputs (5 channels — confirmed configured AIN in stock fw)
 * =========================================================================
 *
 * Channel-to-role mapping requires bench correlation; the firmware uses
 * the ADC raw value through several float-cal stages before storing
 * into the SRAM cache at 0x2000075c (CP voltage, mV scaled). Working
 * hypothesis from visual board ID + topology:
 *   - PB1 / PB2  → 2× ZMPT107-1 split-phase voltage sense (L1 + L2)
 *                  via LM2904 op-amp amplifier stages
 *   - PC0        → CP (J1772 pilot voltage, level-shifted ±12 V → 0-3.3 V)
 *   - PC4        → CC (J1772 proximity / cable amperage code)
 *   - PC5        → NTC near gun connector
 *
 * The "CP cache at 0x2000075c" is the strongest signal we have for
 * which channel is CP — once Phase 2 has the ADC HAL up, dump the
 * per-channel raw value while idle (CP @ +12V, no plug) and pick the
 * channel reading ~ +12 V scaled to count.
 */
#define PIN_ADC_VSENSE_L1_PORT  GPIOB
#define PIN_ADC_VSENSE_L1_PIN   GPIO_PIN_1      /* ADC12_IN9 default; hypothesis */
#define PIN_ADC_VSENSE_L2_PORT  GPIOB
#define PIN_ADC_VSENSE_L2_PIN   GPIO_PIN_2      /* Nations-routed ADC channel */
#define PIN_ADC_CP_PORT         GPIOC
#define PIN_ADC_CP_PIN          GPIO_PIN_0      /* ADC12_IN10 default */
#define PIN_ADC_CC_PORT         GPIOC
#define PIN_ADC_CC_PIN          GPIO_PIN_4      /* ADC12_IN14 default */
#define PIN_ADC_NTC_PORT        GPIOC
#define PIN_ADC_NTC_PIN         GPIO_PIN_5      /* ADC12_IN15 default */
#define PIN_ADC_RCC_AB          (RCC_APB2_PERIPH_GPIOB)
#define PIN_ADC_RCC_C           (RCC_APB2_PERIPH_GPIOC)

/* =========================================================================
 *  Digital inputs — confirmed from init dumps
 * =========================================================================
 */

/* PA11 / PA12 — capacitive-touch front panel, 2 buttons (TTP223-style ICs).
 * Idle HIGH (no touch); pulled LOW when the user taps. */
#define PIN_BTN_TOUCH1_PORT     GPIOA
#define PIN_BTN_TOUCH1_PIN      GPIO_PIN_11     /* IN_PU, active-low */
#define PIN_BTN_TOUCH2_PORT     GPIOA
#define PIN_BTN_TOUCH2_PIN      GPIO_PIN_12     /* IN_PU, active-low */

/* PC13 — likely GFCI fault sense (active-low). Idle LOW on the bench
 * (mains off / GFCI module not yet active), expected to go HIGH at
 * idle once mains is up and the GFCI module is in normal-not-tripped
 * state. TODO bench mains-on confirmation. */
#define PIN_GFCI_SENSE_PORT     GPIOC
#define PIN_GFCI_SENSE_PIN      GPIO_PIN_13

/* PC3 / PC7 / PC9 — three digital inputs from the static dump
 * (PC3 IN_FLOATING, PC7 IN_PD, PC9 IN_PD). Topology suggests two of
 * these carry the TLP293-2 photocoupler outputs for L1/L2 mains-
 * presence detection (60 Hz pulse stream when live). The third may
 * be a status / detect line for the GFCI module. TODO bench mains-on:
 * scope each for 60 Hz toggle activity. */
#define PIN_MAINS_DETECT_A_PORT GPIOC
#define PIN_MAINS_DETECT_A_PIN  GPIO_PIN_3      /* IN_FLOATING — candidate L1 */
#define PIN_MAINS_DETECT_B_PORT GPIOC
#define PIN_MAINS_DETECT_B_PIN  GPIO_PIN_7      /* IN_PD — candidate L2 */
#define PIN_MAINS_DETECT_C_PORT GPIOC
#define PIN_MAINS_DETECT_C_PIN  GPIO_PIN_9      /* IN_PD — extra digital sense */

/* =========================================================================
 *  Confirmed digital outputs
 * =========================================================================
 */

/* PC6 — beeper (confirmed 2026-05-07 SWD pin-wiggle: HIGH → audible
 * tone on bench at 5 V rail). Driven directly, not via PWM (no TIM
 * AF on PC6 in the static decode). */
#define PIN_BUZZER_PORT         GPIOC
#define PIN_BUZZER_PIN          GPIO_PIN_6

/* PC2 — alt-function output (AF_PP/50M), but no peripheral base address
 * referenced. Vendor-remapped peripheral or unused AF. TODO firmware
 * dynamic-trace: read AFIO_MAPR at runtime to resolve.
 */
#define PIN_PC2_AF_PORT         GPIOC
#define PIN_PC2_AF_PIN          GPIO_PIN_2

/* PC12 — initially configured OUT_PP, then re-init'd to AF at runtime.
 * STM32F1 default AF on PC12 is UART5_TX, but UART5 base never appears
 * in the literal pool, so this is a remap target. TODO trace AFIO_MAPR. */
#define PIN_PC12_AF_PORT        GPIOC
#define PIN_PC12_AF_PIN         GPIO_PIN_12

/* PA15 — freed by AFIO SWJ_CFG=010 (SWD only, JTAG off). Likely a status
 * LED or a relay-drive into the ULN2003. Wiggle game silent on 5 V rail
 * (no load activates without 12 V mains). */
#define PIN_PA15_OUT_PORT       GPIOA
#define PIN_PA15_OUT_PIN        GPIO_PIN_15

/* =========================================================================
 *  ULN2003-driven outputs — TODO mains-on wiggle (9 silent pins)
 * =========================================================================
 *
 * These pins are confirmed OUT_PP from the static dump, but stayed
 * silent during the 2026-05-07 bench wiggle because the on-board 12 V
 * rail isn't up without AC mains attached. Each one feeds an input of
 * the ULN2003 Darlington driver array; loads are some mix of:
 *
 *   - 2× main contactor coils (75 A / 1000 VAC, double-pole break)
 *   - 1× small relay (almost certainly the GFCI self-test pulse coil)
 *   - status / panel LEDs
 *
 * Mains-on procedure (deferred): `cd boards/nexcyber/tools && ...`
 * once bench tooling lands. Listen for relay clicks / watch panel LEDs.
 *
 * Group these as PIN_OUTx for now; rename to semantic names
 * (PIN_RELAY_L1, PIN_RELAY_L2, PIN_GFCI_CAL, etc.) once the mains-on
 * wiggle resolves load-to-pin.
 */
#define PIN_OUT_A0_PORT         GPIOA
#define PIN_OUT_A0_PIN          GPIO_PIN_0      /* TODO bench-resolve */
#define PIN_OUT_A1_PORT         GPIOA
#define PIN_OUT_A1_PIN          GPIO_PIN_1      /* TODO bench-resolve */
#define PIN_OUT_B0_PORT         GPIOB
#define PIN_OUT_B0_PIN          GPIO_PIN_0      /* TODO bench-resolve */
#define PIN_OUT_B7_PORT         GPIOB
#define PIN_OUT_B7_PIN          GPIO_PIN_7      /* TODO bench-resolve (slow rate, OUT_PP/2M) */
#define PIN_OUT_B8_PORT         GPIOB
#define PIN_OUT_B8_PIN          GPIO_PIN_8      /* TODO bench-resolve */
#define PIN_OUT_B9_PORT         GPIOB
#define PIN_OUT_B9_PIN          GPIO_PIN_9      /* TODO bench-resolve */
#define PIN_OUT_C8_PORT         GPIOC
#define PIN_OUT_C8_PIN          GPIO_PIN_8      /* TODO bench-resolve */
#define PIN_OUT_C10_PORT        GPIOC
#define PIN_OUT_C10_PIN         GPIO_PIN_10     /* TODO bench-resolve */
#define PIN_OUT_C11_PORT        GPIOC
#define PIN_OUT_C11_PIN         GPIO_PIN_11     /* TODO bench-resolve */

/* =========================================================================
 *  Unused / not in init
 * =========================================================================
 *
 * GPIO ports D, E, F, G are entirely unreferenced in the stock firmware
 * (no init calls, no read/write sites). Many of those pins are
 * physically n/c on the Nexcyber PCB; the rest are reserved for
 * future expansion.
 *
 * SWD pins (PA13 SWDIO, PA14 SWCLK) are preserved by AFIO SWJ_CFG=010
 * (SWD-only, JTAG off). PA15 is freed by that same remap.
 */

#endif /* OPENEVCHARGER_BOARDS_NEXCYBER_PIN_MAP_H */
