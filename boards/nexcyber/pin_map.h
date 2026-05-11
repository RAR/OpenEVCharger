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
 *
 * Additional structure observed 2026-05-09 with mains on (live SWD
 * snapshot of stock firmware):
 *   0x20000700-0x20000744: 40-halfword circular buffer for one ADC
 *                           channel (RMS / waveform analysis ring).
 *                           Reads near 0x0ff8 (ADC rail) on bench →
 *                           consistent with the L2-side ZMPT107
 *                           floating to op-amp rail because L2 is
 *                           dead on US 120 V single-leg supply.
 *   0x20000748-0x20000754: 4-channel scan cache, 6 halfwords. Most
 *                           recent sample per channel.
 *   0x2000075c            : CP voltage cache, signed int32 mV
 *                           (was uint16_t in the earlier memory note;
 *                           SRAM dump shows it's actually 32-bit
 *                           signed — bench reads -300 mV with no plug,
 *                           which makes sense as a near-zero CP).
 *   0x20000760-0x2000076C: other calibrated caches (energy counter
 *                           candidate at 0x2000076C: 0x12000000).
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

/* PC13 — E-stop loop sense (active-low, NC switch). Confirmed
 * 2026-05-11 via three-snapshot SWD sweep on the bench:
 *   - Switch closed (normal / not pressed) → PC13 HIGH
 *   - Switch open (E-stop pressed OR wire pulled) → PC13 LOW
 * The hard-wired E-stop button on the bench unit sits in series
 * across this pin; when the user temporarily jumpered the switch
 * out, PC13 sat HIGH continuously. Original "GFCI fault sense"
 * hypothesis was wrong — the GFCI fault path appears to be the
 * separate ADC trace seen as G1 in the telemetry dump (raw ~2195
 * = mid-rail = healthy at the bench, no dedicated digital sense pin). */
#define PIN_STOP_SENSE_PORT     GPIOC
#define PIN_STOP_SENSE_PIN      GPIO_PIN_13

/* PC3 / PC7 — two digital inputs from the static dump (PC3 IN_FLOATING,
 * PC7 IN_PD). Topology suggests at least one of these carries a
 * TLP293-2 photocoupler output for mains-presence detection
 * (60 Hz pulse stream when live), or a GFCI-module status line.
 * TODO bench mains-on: scope each for 60 Hz toggle activity. */
#define PIN_MAINS_DETECT_A_PORT GPIOC
#define PIN_MAINS_DETECT_A_PIN  GPIO_PIN_3      /* IN_FLOATING — candidate L1 */
#define PIN_MAINS_DETECT_B_PORT GPIOC
#define PIN_MAINS_DETECT_B_PIN  GPIO_PIN_7      /* IN_PD — candidate L2 */

/* PC9 — front-panel "tiny button" (IN_PD, active-HIGH). Bench-confirmed
 * 2026-05-11 via snapshot diff: idle PC9 IDR=0; held-down PC9 IDR=1.
 * Pull-down internal, button connects to +3.3 V when pressed.
 * Originally hypothesised as a "mains-detect C" photocoupler — turned
 * out to be a button instead.
 *
 * Likely roles in stock fw: pairing / WiFi-reset / factory reset.
 * Watch for a related LED flash on a separate pin during press —
 * snapshot diff caught PA15 IDR briefly HIGH during the same press,
 * making PA15 a strong candidate for the button's status LED. */
#define PIN_BUTTON_PORT         GPIOC
#define PIN_BUTTON_PIN          GPIO_PIN_9

/* =========================================================================
 *  Confirmed digital outputs
 * =========================================================================
 */

/* PC6 — beeper (confirmed 2026-05-07 SWD pin-wiggle: HIGH → audible
 * tone on bench at 5 V rail). Driven directly, not via PWM (no TIM
 * AF on PC6 in the static decode). */
#define PIN_BUZZER_PORT         GPIOC
#define PIN_BUZZER_PIN          GPIO_PIN_6

/* PC11 — safety-loop-driven enable (revised 2026-05-11). Original
 * 2026-05-09 finding was "always HIGH at idle" — that turned out to
 * be partial: a 2026-05-11 three-snapshot sweep showed PC11 tracks
 * the E-stop loop's HARD-WIRED state, not just the latched fault
 * state machine:
 *   - STOP switch physically in circuit + closed → PC11 HIGH
 *   - STOP switch JUMPERED out (clean short bypass) → PC11 LOW
 *   - Either case keeps `fault_bitmap = 8` latched (the L2-missing
 *     fault on the bench is decoupled from PC11)
 *
 * So the firmware drives PC11 only when it "sees" the real safety
 * loop is wired through, not just a logical close. Candidate
 * downstream roles, in rough likelihood order:
 *   - GFCI module enable (the chip won't run its detection unless
 *     the upstream safety loop is hardwired)
 *   - Gun-lock relay coil (interlock that requires safety-loop OK)
 *   - 12 V rail to a downstream peripheral that's gated on the loop
 *
 * NOT the contactor permit — that's gated on `fault_bitmap == 0`
 * (or equivalent), and the bench saw PC11 HIGH with bit-3 still set.
 *
 * Next-bench step: with the MCU halted, force PC11 LOW (BRR) /
 * HIGH (BSRR) and watch/listen for a click / LED change / Nextion
 * UI change to pin down which downstream consumer it is. */
#define PIN_SAFETY_LOOP_EN_PORT GPIOC
#define PIN_SAFETY_LOOP_EN_PIN  GPIO_PIN_11

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
 *  GFCI CAL (small relay, mains-on confirmed 2026-05-09)
 * =========================================================================
 *
 * Bench-confirmed 2026-05-09 with 120 V (single-leg) mains attached:
 * pulsing PB0 HIGH for 500 ms produced an audible click from the
 * small relay near the NTC / CP connector — the GFCI self-test
 * pulse relay, exactly as hypothesized in NOTES.md. ULN2003 input
 * 0 → relay coil drive.
 *
 * Drive strategy (mirrors the rippleon BL0939 CAL pattern, see
 * src/hal/gfci.c): assert briefly during boot self-test, expect
 * GFCI sense to trip in response, raise FAULT_GFCI_SELF_TEST if
 * sense doesn't move. Polarity is assumed active-HIGH (HIGH at
 * MCU → ULN2003 sinks coil → relay closes); confirm during
 * Phase 3 integration with a coil-side scope.
 */
#define PIN_GFCI_CAL_PORT       GPIOB
#define PIN_GFCI_CAL_PIN        GPIO_PIN_0      /* HIGH=inject pulse */

/* =========================================================================
 *  Other OUT_PP pins — bench-silent, semantic role TBD
 * =========================================================================
 *
 * Mains-on wiggle 2026-05-09 (120 V single-leg, 12 V rail confirmed
 * up by the PB0 click): pulsing each of the eight remaining OUT_PP
 * pins (PA0, PA1, PA15, PB8, PB9, PC8, PC10, PC11) produced no
 * externally-visible response.
 *
 * That doesn't mean they're unused — most likely candidates:
 *   - LED indicators not fitted on this bench unit (the gun NTC isn't
 *     fitted either; this PCB seems to be a stripped bench variant)
 *   - Chip-enable / control signals into the WBR2, Nextion HMI, or
 *     BL0939 (no on-board LED feedback when these toggle)
 *   - Contactor coil drives that require a redundant-drive pattern
 *     (e.g. "both pin X and pin Y must be HIGH to permit close") —
 *     stock fw doesn't assert these at idle, so individual pulses
 *     don't trigger
 *   - Split-phase L2 contactor — possibly silent on 120 V single-leg
 *     because L2 is dead and the safety supervisor refuses to close
 *     (would need 240 V split-phase to test)
 *
 * Resolution path: read SRAM ADC cache + run stock fw through a
 * simulated J1772 state-B/C transition (CC ladder + CP duty) and
 * snapshot ODR per state — pins that go HIGH during state C are
 * the contactor drives.
 */
/* PA0 — drives ONE of the two large contactors when pulsed; click-on
 * then immediate auto-open with PA1 firmware-controlled (bench wiggle
 * test 2026-05-11 with MCU halted, single PA0 toggle). Working
 * interpretation: contactor weld-detect / self-test pulse — fires one
 * coil briefly so the firmware can verify the contactor will close.
 * Same idiom as rippleon's PB0 GFCI CAL pulse. NOT the steady-state
 * contactor drive (that's PA1).
 *
 * Could alternatively be the AC-coupled-driver "tick" pin if the
 * design uses a transition-triggered driver for the main contactor.
 * Fast-wiggle test (10+ Hz on PA0 alone) would discriminate between
 * the two by observing whether the relay holds during the wiggle. */
#define PIN_CONTACTOR_TEST_PORT GPIOA
#define PIN_CONTACTOR_TEST_PIN  GPIO_PIN_0      /* candidate; bench-verified pulses one contactor */

/* PA1 — MASTER CONTACTOR PERMIT for both large relays. Bench-confirmed
 * 2026-05-11 wiggle: driving PA1 HIGH clicks BOTH large contactors
 * closed and they stay closed for the duration of the HIGH phase.
 * Plain DC drive (no PWM heartbeat needed).
 *
 * On a 240 V split-phase US EVSE, this typically gates the coil-supply
 * rail (e.g. 12 V) to both line contactors via a single high-side
 * switch / opto-isolator → both close together. NOT to be left HIGH
 * during cold-boot / fault — safety task asserts only when
 * safety_state == OK in M5+.
 *
 * NOTE: PA0 fires one coil independently as a self-test pulse;
 * the two pins together form a "permit-and-test" pattern. */
#define PIN_CONTACTOR_MAIN_PORT GPIOA
#define PIN_CONTACTOR_MAIN_PIN  GPIO_PIN_1
/* PA15 — bench-observed 2026-05-11: IDR briefly HIGH (ODR=0) during
 * a PC9 button press, then back to LOW. Most likely the button's
 * status LED — firmware flashes it on press as user feedback.
 * Confirmation needed: snapshot WHILE holding the button longer than
 * the flash duration, OR wiggle PA15 from SWD with the LED visible.
 *
 * Note: PA15 is JTAG-TDI in default AFIO_MAPR. Stock fw remaps
 * SWJ_CFG to "SWD only, JTAG off" (per pin-map analysis), freeing
 * PA15 as a normal GPIO. We'll need the same remap in M2+ before
 * driving this pin. */
#define PIN_BUTTON_LED_PORT     GPIOA
#define PIN_BUTTON_LED_PIN      GPIO_PIN_15     /* candidate; needs PA15 wiggle to confirm */
#define PIN_OUT_PB8_PORT        GPIOB
#define PIN_OUT_PB8_PIN         GPIO_PIN_8      /* TBD */
#define PIN_OUT_PB9_PORT        GPIOB
#define PIN_OUT_PB9_PIN         GPIO_PIN_9      /* TBD */
#define PIN_OUT_PC8_PORT        GPIOC
#define PIN_OUT_PC8_PIN         GPIO_PIN_8      /* TBD */
#define PIN_OUT_PC10_PORT       GPIOC
#define PIN_OUT_PC10_PIN        GPIO_PIN_10     /* TBD */

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
