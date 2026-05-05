#ifndef OPENEVCHARGER_CORE_PIN_MAP_H
#define OPENEVCHARGER_CORE_PIN_MAP_H

/* Canonical pin assignments for OpenEVCharger on the Rippleon ROC001 board.
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

/* ----- Debug UART: UART3 (SPL macro, = STM32 UART4) on default pins
 * PC10 TX / PC11 RX, 115200 8N1.
 *
 * Moved here 2026-05-04 from USART1 remap PD5/PD6: those pins are the
 * stock-fw RFID/NFC reader port (silk-screened on the PCB header)
 * and the bench-validated protocol (see docs/mcu-re/rfid-protocol.md)
 * needs the MCU to send a keepalive at ~3 Hz on PD5 for the module
 * to talk back.
 *
 * PC10/PC11 are the stock-fw DWIN DGUS LCD HMI pads (4-pin connector
 * exposed on the PCB) — physically accessible without poking inside,
 * and the bench unit has no LCD attached so the pins are free. */
#define PIN_LOG_UART_TX_PORT    GPIOC
#define PIN_LOG_UART_TX_PIN     GPIO_PIN_10
#define PIN_LOG_UART_RX_PORT    GPIOC
#define PIN_LOG_UART_RX_PIN     GPIO_PIN_11
#define PIN_LOG_UART_RCU        RCU_GPIOC

/* ----- RFID reader port: USART1 remap PD5 TX / PD6 RX, 115200 8N1
 * full-duplex. Reserved for the upcoming RFID HAL — see
 * docs/mcu-re/rfid-protocol.md. */
#define PIN_RFID_TX_PORT        GPIOD
#define PIN_RFID_TX_PIN         GPIO_PIN_5
#define PIN_RFID_RX_PORT        GPIOD
#define PIN_RFID_RX_PIN         GPIO_PIN_6
#define PIN_RFID_RCU            RCU_GPIOD

/* ----- ADC analog inputs (rank order matches DMA buffer layout) -----
 *
 * Channel-role correction 2026-05-03 evening: bench experiment
 * (grounding the gun-side connector NTC pin) confirmed PA2 is the
 * GUN-cable thermistor, not mains AC voltage. Names PIN_ADC_AC_*
 * are kept for now (wide blast radius — ADC_RANK_AC is referenced
 * across safety_task / boot self-test / FC41D parse offsets) but
 * the SEMANTIC role per the OEM's intent is:
 *   PA2  (rank 0, "AC")    = gun-cable NTC thermistor (populated,
 *                             10 kΩ, β≈3380 working assumption)
 *   PA3  (rank 1, "NTC1")  = wall-plug NTC thermistor (populated)
 *   PB0  (rank 7, "NTC2")  = unknown / probably AC-mains-presence
 *                             sense (reads 565..686 raw with mains;
 *                             stays in that band regardless of relay
 *                             state — see relay.h note). NOT a
 *                             thermistor; OPENEVCHARGER_NTC2_PRESENT=0.
 * Earlier "mains voltage from PA2" calibration (V/count=0.06151
 * against a Fluke @ 123.9 V) was coincidence — the unpopulated NTC
 * pin happened to float-rail near 1.7 V which scaled to a plausible
 * mains number. With the cable plugged in, PA2 follows the gun NTC
 * divider (R_pull=10 kΩ ↔ R_ntc on front-block "NTC" pin). */
#define PIN_ADC_AC_PORT         GPIOA
#define PIN_ADC_AC_PIN          GPIO_PIN_2     /* rank 0, ch 2 — GUN NTC */
#define PIN_ADC_NTC1_PORT       GPIOA
#define PIN_ADC_NTC1_PIN        GPIO_PIN_3     /* rank 1, ch 3 — WALL-PLUG NTC */
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
/* PB0: name "NTC2" is a misnomer carried over from early bring-up.
 * Bench experiments rule out both interpretations originally tried:
 *   - NOT a thermistor: bench-grounding the gun-block NTC pins zeros
 *     PA2 / PA3, NOT PB0 (those are the real two thermistor channels).
 *   - NOT contactor closed-feedback: a 2026-05-03 morning probe
 *     showed a jump 0 → 524 on PE12 toggle, but a more thorough
 *     afternoon sequence showed PB0 stays 565..686 raw across ALL
 *     combinations of PE12 + PB12 / open + closed contactor.
 * Most likely an AC-mains-presence sense (rectified L1 upstream of
 * the contactor). Closed-feedback is therefore still UNKNOWN; weld /
 * stuck-open detection is gated off via OPENEVCHARGER_RELAY_FEEDBACK_KNOWN
 * until the real sense pin is found. */
#define PIN_ADC_NTC2_PORT       GPIOB
#define PIN_ADC_NTC2_PIN        GPIO_PIN_0     /* rank 7, ch 8 — AC-presence (likely) */
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
 * even if the PE12 driver has stuck HIGH. OpenEVCharger currently leaves
 * PB12 LOW (don't assert) but configures it as output PP; future
 * fault paths can assert it for redundant open. Stock firmware READS
 * PB12 too (bidirectional / transistor open-collector sense), which
 * is why driving it externally during probing fired the alarm. */
#define PIN_RELAY_FORCE_OPEN_PORT  GPIOB
#define PIN_RELAY_FORCE_OPEN_PIN   GPIO_PIN_12

/* ----- GFCI / BL0939 fault sense -----
 *
 * PE2 = BL0939 fault output (= GFCI fault sense). Bench-confirmed
 * 2026-05-04 by driving the BL0939's trip line HIGH externally and
 * watching PE2 toggle via tools/gpio_diff.sh (PE2: 1 → 0 on assert,
 * returns 1 on release). Polarity is ACTIVE-LOW at the MCU side:
 * idle HIGH, pulls LOW when the BL0939 's fault output asserts.
 *
 * The "GFCI module" we'd been treating as a discrete chip is
 * actually U11 = BL0939 (identified 2026-05-04). The same chip
 * does V/I metering (over UART) AND differential-current detection
 * (this fault output). PE3 → CAL is the BL0939's self-test input.
 *
 * This INVERTS the agent's static-decode hypothesis (which assumed
 * active-high based on the bit-set ORing semantics in stock fw's
 * polled state machine at 0x08012824 — see docs/re-stock-safety.md).
 * Most likely the stock fw's "bit 1 set" means "PE2 stayed HIGH" =
 * "no fault" rather than "fault asserted". Polled, no EXTI; ~5 s
 * self-test cycle. Detector should be:
 *   - PE2 input pull-up (or float — module pulls it high in normal op)
 *   - Read at safety_task tick; debounce a few ticks
 *   - PE2 LOW sustained → raise FAULT_GFCI (latched, power-cycle clear)
 *
 * PD6 (USART1-RX printk line) also moved during the bench wiggle —
 * almost certainly capacitive coupling from the trip wire running
 * adjacent on the bench harness, not a real second signal.
 *
 * PE3 = GFCI CAL output. Wire-traced 2026-05-02: MCU LOW → external
 * level-shift transistor drives 5 V to the module's CAL input
 * (active asserted); MCU HIGH → CAL idle. Stock fw drives PE3 in
 * its 8-state self-test cycle; OpenEVCharger will need to do the same
 * once we wire `hal/gfci.c`.
 *
 * PE4 also driven in the stock state machine (probably "test latch"
 * or pre-charge bleed); role not yet decoded. */
#define PIN_GFCI_CAL_PORT       GPIOE
#define PIN_GFCI_CAL_PIN        GPIO_PIN_3     /* active-low at MCU; idle low (CAL inactive) */
#define PIN_GFCI_SENSE_PORT     GPIOE
#define PIN_GFCI_SENSE_PIN      GPIO_PIN_2     /* polled, ACTIVE-LOW (bench-confirmed 2026-05-04) */

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

/* ----- U11 = BL0939 metering IC (Shanghai Belling) -----
 *
 * Visually-confirmed 2026-05-04: U11 is a Shanghai Belling BL0939
 * (SOP16L). Single-phase metering with two current channels + one
 * voltage channel, internal calibration. Has both UART (4800 bps)
 * and SPI (900 kHz) interfaces, plus a hardware fault output for
 * differential-current (RCD) detection.
 *
 * MCU ↔ BL0939 pin map (bench-confirmed 2026-05-04 via pin wiggle
 * + reverse-direction gpio_diff probe):
 *   PB9   →  BL0939 pin 13 (SCLK, SPI clock)
 *   PD15  →  BL0939 pin 14 (RX/SDI, SPI data MCU→BL0939)
 *   PD14  ←  BL0939 pin 15 (TX/SDO, SPI data BL0939→MCU)
 *   PE2   ←  BL0939 pin 10 (I_leak, RCD/leak alarm — active-low)
 *   PE3   →  external CAL-injection transistor (GFCI self-test)
 *
 * Comm mode: **SPI** (not UART). SEL pin is hardwired (likely
 * VDD) since none of the traced MCU pins connect to SEL.
 * GD32F205's SPI peripheral doesn't natively map to PB9+PD15+PD14
 * (no AF), so this is BIT-BANGED SPI at ≤ 900 kHz from the MCU
 * side. PD14 (SDO input) needs an external pull-up per the
 * datasheet — likely already on the OEM PCB. */
#define PIN_U11_G0_PORT         GPIOB
#define PIN_U11_G0_PIN          GPIO_PIN_9
#define PIN_U11_G1_PORT         GPIOD
#define PIN_U11_G1_PIN          GPIO_PIN_15

#define PIN_BL0939_SCLK_PORT    GPIOB
#define PIN_BL0939_SCLK_PIN     GPIO_PIN_9      /* MCU SPI clock → BL0939 pin 13 */
#define PIN_BL0939_SDI_PORT     GPIOD
#define PIN_BL0939_SDI_PIN      GPIO_PIN_15     /* MCU data → BL0939 pin 14 (RX/SDI) */
#define PIN_BL0939_SDO_PORT     GPIOD
#define PIN_BL0939_SDO_PIN      GPIO_PIN_14     /* MCU data ← BL0939 pin 15 (TX/SDO) */

/* ----- FC41D Wi-Fi/BLE control (held OFF in M2) ----- */
#define PIN_FC41D_VEN_PORT      GPIOE
#define PIN_FC41D_VEN_PIN       GPIO_PIN_1     /* supply enable; idle 0 = module off */
#define PIN_FC41D_CEN_PORT      GPIOD
#define PIN_FC41D_CEN_PIN       GPIO_PIN_0     /* chip-enable; idle 0 */
#define PIN_FC41D_WAKE_PORT     GPIOD
#define PIN_FC41D_WAKE_PIN      GPIO_PIN_1     /* wake-up out; idle 0 */

/* ----- UART4 (= "UART5" per spec § 5 / pinout doc) — FC41D TLV link ----- */
#define PIN_UART4_TX_PORT       GPIOC
#define PIN_UART4_TX_PIN        GPIO_PIN_12    /* AF push-pull */
#define PIN_UART4_RX_PORT       GPIOD
#define PIN_UART4_RX_PIN        GPIO_PIN_2     /* input float */
#define PIN_UART4_TX_RCU        RCU_GPIOC
#define PIN_UART4_RX_RCU        RCU_GPIOD

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

#endif /* OPENEVCHARGER_CORE_PIN_MAP_H */
