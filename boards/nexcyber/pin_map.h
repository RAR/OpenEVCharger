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

/* USART3 (PB10 TX / PB11 RX) — third active serial peer; role TBD.
 *
 * DMA-confirmed 2026-05-11 pm: DMA1 channel 3 is actively configured
 * with CCR=0x30A1 (EN + CIRC + MINC + very-high priority), CNDTR=128,
 * CPAR=USART3_DR. RX ring lives at 0x200001EC. So this is NOT a
 * debug-log printk (which would have no reason to use RX-DMA); it's a
 * bidirectional peer the firmware actively listens to.
 *
 * Earlier debug-log hypothesis downgraded — the heavy literal-pool
 * footprint matches the size of a structured-protocol parser (frame
 * decode, byte queues), not a printf wrapper.
 *
 * Candidate roles (bench-blocked):
 *   - RFID reader (Wiegand serial or UART-modulated Mifare)
 *   - Internal BLE module (some Tuya designs have a separate BK3432)
 *   - LED-strip controller IC (some N32G45x boards drive a separate
 *     LED controller over UART, leaving the main MCU free of bit-bang)
 *
 * BL0939-on-USART3 ruled out: SPI2 (PB12-15) is the metering link,
 * verified by the 2026-05-11 PB14 touch-coupling test. */
#define PIN_USART3_TX_PORT      GPIOB
#define PIN_USART3_TX_PIN       GPIO_PIN_10
#define PIN_USART3_RX_PORT      GPIOB
#define PIN_USART3_RX_PIN       GPIO_PIN_11
#define PIN_USART3_RCC          RCC_APB2_PERIPH_GPIOB

/* UART4 + UART5 — Nations-remapped to 0x40015000 / 0x40015400.
 * DMA-confirmed 2026-05-11 pm via DMA2 channel dumps:
 *   - DMA2 ch1: CCR=0x30A1, CNDTR=128 B, CPAR=0x40015004, CMAR=0x2000026C
 *   - DMA2 ch6: CCR=0x30A1, CNDTR=128 B, CPAR=0x40015404, CMAR=0x200002EC
 *
 * IMPORTANT — Nations N32G45x deviates from STM32F1: F1 puts UART4 at
 * 0x40004C00 and UART5 at 0x40005000 (APB1 chunk), but stock-fw DMA
 * CPARs point to 0x40015004 and 0x40015404 — implying N32G45x relocates
 * these UARTs to a custom range. The +0x04 offset suggests USART_DR
 * sits at base+4 (matches F1 USART_DR layout). DR layout consistent
 * with the standard 5-USART peripheral set.
 *
 * Total active UARTs on this chip: 5 (USART1-3 + UART4-5). All 5 are
 * configured with circular-RX DMA. Bench-blocked: pinout-to-pad mapping
 * for UART4/UART5 (they take some PC pins per F1 default AF, but Nations
 * may have remapped those too).
 *
 * Action items for M3+:
 *   - Add UART4/UART5 peripheral handles to the HAL when needed
 *   - Reconcile RCC enable bits (Nations APB1ENR or similar) at runtime
 *   - Identify both peers (RFID? cellular? auxiliary debug?) via UART
 *     bus snooping during a stock-fw OCPP transaction
 */

/* SPI2 (PB12-15) — BL0939 metering link, SPI variant.
 * Bench-confirmed 2026-05-11: touching the data lines coupled into
 * PB14 (MISO) IDR snapshots — meaning firmware is actively running an
 * SPI2 transfer cycle reading data from a slave. BL0939's
 * UART-variant hypothesis ruled out by the absence of any equivalent
 * coupling on USART3 (PB11). NSS is software-driven (PB12 OUT_PP
 * rather than AF). Rippleon's src/hal/bl0939.c + src/hal/spi3.c
 * (modulo pin remap to SPI2) port over verbatim for M3. */
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
 *
 * 2026-05-11 late-pm refinement — two-snapshot diff (steady-state +
 * mid-charge with EV-sim resistors) locks 4 of the 6 scan-cache slots
 * to specific roles. Cache layout fully decoded:
 *
 *   0x20000748: VSENSE_L1 raw (op-amp railed @ 3.30 V on bench — L1
 *               sensor saturated; not split-phase calibrated)
 *   0x2000074A: VSENSE_L2 raw (same — L2 leg dead on US 120 V)
 *   0x2000074C, 0x2000074E: duplicate/aux voltage rails (also railed)
 *   0x20000750: CP_RAW (level-shifted CP pilot) — confirmed two-point
 *               swing: idle (no plug, +12 V CP) = 0x09AC (~1.99 V);
 *               EV-loaded (~+5.4 V CP) = 0x06EC (~1.43 V). Compressed
 *               read-back range (~0.56 V across 6.2 V CP swing) is the
 *               same pattern as rippleon's PB0 — empirical 2-point fit
 *               needed for M3 cp_mv() decode.
 *   0x20000752: CC raw — flat (0x0773 ~1.54 V) in both snapshots
 *               because no plug = no proximity loading; cable-side
 *               resistor varies this in real use (PROX_2200/PROX_882
 *               etc).
 *   0x20000754: I_L1 (current transformer L1) — mid-rail when
 *               contactor open (steady = 0x0879 ~1.75 V); swings DOWN
 *               under load (charging = 0x03FC ~0.82 V). Sample is
 *               instantaneous, not RMS — captures one phase of the
 *               60 Hz cycle.
 *   0x20000756: GFCI sense (analog input from the residual-current
 *               CT integrator). 4-snapshot bench evidence 2026-05-11
 *               late-pm:
 *                 idle, GFCI unplugged:   0x0743 (1.50 V)
 *                 idle, GFCI plugged:     0x0745 (1.50 V) — flat
 *                 charging, GFCI unplug:  0x05CE (1.20 V) — drift
 *                 charging, GFCI plugged: 0x06FD (1.43 V) — small
 *                                                           residual
 *               Key evidence: during the SAME kind of charging
 *               attempt, the slot reads 1.43 V with GFCI plugged
 *               (close to the healthy mid-rail baseline) but drifts
 *               to 1.20 V with GFCI unplugged (floating CT input
 *               drifts further). That coupling pattern is the
 *               signature of a CT-fed integrator: connected CT pulls
 *               the input to mid-rail at zero residual; disconnected
 *               CT lets the input drift. The onboard-NTC alternative
 *               doesn't predict the plugged-vs-unplugged differential
 *               during the same load condition.
 *
 *               (Still not 100 % closed: a single residual-current
 *               injection test on the bench, e.g. 6 mA L→ground
 *               while charging, would lock the swing direction and
 *               sensitivity for sure. Filed as bench-blocked.)
 *
 *               Note GFCI subsystem health is communicated to the
 *               MCU primarily via PC11 heartbeat suppression
 *               (independently confirmed 2026-05-11 pm — 5 rapid SWD
 *               samples of GPIOC ODR with GFCI plugged showed bit-11
 *               toggling), not via this ADC channel. The MCU watches
 *               both, but ADC tells it "how much residual current"
 *               while PC11 tells it "is the subsystem alive".
 *   0x20000758: aux rail / Vref-like (railed in both states)
 *   0x2000075A: I_L2 (current transformer L2) — pairs with 0x754
 *               (same swing pattern, same magnitude).
 *
 * Calibrated cache (0x2000075C onwards) — bench-confirmed scale:
 *   0x2000075C: CP_filtered, signed int32 mV. SCALE CONFIRMED via
 *               bench two-point: idle = +11,667 mV (≈ J1772 state-A
 *               +12 V), EV-loaded = +5,429 mV (≈ state-C +6 V range).
 *               PRIMARY CP DATA POINT for state determination — M3
 *               should read this not the raw at 0x750 when possible.
 *   0x20000760: small constant 0x118 — unchanged between states
 *   0x20000764: prior-cycle CP baseline (-300 mV as signed int32),
 *               same value pin_map cited before. Reference cal.
 *   0x20000768: small constant 0x52
 *   0x2000076C: session counter — was 0x12000000 in earlier note,
 *               read 0x01000000 in this session. Resets/advances per
 *               session.
 *   0x20000778: session-active flag — clean edge 0 → 1 between idle
 *               and charging snapshots. Useful for "EVSE state" logic.
 *
 * 2026-05-11 pm finding — ADC clocks are NOT steady-on. SWD reads of
 * RCC_APB2ENR while the chip is sleeping show ADC1EN=0, ADC2EN=0,
 * ADC3EN=0 (RCC_APB2ENR = 0x000679FD), and ADC1 register block at
 * 0x40012400 reads all-zero in that state. Stock firmware appears to
 * power-cycle the ADC peripheral around each sample (clock-enable →
 * SQR setup → SWSTART → DR read → clock-disable) to save standby
 * current. None of the active DMA channels (DMA1 ch3,5,6 + DMA2
 * ch1,6) is configured against an ADC peripheral — all 5 channels
 * feed UART RX rings. So the ADC HAL we ship in M3 will need to
 * own the clock-enable explicitly per sample (or run continuous-scan
 * mode with our own RAM target). Don't expect to see live ADC values
 * via halt-poke on the stock fw — peripheral is gated off most of
 * the time. */
/* CP_RAW physical pin BENCH-CONFIRMED 2026-05-11 (M3 build with
 * adc2_diag_scan + J1772 state walk):
 *   PB2 = ADC2 channel 13 = CP_RAW
 *
 * Cal table (5 states, multimeter at CP connector vs ADC raw):
 *   A  CP=+11.72 V → raw 2172
 *   B  CP=+8.50  V → raw 1662
 *   C  CP=+5.71  V → raw 1081
 *   D  CP=+2.81  V → raw  506
 *   E  CP=~0   V → raw   96
 *
 * Linear fit (best across the 5 points):
 *   CP_mV ≈ raw * 1000 / 187    (slope ≈ 187 raw/V)
 * 5-point residual is < 0.5 V across the whole positive range —
 * good enough for M5 J1772 state thresholds (A/B/C/D bins are >2 V
 * wide each). Slight knee approaching saturation below CP ≈ +1 V.
 *
 * Range note: the level-shifter is one-sided — CP < ~0 V saturates
 * PB2 at the ADC negative rail (0). State F (CP -12 V) is therefore
 * indistinguishable from state E or any sub-0 V CP via this channel
 * alone; relying on PB2 for fault-vs-state-F detection isn't safe.
 * Stock fw probably differentiates by checking that CP went negative
 * during a known PWM low phase (synchronous sample).
 *
 * Also bench-confirmed 2026-05-11: PA4 (ADC2 ch1) tracks CP with a
 * SECONDARY slope (~38 raw/V vs PB2's 187). Likely a high-precision
 * low-current measurement path or a differential pair partner. Use
 * PB2 as the primary CP read-back; PA4 is a backup / cross-check.
 *
 * Other ADC2 candidate pins surveyed in the same state walk:
 *   PB1, PC5            : dead-flat (not connected to active signals
 *                          on this PCBA — possibly stuffed only on
 *                          higher-current variants or board options)
 *   PC4                 : non-monotonic across states — probably
 *                          noise / coupling rather than a real signal
 *   PA5, PA7            : minor drift, no clear correlation with CP
 *
 * CC, NTC, GFCI sense, I_L1/I_L2 physical pins are STILL UNRESOLVED
 * (none moved meaningfully in the CP state walk). Resolving requires
 * a different stimulus: plug a real EV cable (CC changes), apply
 * heat to gun connector (NTC), close contactors with mains live
 * (I_L1/L2 + GFCI). All bench-blocked. */
#define PIN_ADC_CP_PORT         GPIOB
#define PIN_ADC_CP_PIN          GPIO_PIN_2      /* ADC2 ch13 — bench-confirmed */
#define PIN_ADC_VSENSE_L1_PORT  GPIOB
#define PIN_ADC_VSENSE_L1_PIN   GPIO_PIN_1      /* TBD (PB1 dead-flat on bench unit) */
#define PIN_ADC_VSENSE_L2_PORT  GPIOA
#define PIN_ADC_VSENSE_L2_PIN   GPIO_PIN_5      /* TBD candidate */
#define PIN_ADC_CC_PORT         GPIOC
#define PIN_ADC_CC_PIN          GPIO_PIN_4      /* WRONG — replaced by GFCI_SENSE below.
                                                 * Old placeholder retained for
                                                 * backward-compat only. */
#define PIN_ADC_GFCI_SENSE_PORT GPIOC
#define PIN_ADC_GFCI_SENSE_PIN  GPIO_PIN_4      /* ADC2 ch5 — BENCH-CONFIRMED 2026-05-11
                                                 * via real residual-current injection.
                                                 * Idle baseline ~1900 raw (mid-rail).
                                                 * Real GFCI fault: +242 (upward
                                                 * deflection, CT polarity-sensitive).
                                                 * CAL pulse: -234 (opposite direction,
                                                 * because CAL injects opposite-polarity
                                                 * test current).
                                                 * PE fault (tester force-state-A):
                                                 * -137 deflection (current paths
                                                 * shifted but no specific PE event).
                                                 * Slow RC-decay after fault clears
                                                 * (a few seconds time constant) —
                                                 * consistent with CT integrator.
                                                 * M5 safety task should treat any
                                                 * |raw - 1900| > N for > debounce as
                                                 * a GFCI event. Threshold TBD by
                                                 * sensitivity calibration. */

/* PE continuity sense — NOT PRESENT on this PCB.
 *
 * Bench-confirmed 2026-05-11 via tester PE-fault injection: there is
 * no dedicated digital or analog PE sense pin. The tester's "PE
 * fault" mode reads on this EVSE as "open CP cable" (PB2 jumps to
 * state-A levels regardless of dial position). The EVSE's PE
 * detection is therefore INDIRECT — via either:
 *   - The CP behavior change (state-A reading without an unplug action)
 *   - Small deflections on the GFCI sense pin (PC4) when current
 *     paths shift due to PE-line interruption
 * Matches the stock-fw SRAM cache decode: no PE_SENSE slot exists
 * in the cache (only CP/CC/I_L1/GFCI/I_L2). M5 safety task on
 * nexcyber should treat "CP unexpectedly state A while plug was
 * known-engaged" as a PE-fault candidate. */
/* NTC topology (bench-confirmed 2026-05-11 pm via ground-short test):
 *
 *   Gun-connector NTC pad → two parallel conditioning paths into
 *   ADC1 ch6 (PC0) and ADC1 ch7 (PC1). Shorting the gun-NTC pin to
 *   GND collapses BOTH PC0 and PC1 to 0 raw simultaneously.
 *     - PC0 (ADC1 ch6): high-impedance read, sits at ~3.33 V (pegged
 *       to rail) when the NTC connector is open/unpopulated.
 *     - PC1 (ADC1 ch7): divided read, sits at ~2.20 V (mid-rail) when
 *       open. The divider compresses the hot-thermistor range.
 *   Two parallel reads off the same pad is a safety-critical redundant
 *   sensing pattern — agreement gate detects an open or shorted line.
 *
 *   PC5 (ADC2 ch12): on-board PCB NTC, sits at ~1.68 V steady, NOT a
 *   gun-NTC. Confirmed by zero deflection during the gun-NTC short.
 *   Likely senses internal enclosure temp (board / contactor area).
 *
 * M6 over-temp logic must:
 *   1. Read PC0 + PC1, require both within sensible bounds.
 *   2. PC0 pegged at rail AND PC1 at mid-rail simultaneously = gun
 *      NTC unpopulated/disconnected → distinct fault "NTC_OPEN".
 *   3. PC0 = 0 AND PC1 = 0 simultaneously = NTC shorted → fault
 *      "NTC_SHORT".
 *   4. Disagreement between PC0 and PC1 (e.g., one near rail and the
 *      other low) = wiring/conditioning fault → fault "NTC_MISMATCH".
 *   5. PC5 mid-rail (~1.5–1.9 V) = healthy; deviation = onboard
 *      thermal event.
 *
 * Calibration deferred to M6 (need a known-good NTC and a thermal
 * test rig). For M5 safety, just check open/short fault patterns. */
#define PIN_ADC_NTC_GUN_A_PORT  GPIOC
#define PIN_ADC_NTC_GUN_A_PIN   GPIO_PIN_0      /* ADC1 ch6, high-Z read */
#define PIN_ADC_NTC_GUN_B_PORT  GPIOC
#define PIN_ADC_NTC_GUN_B_PIN   GPIO_PIN_1      /* ADC1 ch7, divided read */
#define PIN_ADC_NTC_BOARD_PORT  GPIOC
#define PIN_ADC_NTC_BOARD_PIN   GPIO_PIN_5      /* ADC2 ch12, onboard PCB */
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

/* PC3 = mains-detect L1 (IN_FLOATING, active-HIGH).
 * PC7 = mains-detect L2 (IN_PD, active-HIGH).
 *
 * Bench-confirmed 2026-05-11 late-pm with US 120 V single-leg supply:
 * PC3 reads HIGH (L1 present), PC7 reads LOW (L2 missing). Matches
 * exactly the "5-second voltage error" fault behaviour the user
 * observed when trying to charge — firmware's L2-presence watchdog
 * trips because the bench supply only carries one leg. Pulls a
 * latched fault_bitmap = 8 (L2-missing) per the earlier static
 * decode in NOTES.md.
 *
 * Polarity convention: each pin reads HIGH when the corresponding
 * mains leg's TLP293-2 photocoupler output is asserting "AC
 * present". The pull mode on each pin reflects the photocoupler's
 * output stage: PC3 floats (open-collector with pull-up upstream),
 * PC7 has internal pull-down (active-HIGH push-pull upstream).
 *
 * M5+ safety task should treat either pin LOW for > N ticks as
 * FAULT_MAINS_MISSING_L1 / _L2 (independent fault codes). Failing
 * to detect either trips the contactor-permit drop.
 */
#define PIN_MAINS_DETECT_L1_PORT GPIOC
#define PIN_MAINS_DETECT_L1_PIN  GPIO_PIN_3      /* IN_FLOATING, active-HIGH */
#define PIN_MAINS_DETECT_L2_PORT GPIOC
#define PIN_MAINS_DETECT_L2_PIN  GPIO_PIN_7      /* IN_PD, active-HIGH */
/* Backward-compat aliases for in-flight references */
#define PIN_MAINS_DETECT_A_PORT  PIN_MAINS_DETECT_L1_PORT
#define PIN_MAINS_DETECT_A_PIN   PIN_MAINS_DETECT_L1_PIN
#define PIN_MAINS_DETECT_B_PORT  PIN_MAINS_DETECT_L2_PORT
#define PIN_MAINS_DETECT_B_PIN   PIN_MAINS_DETECT_L2_PIN

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

/* PC11 — safety-supervisor HEARTBEAT (revised again 2026-05-11 pm).
 * Earlier-in-the-session reads showed PC11 in steady HIGH or LOW
 * states; the working model was "static enable". Later-in-the-session
 * bench observation confirmed PC11 toggles continuously while the
 * firmware is happy — those earlier "snapshot 1 vs 2" diffs were
 * just catching different phases of the heartbeat at our ~1.5 s
 * sample spacing.
 *
 * Working model: PC11 is a dead-man heartbeat the safety supervisor
 * pulses while it's happy. An external watchdog timer chip downstream
 * keeps a peripheral (GFCI module enable, coil-supply rail, or
 * similar) energised only while it keeps seeing pulses. On an
 * immediate hardware fault the firmware stops pulsing → external
 * watchdog times out → peripheral disabled.
 *
 * Bench evidence for the heartbeat model:
 *   - STOP wired through + GFCI healthy: PC11 oscillates (snapshots
 *     catch random phases — sometimes HIGH, sometimes LOW)
 *   - STOP JUMPERED out (clean short bypass): pulsing stops, PC11
 *     latches at last state (often LOW)
 *   - GFCI driven HIGH (faked fault) OR GFCI connector disconnected:
 *     same — pulsing stops
 *   - Latched soft fault `fault_bitmap = 8` (L2-missing): heartbeat
 *     continues. Confirms PC11 is for immediate hardware faults,
 *     decoupled from latched soft faults
 *
 * Pulse rate: not yet measured. Rapid SWD-sample test pending. Likely
 * 50 Hz–1 kHz based on typical external-watchdog-chip timeout windows
 * (TPL5010-style). Our M2+ firmware needs to MATCH this rate;
 * driving PC11 statically HIGH is NOT sufficient — the external
 * watchdog won't accept a DC level.
 *
 * Macro name kept PIN_SAFETY_LOOP_EN_* for continuity; "safety
 * heartbeat" is more precise. */
#define PIN_SAFETY_LOOP_EN_PORT GPIOC
#define PIN_SAFETY_LOOP_EN_PIN  GPIO_PIN_11

/* PC2 — alt-function output (AF_PP/50M), but no peripheral base address
 * referenced. Vendor-remapped peripheral or unused AF. TODO firmware
 * dynamic-trace: read AFIO_MAPR at runtime to resolve.
 */
#define PIN_PC2_AF_PORT         GPIOC
#define PIN_PC2_AF_PIN          GPIO_PIN_2

/* PA15 — freed by AFIO SWJ_CFG=010 (SWD only, JTAG off). Likely a status
 * LED or a relay-drive into the ULN2003. Wiggle game silent on 5 V rail
 * (no load activates without 12 V mains). LED walk 2026-05-11 pm with
 * MCU pin HIGH (NPN/MOSFET buffer topology) showed no LED response. */
#define PIN_PA15_OUT_PORT       GPIOA
#define PIN_PA15_OUT_PIN        GPIO_PIN_15

/* =========================================================================
 *  LED ring drive (active-HIGH via NPN/MOSFET buffer to 12V LED rail)
 * =========================================================================
 *
 * Bench-confirmed 2026-05-11 pm. The LED ring sits on its own daughter
 * board with three discrete LEDs (red / green / blue) and current-limit
 * resistors — no driver IC, no addressable protocol. The ring is fed
 * from a 12 V rail with each LED cathode running back to the main
 * PCB through a buffer transistor (NPN base or N-MOSFET gate).
 * MCU pin HIGH → buffer ON → LED cathode pulled to GND → LED on.
 *
 * MCU → buffer → LED mapping (bench-walked via tools/led_walk.py):
 *   PC10  → BLUE
 *   PC12  → GREEN
 *   ???   → RED (driver hardware-damaged on this bench unit; stock
 *           firmware pulsed red with a PWM breathing pattern, so the
 *           original drive is from a TIM channel we haven't located.
 *           After early-bench-session probing red sits stuck ON
 *           regardless of MCU state; 2026-05-11 voltmeter check
 *           showed 8 V across the red LED string at rest (12 V rail,
 *           LED Vf stack ≈ 4 V), with no response to forced PC11 or
 *           PC14 HIGH/LOW — consistent with a buffer FET in linear
 *           region (partial gate-leak failure mode, not clean short).
 *           Both candidate-driver tests inconclusive.
 *           Park red as TBD for this unit; retry identification on a
 *           fresh unit.)
 *
 * Earlier comments at PIN_PC10_AF and PIN_PC12_AF described these
 * pins as "alt-function output" based on stock-fw GPIO init analysis;
 * that was misleading. The init sets them OUT_PP (general-purpose
 * push-pull); the "AF" suffix in the macro name was speculative and
 * is corrected here. */
#define PIN_LED_BLUE_PORT       GPIOC
#define PIN_LED_BLUE_PIN        GPIO_PIN_10     /* OUT_PP, active-HIGH */
#define PIN_LED_GREEN_PORT      GPIOC
#define PIN_LED_GREEN_PIN       GPIO_PIN_12     /* OUT_PP, active-HIGH */
/* PIN_LED_RED — TBD (hardware-damaged on this bench unit) */

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
/* PA0 — CONTACTOR HOLD / PERMIT signal (revised 2026-05-11 pm based on
 * an SWD snapshot taken WHILE the unit was actively trying to charge,
 * before its 5-second voltage-error timeout fired).
 *
 * Charging-state snapshot showed PA0 ODR=1, PA1 ODR=0 (mains live,
 * contactors energised, BL0939 SPI active on PB12-15). That inverts
 * our earlier bench interpretation: PA0 is the steady-state HOLD line,
 * not a one-shot test pulse. The "click but no latch" we saw on the
 * bench is consistent with PA0 being ANDed externally with PA1's
 * post-close state or with the PC11 heartbeat — bench conditions
 * didn't satisfy the AND, so the pulse decayed.
 *
 * Working model now:
 *   - PA1 = close pulse (one-shot at session start, then RELEASED LOW;
 *     external SR latch holds the contactors)
 *   - PA0 = permit (held HIGH for entire charging session; release ->
 *     external latch reset -> contactors drop)
 *
 * That matches a textbook EVSE safety pattern: latch closes on PA1
 * edge, MCU must continuously assert PA0 to keep the latch armed; any
 * MCU fault drops PA0 and the contactors open.
 *
 * Bench-blocked confirmation steps (next session):
 *   1. Rapid-SWD sample of PA1 across a plug-in event to catch the
 *      one-shot close pulse width
 *   2. Cycle PA0 LOW mid-charge to confirm contactors drop (with
 *      current limited / no EV attached for safety) */
#define PIN_CONTACTOR_HOLD_PORT GPIOA
#define PIN_CONTACTOR_HOLD_PIN  GPIO_PIN_0      /* held HIGH during charge */
/* Backward-compat alias: earlier code used PIN_CONTACTOR_TEST_*. */
#define PIN_CONTACTOR_TEST_PORT PIN_CONTACTOR_HOLD_PORT
#define PIN_CONTACTOR_TEST_PIN  PIN_CONTACTOR_HOLD_PIN

/* PA1 — CONTACTOR CLOSE PULSE (one-shot at session start). Revised
 * 2026-05-11 pm — see PA0 comment above for full reasoning.
 *
 * The earlier bench-wiggle that "latched both contactors" was the
 * external SR latch doing its job, not PA1 itself holding the line.
 * Once latched, PA1 returns LOW and the contactors stay closed via
 * the external hold network (gated by PA0 permit).
 *
 * M5+ safety task behaviour:
 *   - Asserts PA1 HIGH briefly (~50-200 ms, TBD) when entering
 *     evse_state == CHARGING
 *   - Releases PA1 LOW immediately after the pulse
 *   - Holds PA0 HIGH for the duration of the charging session
 *   - Releases PA0 LOW on session end OR any safety fault */
#define PIN_CONTACTOR_CLOSE_PORT GPIOA
#define PIN_CONTACTOR_CLOSE_PIN  GPIO_PIN_1
/* Backward-compat alias: earlier code used PIN_CONTACTOR_MAIN_*. */
#define PIN_CONTACTOR_MAIN_PORT  PIN_CONTACTOR_CLOSE_PORT
#define PIN_CONTACTOR_MAIN_PIN   PIN_CONTACTOR_CLOSE_PIN
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
