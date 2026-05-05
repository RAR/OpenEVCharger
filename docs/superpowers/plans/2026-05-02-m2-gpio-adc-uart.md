# M2: GPIO + ADC Scan + Buttons + Debug UART Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring up the rest of the on-chip I/O the safety core depends on: every load-bearing GPIO is configured per the canonical pin map, an 11-channel ADC scan replicates the stock firmware's `0x20002a80` rank layout via DMA, the 3-button resistor ladder on PC3 + the on-board PC9 button decode into discrete press events, and a USART1 console (PA9 TX / PA10 RX @ 115200) gives every subsequent milestone a `printk` channel for live diagnostics.

**Architecture:** A single `pin_map.h` becomes the canonical source of pin assignments — every later milestone refers to it instead of duplicating numbers. `hal/gpio.c` owns the bulk-init function called once before the scheduler starts; nothing else writes config registers after that. `hal/uart.c` is a polling-mode TX (busy-wait on `USART_FLAG_TBE`) printk plus an interrupt-driven RX ring buffer (used in M8 for the FC41D-side TLV protocol; M2 only needs TX). `hal/adc_scan.c` runs ADC0 in continuous-conversion + DMA1 channel 0 + circular mode at ~3.6 kHz scan rate. `ui/buttons.c` polls `adc_scan_latest()` on a 50 ms cadence inside `io_task`, applies threshold bands + 3-consecutive-same debouncing, and emits press/release events via `printk`. The watchdog continues to be kicked by `safety_task`; nothing in M2 takes over relay/PWM ownership.

**Tech Stack:** GD32F20x SPL (`gd32f20x_gpio.c`, `gd32f20x_usart.c`, `gd32f20x_adc.c`, `gd32f20x_dma.c`), FreeRTOS V11 from M1, existing CMake + arm-none-eabi-gcc + linker setup.

**Hardware preconditions:**
- M1 complete and tagged `m1-freertos-scaffold`. PD4 1 Hz blink validated under FreeRTOS.
- Bench unit powered via USB 5 V, SWD probe connected. AC NOT required for M2.
- USB-UART adapter (FT232 / CP2102 / CH340 — anything 3.3 V tolerant) wired:
  - host RX ← chip PA9 (TX) at 115200 8N1
  - host TX → chip PA10 (RX) — optional, M2 doesn't read but cable is one-time setup
  - host GND ↔ chip GND
- A terminal program (e.g. `tio /dev/ttyUSB0 -b 115200`) ready before flashing.

**Success criterion:**
1. After reset, host terminal sees a boot banner (`"OpenEVCharger M2 build <git-sha> @ 120 MHz, %d ms uptime"`) within 250 ms.
2. PD4 still blinks at 1 Hz (M1 regression).
3. Pressing each of the 3 ladder buttons on PC3 prints a unique line (`BTN top`, `BTN mid`, `BTN bot`); pressing the on-board PC9 button prints `BTN pc9`. Releasing prints `BTN release`.
4. Every 5 s `io_task` dumps the 11 ADC ranks as raw 12-bit values + roles, e.g.:
   ```
   ADC: AC=4012 NTC1=1480 CT=2010 LCT=2014 CPR=1980 CC=1960 PE=0 NTC2=1465 unused=0 BTN=4090 VREF=1505
   ```
5. All controlled-output pins idle at the safe level documented in `pin_map.h` (PE12=0, PE0=0, PE3=0, PB2=0, PE1=0 — FC41D held off in M2 — PD0=0, PD1=0, PB6=1 deasserted-CS, PB9=0, PD15=0).
6. Unit runs continuously ≥ 5 minutes with no spontaneous resets (watchdog still healthy).

---

## File Structure

```
OpenEVCharger/
├── src/
│   ├── core/
│   │   └── pin_map.h                # NEW — canonical pin assignments + idle states
│   ├── hal/
│   │   ├── gpio.c                   # NEW — gpio_init_all() one-shot bulk config
│   │   ├── gpio.h
│   │   ├── uart.c                   # NEW — USART1 init + printk + IRQ RX ring (M2 only TX)
│   │   ├── uart.h
│   │   ├── adc_scan.c               # NEW — ADC0 + DMA1 ch0 11-rank circular scan
│   │   ├── adc_scan.h
│   │   ├── wdg.c                    # unchanged
│   │   └── wdg.h                    # unchanged
│   ├── ui/
│   │   ├── buttons.c                # NEW — PC3 ladder + PC9 decoder
│   │   └── buttons.h
│   ├── tasks/
│   │   ├── io_task.c                # MODIFIED — heartbeat + buttons_poll + ADC dump
│   │   ├── io_task.h                # unchanged (period stays in .c)
│   │   ├── safety_task.{c,h}        # unchanged
│   │   ├── comms_task.{c,h}         # unchanged
│   │   └── persist_task.{c,h}       # unchanged
│   ├── main.c                       # MODIFIED — call uart_init() + gpio_init_all() pre-scheduler
│   └── FreeRTOSConfig.h             # unchanged
└── CMakeLists.txt                   # MODIFIED — add new SPL .c + new app .c
```

**Why each file is separate:**

- `core/pin_map.h` is the ONE place a future reader looks to learn what each pin does. Every later .c file `#include`s it and uses its `PIN_*` symbols. Drift between docs and code is impossible because the docs reference the header.
- `hal/gpio.c` does only one thing — bulk-init at boot — so it's small enough to audit in one read. Per-pin micro-helpers go in the consumer file (e.g. `ui/buttons.c` reads PC9, not `gpio.c`).
- `hal/uart.c` is split from `hal/gpio.c` because USART1 has its own clock + interrupt + remap considerations; merging them would force any GPIO change to also re-link the UART driver.
- `hal/adc_scan.c` is split from anything else because the ADC + DMA dance is ~120 lines of init plus a tiny accessor; later milestones (M3 injected, M6 supervisor) read its buffer, none of them re-implement scan setup.
- `ui/buttons.c` lives under `ui/` because it's a human-input decoder; no safety code reads buttons. (M9 will add `ui/ws2812.c` and `ui/buzzer.c` next to it.)

---

## Pin map summary (informational — see `pin_map.h` for full table)

| Group | Pins | M2 treatment |
|---|---|---|
| **Heartbeat LED** | PD4 | output PP, idle 0, owned by `io_task` (already in M1) |
| **Debug UART (USART1)** | PA9 (AF PP TX), PA10 (in float RX) | bring up at 115200 8N1, `printk` only |
| **ADC analog inputs** (11 of them) | PA2, PA3, PA4, PA7, PB0, PB1, PC0, PC1, PC3, PC5 + VREFINT (ch 17) | mode `GPIO_MODE_AIN`, scanned by DMA |
| **Relay coil drives** | PE12 (main), PE0 (aux) | output PP, idle 0 — **DO NOT toggle in M2** |
| **GFCI CAL** | PE3 | output PP, idle 0 (CAL inactive) — leave low in M2 |
| **Buzzer** | PB2 | output PP, idle 0 (silent) |
| **Relay readback** | PB12 | input float, read-only |
| **W25Q SPI3** | PB3 (SCK AF), PB4 (MISO float), PB5 (MOSI AF), PB6 (CS GPIO) | configure pads + idle CS=1; SPI peripheral itself stays off until M4 |
| **U11 gain bits** | PB9, PD15 | output PP, idle 0 (lowest gain — safest because won't saturate on full load current) |
| **FC41D control** | PE1 (supply), PD0 (CEN), PD1 (WAKE) | output PP, all idle 0 (module held OFF) — UART5 init in M8 |
| **WS2812 DIN** | PA15 | input float in M2 (TIM2 owns it from M9 onward) — keeps the strip dark |
| **PC9 on-board button** | PC9 | input pull-up, active-low |
| **DIP switches** | PD13, PD12, PD11, PD10 | input pull-up, read once at boot, log values |
| **Board straps to log** | PB7, PB8, PE2, PB14 | input (mode per docs), read once at boot, log values |
| **SWD** | PA13, PA14 | LEAVE ALONE (do not configure) |
| **Configured-but-unused** | PA0, PA1, PC2 | mode `GPIO_MODE_AIN`, never read in M2 |

The full machine-readable table is in `pin_map.h` (Task 1).

**Critical rule:** M2 must NEVER drive PE12, PE0, or PE13 to a non-zero state. The watchdog and lack of injected ADC mean we have no way to verify safe operation. Those pins go live in M3 (CP PWM) and M7 (relays under load).

---

## ADC scan layout (replicate stock 0x20002a80)

| Rank | ADC ch | Pin | Role |
|---:|---:|---|---|
| 0 | 2 | PA2 | AC-supply-present |
| 1 | 3 | PA3 | NTC #1 (gun temp) |
| 2 | 10 | PC0 | CT902 secondary (main current) |
| 3 | 11 | PC1 | 2 mA leakage CT |
| 4 | 4 | PA4 | CP voltage read-back |
| 5 | 7 | PA7 | CC (cable capacity) |
| 6 | 15 | PC5 | PE continuity / fault detect |
| 7 | 8 | PB0 | NTC #2 |
| 8 | 9 | PB1 | unused (board variant) |
| 9 | 13 | PC3 | 3-button resistor ladder |
| 10 | 17 | VREFINT | internal ref (TSVREFE enabled) |

**Sample-time choice:** `ADC_SAMPLETIME_239POINT5` on every channel. With ADCCLK = APB2/6 ≈ 10 MHz: per-sample = (239.5 + 12.5)/10 MHz = 25.2 µs; full 11-rank scan = 277 µs ≈ 3.6 kHz. That's plenty for slow signals (NTC, AC-presence, buttons) and we'll tighten it for current-sense channels in M6 if anti-aliasing analysis demands.

**Trigger:** software-triggered, continuous-conversion. M3 will switch the regular group OFF (so the trigger source becomes irrelevant) and add the **injected** group on TIM1 update for the CP sample. The two groups don't conflict.

**TSVREFE:** stock firmware did NOT enable it (rank 10 read garbage). We DO enable it (`adc_tempsensor_vrefint_enable()` — SPL helper sets `ADC_CTL1_TSVREN`) — gives a real VREFINT reading, useful as a sanity check that ADC isn't drifting.

**DMA:** DMA1 channel 0 (the only channel that hardware-multiplexes to ADC0). Peripheral addr = `&ADC_RDATA(ADC0)`, memory addr = `g_adc_buf[]`, transfer count = 11, mem-inc on, periph-inc off, both 16-bit, circular mode, no IRQ — buffer self-refreshes; consumer reads at any time.

**Lock-free read pattern:** consumers call `adc_scan_latest(uint16_t out[ADC_RANKS])` which `memcpy`s the 22 bytes under a brief `__disable_irq()`/`__enable_irq()` window (no kernel critical section needed; no ISR writes the buffer — DMA does, and the snapshot is correct enough for human-rate reads). Total cost ~50 cycles. M3+ that need rank-coherent reads can use the same primitive.

---

## Button decoder (PC3 ladder)

Voltage bands (initial estimate; thresholds bench-tunable in Step 6.5):

| Band | Raw 12-bit ADC | mV @ 3.3 V Vref | Decoded |
|---|---|---|---|
| `0..400` | 0–322 mV | bottom button |
| `800..1700` | 644–1369 mV | middle button |
| `1900..2800` | 1530–2255 mV | top button |
| `>3500` | >2818 mV | idle (no press) |
| anything else | "between" — count as still-debouncing |

3-consecutive-same-band debouncing at 50 ms poll → 150 ms commit time. State machine emits `BTN_PRESSED(<id>)` on a 0→pressed transition and `BTN_RELEASED` on pressed→idle. Mid-press band changes (e.g. user slides finger across the ladder PCB pads) are reported as RELEASE then PRESS.

PC9 is read straight via `gpio_input_bit_get()`; identical 3-poll debounce. PC9 is its own logical button (`PC9_BTN`), not part of the ladder.

---

## Tasks

### Task 1: Create the canonical pin map header

**Files:**
- Create: `src/core/pin_map.h`

- [ ] **Step 1: Create `src/core/pin_map.h` with one symbol per load-bearing pin and one boot-time idle level**

```c
#ifndef OPENBHZD_CORE_PIN_MAP_H
#define OPENBHZD_CORE_PIN_MAP_H

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

/* ----- Heartbeat LED (already in M1) ----- */
#define PIN_HEARTBEAT_PORT      GPIOD
#define PIN_HEARTBEAT_PIN       GPIO_PIN_4
#define PIN_HEARTBEAT_RCU       RCU_GPIOD

/* ----- Debug UART: USART1 PA9 TX / PA10 RX (no remap, default pads) ----- */
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
#define PIN_ADC_NTC2_PORT       GPIOB
#define PIN_ADC_NTC2_PIN        GPIO_PIN_0     /* rank 7, ch 8  */
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
#define PIN_RELAY_SENSE_PORT    GPIOB          /* INPUT — read-only */
#define PIN_RELAY_SENSE_PIN     GPIO_PIN_12

/* ----- GFCI ----- */
#define PIN_GFCI_CAL_PORT       GPIOE
#define PIN_GFCI_CAL_PIN        GPIO_PIN_3     /* active-low at MCU; idle low (CAL inactive) */
                                /* GFCI fault EXTI input is on a TBD pin, fixed in M6 */

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

/* ----- J1772 PWM (configured AF in M2 but TIM1 idle; PWM goes live in M3) ----- */
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
```

- [ ] **Step 2: Add `src/core/` to the include path**

Modify `CMakeLists.txt` `target_include_directories` block (existing block already lists `src`):

```cmake
target_include_directories(${TARGET} PRIVATE
    src
    src/core
    ${CMSIS_CORE}
    ${GD_HEADERS}
    ${STDP_INC}
    ${FREERTOS_DIR}/include
    ${FREERTOS_PORT_DIR}
)
```

- [ ] **Step 3: Verify build still passes (no new .c yet)**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
cmake --build build
```

Expected: identical M1 size (1.06 % flash). Header-only change.

- [ ] **Step 4: Commit**

```sh
git add src/core/pin_map.h CMakeLists.txt
git commit -m "M2.1: add canonical pin_map.h"
```

---

### Task 2: USART1 console driver

**Files:**
- Create: `src/hal/uart.c`, `src/hal/uart.h`
- Modify: `CMakeLists.txt` (add `gd32f20x_usart.c` and `src/hal/uart.c`)

- [ ] **Step 1: Create `src/hal/uart.h`**

```c
#ifndef OPENBHZD_HAL_UART_H
#define OPENBHZD_HAL_UART_H

#include <stddef.h>
#include <stdint.h>

/* Initialise USART1 at 115200 8N1 on PA9/PA10. Idempotent: safe to call
 * before the scheduler starts. After this returns, printk() is usable. */
void uart_init(void);

/* Synchronous, busy-wait TX. Safe to call from ISR or task context. Drops
 * the call cleanly (returns 0) if uart_init() hasn't run yet. Returns the
 * number of bytes written (always == len when uart is up). */
size_t uart_write(const void *buf, size_t len);

/* printf-shaped console. Format spec is a small subset:
 *   %s, %d, %u, %x, %02x, %08x, %c, %% — enough for diagnostic output.
 * Writes synchronously; never blocks on the kernel.
 * Returns bytes actually emitted. */
int printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
```

- [ ] **Step 2: Create `src/hal/uart.c`**

```c
#include "uart.h"
#include "core/pin_map.h"
#include "gd32f20x.h"
#include <stdarg.h>

static volatile uint8_t s_uart_ready = 0;

void uart_init(void)
{
    /* GPIOA + USART1 clocks */
    rcu_periph_clock_enable(PIN_USART1_RCU);
    rcu_periph_clock_enable(RCU_USART1);

    /* PA9 TX = AF push-pull 50 MHz; PA10 RX = input floating */
    gpio_init(PIN_USART1_TX_PORT, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ,
              PIN_USART1_TX_PIN);
    gpio_init(PIN_USART1_RX_PORT, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ,
              PIN_USART1_RX_PIN);

    /* 115200 8N1, RX+TX */
    usart_deinit(USART1);
    usart_baudrate_set(USART1, 115200U);
    usart_word_length_set(USART1, USART_WL_8BIT);
    usart_stop_bit_set(USART1, USART_STB_1BIT);
    usart_parity_config(USART1, USART_PM_NONE);
    usart_hardware_flow_rts_config(USART1, USART_RTS_DISABLE);
    usart_hardware_flow_cts_config(USART1, USART_CTS_DISABLE);
    usart_receive_config(USART1, USART_RECEIVE_ENABLE);
    usart_transmit_config(USART1, USART_TRANSMIT_ENABLE);
    usart_enable(USART1);

    s_uart_ready = 1;
}

static void uart_putc(char c)
{
    while (RESET == usart_flag_get(USART1, USART_FLAG_TBE)) { /* spin */ }
    usart_data_transmit(USART1, (uint8_t)c);
}

size_t uart_write(const void *buf, size_t len)
{
    if (!s_uart_ready) return 0;
    const char *p = (const char *)buf;
    for (size_t i = 0; i < len; ++i) {
        if (p[i] == '\n') uart_putc('\r');
        uart_putc(p[i]);
    }
    return len;
}

/* ----- minimal printf ----- */

static char *itoa_u(uint32_t v, char *buf, int base, int width, char pad)
{
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) {
        uint32_t d = v % (uint32_t)base;
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
        v /= (uint32_t)base;
    }
    while (n < width) tmp[n++] = pad;
    while (n--) *buf++ = tmp[n];
    return buf;
}

int printk(const char *fmt, ...)
{
    if (!s_uart_ready) return 0;
    char out[160];
    char *o = out;
    char *end = out + sizeof(out) - 1;
    va_list ap;
    va_start(ap, fmt);
    while (*fmt && o < end) {
        if (*fmt != '%') { *o++ = *fmt++; continue; }
        ++fmt;
        int width = 0; char pad = ' ';
        if (*fmt == '0') { pad = '0'; ++fmt; }
        while (*fmt >= '0' && *fmt <= '9') { width = width*10 + (*fmt - '0'); ++fmt; }
        switch (*fmt) {
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) { *o++ = '-'; v = -v; }
            o = itoa_u((uint32_t)v, o, 10, width, pad);
            break;
        }
        case 'u': o = itoa_u(va_arg(ap, unsigned), o, 10, width, pad); break;
        case 'x': o = itoa_u(va_arg(ap, unsigned), o, 16, width, pad); break;
        case 'c': *o++ = (char)va_arg(ap, int); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && o < end) *o++ = *s++;
            break;
        }
        case '%': *o++ = '%'; break;
        default:  *o++ = '%'; if (o < end) *o++ = *fmt; break;
        }
        ++fmt;
    }
    va_end(ap);
    return (int)uart_write(out, (size_t)(o - out));
}
```

- [ ] **Step 3: Add USART SPL + new uart.c to the build**

Modify `CMakeLists.txt`:

```cmake
set(STDP_SRCS
    ${STDP_SRC}/gd32f20x_gpio.c
    ${STDP_SRC}/gd32f20x_rcu.c
    ${STDP_SRC}/gd32f20x_misc.c
    ${STDP_SRC}/gd32f20x_fwdgt.c
    ${STDP_SRC}/gd32f20x_usart.c   # NEW
)

set(APP_SRCS
    src/main.c
    src/hal/wdg.c
    src/hal/uart.c                 # NEW
    src/tasks/safety_task.c
    src/tasks/io_task.c
    src/tasks/comms_task.c
    src/tasks/persist_task.c
)
```

- [ ] **Step 4: Wire `uart_init()` into `main()` BEFORE task creation**

Modify `src/main.c`:

```c
#include "FreeRTOS.h"
#include "task.h"
#include "hal/uart.h"
#include "tasks/safety_task.h"
#include "tasks/io_task.h"
#include "tasks/comms_task.h"
#include "tasks/persist_task.h"

void _init(void) {}
void _fini(void) {}

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task; (void)task_name;
    __asm volatile("cpsid i");
    for (;;) {}
}

void vApplicationMallocFailedHook(void)
{
    __asm volatile("cpsid i");
    for (;;) {}
}

extern uint32_t SystemCoreClock;

int main(void)
{
    uart_init();
    printk("\n--- OpenEVCharger M2 boot, SystemCoreClock=%u Hz ---\n", SystemCoreClock);

    safety_task_create();
    io_task_create();
    comms_task_create();
    persist_task_create();

    printk("scheduler starting\n");
    vTaskStartScheduler();

    for (;;) {}
}
```

- [ ] **Step 5: Build and verify size grows by ~1–2 KB (printk + USART driver)**

```sh
cmake --build build
```

Expected: text grows from 5544 → ~7000 B; flash usage ~1.4 %.

- [ ] **Step 6: PAUSE — bench validation of UART boot banner**

Wire host RX ← chip PA9, host GND ↔ chip GND. Open `tio /dev/ttyUSB0 -b 115200` BEFORE flashing.

Flash:
```sh
./tools/flash.sh
```

Expected on terminal within 250 ms of reset:
```
--- OpenEVCharger M2 boot, SystemCoreClock=120000000 Hz ---
scheduler starting
```

PD4 should still blink at 1 Hz (M1 regression check). If both pass, proceed to Step 7. If banner is garbled: usually means baud-rate mismatch (verify host=115200) or wrong PA9 vs PA10 wire orientation.

- [ ] **Step 7: Commit**

```sh
git add src/hal/uart.c src/hal/uart.h src/main.c CMakeLists.txt
git commit -m "M2.2: USART1 debug console + printk"
```

---

### Task 3: GPIO bulk init

**Files:**
- Create: `src/hal/gpio.c`, `src/hal/gpio.h`
- Modify: `src/main.c`, `CMakeLists.txt`

- [ ] **Step 1: Create `src/hal/gpio.h`**

```c
#ifndef OPENBHZD_HAL_GPIO_H
#define OPENBHZD_HAL_GPIO_H

#include <stdint.h>

/* One-shot bulk GPIO config for every load-bearing pin in pin_map.h.
 * Idempotent. Must run after uart_init() so any failure can printk(),
 * but before any task touches a pin. main() calls it once. */
void gpio_init_all(void);

/* Read-and-log straps (DIPs + miscellaneous strap inputs). Called once
 * after gpio_init_all() to printk a single line summarising boot config. */
void gpio_log_straps(void);

#endif
```

- [ ] **Step 2: Create `src/hal/gpio.c`**

```c
#include "gpio.h"
#include "uart.h"
#include "core/pin_map.h"
#include "gd32f20x.h"

static void clock_enable_all(void)
{
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_GPIOC);
    rcu_periph_clock_enable(RCU_GPIOD);
    rcu_periph_clock_enable(RCU_GPIOE);
    rcu_periph_clock_enable(RCU_AF);     /* AFIO bus clock for any remaps */
}

static void init_outputs_safe_low(void)
{
    /* Drive pin LOW *before* configuring as output (BC clears the bit
     * regardless of current MODE). Then configure as output PP. This
     * guarantees zero glitch to a momentary HIGH on any safety output. */
    gpio_bit_reset(PIN_RELAY_MAIN_PORT, PIN_RELAY_MAIN_PIN);
    gpio_bit_reset(PIN_RELAY_AUX_PORT,  PIN_RELAY_AUX_PIN);
    gpio_bit_reset(PIN_GFCI_CAL_PORT,   PIN_GFCI_CAL_PIN);
    gpio_bit_reset(PIN_BUZZER_PORT,     PIN_BUZZER_PIN);
    gpio_bit_reset(PIN_U11_G0_PORT,     PIN_U11_G0_PIN);
    gpio_bit_reset(PIN_U11_G1_PORT,     PIN_U11_G1_PIN);
    gpio_bit_reset(PIN_FC41D_VEN_PORT,  PIN_FC41D_VEN_PIN);
    gpio_bit_reset(PIN_FC41D_CEN_PORT,  PIN_FC41D_CEN_PIN);
    gpio_bit_reset(PIN_FC41D_WAKE_PORT, PIN_FC41D_WAKE_PIN);

    gpio_init(PIN_RELAY_MAIN_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_RELAY_MAIN_PIN);
    gpio_init(PIN_RELAY_AUX_PORT,  GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_RELAY_AUX_PIN);
    gpio_init(PIN_GFCI_CAL_PORT,   GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_GFCI_CAL_PIN);
    gpio_init(PIN_BUZZER_PORT,     GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_BUZZER_PIN);
    gpio_init(PIN_U11_G0_PORT,     GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_U11_G0_PIN);
    gpio_init(PIN_U11_G1_PORT,     GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_U11_G1_PIN);
    gpio_init(PIN_FC41D_VEN_PORT,  GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_FC41D_VEN_PIN);
    gpio_init(PIN_FC41D_CEN_PORT,  GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_FC41D_CEN_PIN);
    gpio_init(PIN_FC41D_WAKE_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_FC41D_WAKE_PIN);
}

static void init_w25q_pads(void)
{
    /* CS deasserted (HIGH) BEFORE configuring as output. */
    gpio_bit_set(PIN_W25Q_CS_PORT, PIN_W25Q_CS_PIN);
    gpio_init(PIN_W25Q_CS_PORT,   GPIO_MODE_OUT_PP,    GPIO_OSPEED_50MHZ, PIN_W25Q_CS_PIN);
    gpio_init(PIN_W25Q_SCK_PORT,  GPIO_MODE_AF_PP,     GPIO_OSPEED_50MHZ, PIN_W25Q_SCK_PIN);
    gpio_init(PIN_W25Q_MOSI_PORT, GPIO_MODE_AF_PP,     GPIO_OSPEED_50MHZ, PIN_W25Q_MOSI_PIN);
    gpio_init(PIN_W25Q_MISO_PORT, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, PIN_W25Q_MISO_PIN);
    /* SPI3 peripheral itself stays disabled until M4. */
}

static void init_cp_pwm_pad(void)
{
    /* TIM1_CH3 full-remap → PE13. AFIO remap is set when TIM1 driver
     * runs in M3; the pad just needs AF_PP here. With TIM1 disabled the
     * pin parks at the GPIO ODR level, which is HIGH-Z on AF in F1-style
     * register model — safe. */
    gpio_init(PIN_CP_PWM_PORT, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, PIN_CP_PWM_PIN);
}

static void init_inputs_floating(void)
{
    /* WS2812 stays input float in M2 (TIM2 owns it from M9 onward). Idle
     * state of the data line is then determined by external pull on the
     * board; if missing, the strip stays dark because the data line never
     * sees a valid bitstream. */
    gpio_init(PIN_WS2812_PORT,         GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, PIN_WS2812_PIN);
    gpio_init(PIN_RELAY_SENSE_PORT,    GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, PIN_RELAY_SENSE_PIN);
    gpio_init(PIN_STRAP_PB7_PORT,      GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, PIN_STRAP_PB7_PIN);
    gpio_init(PIN_STRAP_PB14_PORT,     GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, PIN_STRAP_PB14_PIN);
}

static void init_inputs_pullup(void)
{
    /* Enable pull-up via ODR=1 + IPD/IPU mode trick: in F1 register model,
     * GPIO_MODE_IPU sets CNF=10 and we then assert ODR before init to
     * select pull-up direction. */
    gpio_bit_set(PIN_BTN_PC9_PORT,   PIN_BTN_PC9_PIN);
    gpio_bit_set(PIN_DIP1_PORT,      PIN_DIP1_PIN);
    gpio_bit_set(PIN_DIP2_PORT,      PIN_DIP2_PIN);
    gpio_bit_set(PIN_DIP3_PORT,      PIN_DIP3_PIN);
    gpio_bit_set(PIN_DIP4_PORT,      PIN_DIP4_PIN);
    gpio_bit_set(PIN_STRAP_PB8_PORT, PIN_STRAP_PB8_PIN);
    gpio_bit_set(PIN_STRAP_PE2_PORT, PIN_STRAP_PE2_PIN);

    gpio_init(PIN_BTN_PC9_PORT,   GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, PIN_BTN_PC9_PIN);
    gpio_init(PIN_DIP1_PORT,      GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, PIN_DIP1_PIN);
    gpio_init(PIN_DIP2_PORT,      GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, PIN_DIP2_PIN);
    gpio_init(PIN_DIP3_PORT,      GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, PIN_DIP3_PIN);
    gpio_init(PIN_DIP4_PORT,      GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, PIN_DIP4_PIN);
    gpio_init(PIN_STRAP_PB8_PORT, GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, PIN_STRAP_PB8_PIN);
    gpio_init(PIN_STRAP_PE2_PORT, GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, PIN_STRAP_PE2_PIN);
}

static void init_analog_inputs(void)
{
    /* All analog inputs configure to GPIO_MODE_AIN. The ADC peripheral
     * itself comes up in adc_scan_init() in M2.4. */
    gpio_init(PIN_ADC_AC_PORT,     GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_AC_PIN);
    gpio_init(PIN_ADC_NTC1_PORT,   GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_NTC1_PIN);
    gpio_init(PIN_ADC_CT_PORT,     GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_CT_PIN);
    gpio_init(PIN_ADC_LCT_PORT,    GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_LCT_PIN);
    gpio_init(PIN_ADC_CP_PORT,     GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_CP_PIN);
    gpio_init(PIN_ADC_CC_PORT,     GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_CC_PIN);
    gpio_init(PIN_ADC_PE_PORT,     GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_PE_PIN);
    gpio_init(PIN_ADC_NTC2_PORT,   GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_NTC2_PIN);
    gpio_init(PIN_ADC_UNUSED_PORT, GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_UNUSED_PIN);
    gpio_init(PIN_ADC_BTN_PORT,    GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_BTN_PIN);

    /* High-Z floats for the configured-but-unused analog pins. */
    gpio_init(PIN_ADC_NC0_PORT, GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_NC0_PIN);
    gpio_init(PIN_ADC_NC1_PORT, GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_NC1_PIN);
    gpio_init(PIN_ADC_NC2_PORT, GPIO_MODE_AIN, GPIO_OSPEED_2MHZ, PIN_ADC_NC2_PIN);
}

void gpio_init_all(void)
{
    clock_enable_all();
    init_outputs_safe_low();
    init_w25q_pads();
    init_cp_pwm_pad();
    init_inputs_floating();
    init_inputs_pullup();
    init_analog_inputs();
}

void gpio_log_straps(void)
{
    int dip1 = gpio_input_bit_get(PIN_DIP1_PORT, PIN_DIP1_PIN);
    int dip2 = gpio_input_bit_get(PIN_DIP2_PORT, PIN_DIP2_PIN);
    int dip3 = gpio_input_bit_get(PIN_DIP3_PORT, PIN_DIP3_PIN);
    int dip4 = gpio_input_bit_get(PIN_DIP4_PORT, PIN_DIP4_PIN);
    int pb7  = gpio_input_bit_get(PIN_STRAP_PB7_PORT,  PIN_STRAP_PB7_PIN);
    int pb8  = gpio_input_bit_get(PIN_STRAP_PB8_PORT,  PIN_STRAP_PB8_PIN);
    int pe2  = gpio_input_bit_get(PIN_STRAP_PE2_PORT,  PIN_STRAP_PE2_PIN);
    int pb14 = gpio_input_bit_get(PIN_STRAP_PB14_PORT, PIN_STRAP_PB14_PIN);
    printk("STRAPS: dip=%d%d%d%d pb7=%d pb8=%d pe2=%d pb14=%d\n",
           dip1, dip2, dip3, dip4, pb7, pb8, pe2, pb14);
}
```

- [ ] **Step 3: Add `src/hal/gpio.c` to the build**

Modify `CMakeLists.txt`:

```cmake
set(APP_SRCS
    src/main.c
    src/hal/wdg.c
    src/hal/uart.c
    src/hal/gpio.c                 # NEW
    src/tasks/safety_task.c
    src/tasks/io_task.c
    src/tasks/comms_task.c
    src/tasks/persist_task.c
)
```

- [ ] **Step 4: Wire `gpio_init_all()` into `main()` after `uart_init()`**

Edit `src/main.c` — insert two lines after the existing `uart_init()` call:

```c
    uart_init();
    printk("\n--- OpenEVCharger M2 boot, SystemCoreClock=%u Hz ---\n", SystemCoreClock);

    gpio_init_all();           /* NEW */
    gpio_log_straps();         /* NEW */

    safety_task_create();
```

Add `#include "hal/gpio.h"` to the top of `main.c`.

- [ ] **Step 5: Remove redundant heartbeat init from `io_task.c`**

The PD4 init now lives inside `gpio_init_all()` implicitly? — no, PD4 isn't in the per-group helpers above. **Add it explicitly.** Edit `src/hal/gpio.c`'s `init_outputs_safe_low()` to also init PD4 idle-low + push-pull, then drop the `heartbeat_init()` function entirely from `io_task.c`. Patch `init_outputs_safe_low()`:

```c
    /* Heartbeat LED — was set up inline in M1's io_task; centralised here. */
    gpio_bit_reset(PIN_HEARTBEAT_PORT, PIN_HEARTBEAT_PIN);
    gpio_init(PIN_HEARTBEAT_PORT, GPIO_MODE_OUT_PP, GPIO_OSPEED_2MHZ, PIN_HEARTBEAT_PIN);
```

Then patch `src/tasks/io_task.c`:

```c
#include "io_task.h"
#include "core/pin_map.h"
#include "gd32f20x.h"

static void io_task_run(void *arg)
{
    (void)arg;
    /* GPIO already configured by gpio_init_all() in main(). */

    for (;;) {
        gpio_bit_set(PIN_HEARTBEAT_PORT, PIN_HEARTBEAT_PIN);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_bit_reset(PIN_HEARTBEAT_PORT, PIN_HEARTBEAT_PIN);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void io_task_create(void)
{
    xTaskCreate(io_task_run, "io",
                IO_TASK_STACK_WORDS, NULL, IO_TASK_PRIORITY, NULL);
}
```

- [ ] **Step 6: Build**

```sh
cmake --build build
```

Expected: text grows to ~9 KB; flash usage ~1.7 %.

- [ ] **Step 7: PAUSE — bench validation of GPIO init**

Flash and reset:

```sh
./tools/flash.sh
```

On terminal expect:
```
--- OpenEVCharger M2 boot, SystemCoreClock=120000000 Hz ---
STRAPS: dip=???? pb7=? pb8=0 pe2=1 pb14=?
scheduler starting
```

Acceptance:
- PD4 still blinks at 1 Hz.
- DMM check on each output pin (probe gently — most ride 12 V if AC were applied; on USB-only bench, every controlled output should read 0.0 V):
  - PE12, PE0, PE3, PB2, PE1, PD0, PD1, PB9, PD15 → 0.0 V (within 50 mV)
  - PB6 → 3.3 V (W25Q CS deasserted)
  - PA9 → idle high (UART TX line at rest)

If PE12 or PE0 is NOT at 0 V, **stop and debug** before proceeding. Those are relay drives on AC; we cannot ship a milestone where they default high.

- [ ] **Step 8: Commit**

```sh
git add src/hal/gpio.c src/hal/gpio.h src/main.c src/tasks/io_task.c CMakeLists.txt
git commit -m "M2.3: gpio_init_all() — bulk pin config from canonical pin_map"
```

---

### Task 4: ADC scan + DMA

**Files:**
- Create: `src/hal/adc_scan.c`, `src/hal/adc_scan.h`
- Modify: `CMakeLists.txt` (add `gd32f20x_adc.c`, `gd32f20x_dma.c`, `src/hal/adc_scan.c`)
- Modify: `src/main.c` (call `adc_scan_init()` after `gpio_init_all()`)

- [ ] **Step 1: Create `src/hal/adc_scan.h`**

```c
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

/* Snapshot the latest 11 samples atomically into out[]. Cost ~50 cycles.
 * Caller must provide a buffer of at least ADC_RANKS u16 entries. */
void adc_scan_latest(uint16_t out[ADC_RANKS]);

/* Convenience getter: returns the most recently DMA'd value for one rank.
 * Use adc_scan_latest() if you need rank coherence (which is normally
 * what you want). */
uint16_t adc_scan_rank(unsigned rank);

#endif
```

- [ ] **Step 2: Create `src/hal/adc_scan.c`**

```c
#include "adc_scan.h"
#include "gd32f20x.h"

/* DMA destination buffer. Lives in BSS at a known fixed offset; matches
 * stock firmware's 0x20002a80 only by chance — we don't depend on the
 * exact address. */
static volatile uint16_t s_adc_buf[ADC_RANKS];

void adc_scan_init(void)
{
    /* ----- 1. Clocks ----- */
    rcu_periph_clock_enable(RCU_DMA0);     /* SPL "DMA0" == hardware DMA1 */
    rcu_periph_clock_enable(RCU_ADC0);     /* SPL "ADC0" == the ADC1 instance */

    /* ADCCLK = APB2 / 6 = 60 MHz / 6 = 10 MHz (within 14 MHz max) */
    rcu_adc_clock_config(RCU_CKADC_CKAPB2_DIV6);

    /* ----- 2. ADC0 in scan + continuous mode, 11 ranks ----- */
    adc_deinit(ADC0);
    adc_mode_config(ADC_MODE_FREE);                       /* ADC0 standalone */
    adc_data_alignment_config(ADC0, ADC_DATAALIGN_RIGHT);
    adc_resolution_config(ADC0, ADC_RESOLUTION_12B);

    adc_special_function_config(ADC0, ADC_SCAN_MODE,        ENABLE);
    adc_special_function_config(ADC0, ADC_CONTINUOUS_MODE,  ENABLE);

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

    /* Software-trigger as the regular external trigger (we never call
     * adc_software_trigger_enable() because continuous mode self-loops
     * after one shot). */
    adc_external_trigger_source_config(ADC0, ADC_REGULAR_CHANNEL,
                                       ADC0_1_2_EXTTRIG_REGULAR_NONE);
    adc_external_trigger_config(ADC0, ADC_REGULAR_CHANNEL, ENABLE);

    /* ----- 3. DMA1 channel 0 → s_adc_buf, circular ----- */
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

    /* ----- 4. Hand ADC0 to DMA, calibrate, and start ----- */
    adc_dma_mode_enable(ADC0);
    adc_enable(ADC0);

    /* Vendor calibration sequence — must run with ADC enabled. */
    for (volatile int i = 0; i < 10000; ++i) { /* settle ADC ~1 µs * 10000 -> conservative */ }
    adc_calibration_enable(ADC0);

    /* In continuous mode, software_trigger fires the first conversion
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
```

- [ ] **Step 3: Add ADC + DMA SPL + new file to the build**

Modify `CMakeLists.txt`:

```cmake
set(STDP_SRCS
    ${STDP_SRC}/gd32f20x_gpio.c
    ${STDP_SRC}/gd32f20x_rcu.c
    ${STDP_SRC}/gd32f20x_misc.c
    ${STDP_SRC}/gd32f20x_fwdgt.c
    ${STDP_SRC}/gd32f20x_usart.c
    ${STDP_SRC}/gd32f20x_adc.c     # NEW
    ${STDP_SRC}/gd32f20x_dma.c     # NEW
)

set(APP_SRCS
    src/main.c
    src/hal/wdg.c
    src/hal/uart.c
    src/hal/gpio.c
    src/hal/adc_scan.c             # NEW
    src/tasks/safety_task.c
    src/tasks/io_task.c
    src/tasks/comms_task.c
    src/tasks/persist_task.c
)
```

- [ ] **Step 4: Wire `adc_scan_init()` into `main()`**

Edit `src/main.c`:

```c
#include "hal/uart.h"
#include "hal/gpio.h"
#include "hal/adc_scan.h"   /* NEW */
...
    uart_init();
    printk("\n--- OpenEVCharger M2 boot, SystemCoreClock=%u Hz ---\n", SystemCoreClock);
    gpio_init_all();
    gpio_log_straps();
    adc_scan_init();        /* NEW */
    printk("ADC scan armed: 11 ranks @ ~3.6 kHz\n");
```

- [ ] **Step 5: Build**

```sh
cmake --build build
```

Expected: text ~12 KB, flash usage ~2.4 %.

- [ ] **Step 6: PAUSE — bench validation of ADC**

We can't dump the buffer until io_task does so; that's Task 6. For this milestone confirm only that:
- Boot banner + scan-armed line appear.
- PD4 still blinks.
- Unit doesn't crash (5+ minutes of uptime).
- A halt+`mdw 0x20000000 0x100` via openocd shows the buffer area contains plausible 12-bit ADC values (0x000–0xFFF) not stuck at zero.

Halt-and-peek via openocd (don't reset; just halt):
```sh
openocd -f tools/openocd-gd32f205.cfg -c "init; halt; mdw 0x20000400 16; resume; exit"
```

(The actual address of `s_adc_buf` will appear in `build/openevcharger.map` — grep for `s_adc_buf`. The address shown above is illustrative; substitute the map-file value.)

Expected: 11 different non-zero u16 values matching live voltages on each pin (e.g. 0xFFx for AC presence on PA2 if AC connected, else ~0; ~0x500 for NTC1 at room temp; 0x000 for PB1 unused).

- [ ] **Step 7: Commit**

```sh
git add src/hal/adc_scan.c src/hal/adc_scan.h src/main.c CMakeLists.txt
git commit -m "M2.4: ADC0 + DMA1 ch0 11-rank circular scan"
```

---

### Task 5: Button decoder

**Files:**
- Create: `src/ui/buttons.c`, `src/ui/buttons.h`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create `src/ui/buttons.h`**

```c
#ifndef OPENBHZD_UI_BUTTONS_H
#define OPENBHZD_UI_BUTTONS_H

typedef enum {
    BTN_NONE = 0,
    BTN_TOP  = 1,
    BTN_MID  = 2,
    BTN_BOT  = 3,
    BTN_PC9  = 4,
} button_id_t;

/* Call once at boot (any time after adc_scan_init()). */
void buttons_init(void);

/* Poll once per io_task tick (50 ms recommended). Emits printk lines on
 * press and release transitions. Idempotent if called more frequently. */
void buttons_poll(void);

#endif
```

- [ ] **Step 2: Create `src/ui/buttons.c`**

```c
#include "buttons.h"
#include "../hal/uart.h"
#include "../hal/adc_scan.h"
#include "core/pin_map.h"
#include "gd32f20x.h"

/* Threshold bands as described in the M2 plan. Tweak in Task 5.5 once
 * the bench reading reveals which extreme (0 V or 3.3 V) is idle. */
#define BAND_BOT_HI   400u
#define BAND_MID_LO   800u
#define BAND_MID_HI  1700u
#define BAND_TOP_LO  1900u
#define BAND_TOP_HI  2800u
#define BAND_IDLE_LO 3500u

#define DEBOUNCE_N    3   /* consecutive same-reads to commit */

static button_id_t s_committed = BTN_NONE;
static button_id_t s_candidate = BTN_NONE;
static unsigned    s_count = 0;
static int         s_pc9_committed = 0;     /* 1 = pressed (active-low IDR=0) */
static int         s_pc9_candidate = 0;
static unsigned    s_pc9_count = 0;

static const char *btn_name(button_id_t b)
{
    switch (b) {
    case BTN_TOP: return "top";
    case BTN_MID: return "mid";
    case BTN_BOT: return "bot";
    case BTN_PC9: return "pc9";
    default:      return "none";
    }
}

static button_id_t classify_ladder(uint16_t raw)
{
    if (raw <= BAND_BOT_HI)                          return BTN_BOT;
    if (raw >= BAND_MID_LO && raw <= BAND_MID_HI)    return BTN_MID;
    if (raw >= BAND_TOP_LO && raw <= BAND_TOP_HI)    return BTN_TOP;
    if (raw >= BAND_IDLE_LO)                         return BTN_NONE;
    return s_candidate;  /* "between" — keep last candidate */
}

void buttons_init(void)
{
    s_committed = s_candidate = BTN_NONE;
    s_count = 0;
    s_pc9_committed = s_pc9_candidate = 0;
    s_pc9_count = 0;
}

void buttons_poll(void)
{
    /* ---- Ladder via ADC rank 9 (PC3) ---- */
    uint16_t raw = adc_scan_rank(ADC_RANK_BTN);
    button_id_t classed = classify_ladder(raw);
    if (classed == s_candidate) {
        if (s_count < DEBOUNCE_N) ++s_count;
    } else {
        s_candidate = classed;
        s_count = 1;
    }
    if (s_count >= DEBOUNCE_N && s_committed != s_candidate) {
        if (s_committed != BTN_NONE) printk("BTN release %s\n", btn_name(s_committed));
        if (s_candidate != BTN_NONE) printk("BTN press %s (raw=%u)\n",
                                            btn_name(s_candidate), raw);
        s_committed = s_candidate;
    }

    /* ---- PC9 GPIO ---- */
    int pressed = (gpio_input_bit_get(PIN_BTN_PC9_PORT, PIN_BTN_PC9_PIN) == 0) ? 1 : 0;
    if (pressed == s_pc9_candidate) {
        if (s_pc9_count < DEBOUNCE_N) ++s_pc9_count;
    } else {
        s_pc9_candidate = pressed;
        s_pc9_count = 1;
    }
    if (s_pc9_count >= DEBOUNCE_N && s_pc9_committed != s_pc9_candidate) {
        s_pc9_committed = s_pc9_candidate;
        printk("BTN %s pc9\n", s_pc9_committed ? "press" : "release");
    }
}
```

- [ ] **Step 3: Add `src/ui/buttons.c` to the build + add include path**

Modify `CMakeLists.txt`:

```cmake
set(APP_SRCS
    src/main.c
    src/hal/wdg.c
    src/hal/uart.c
    src/hal/gpio.c
    src/hal/adc_scan.c
    src/ui/buttons.c               # NEW
    src/tasks/safety_task.c
    src/tasks/io_task.c
    src/tasks/comms_task.c
    src/tasks/persist_task.c
)
```

(`src/` is already in include path so `#include "ui/buttons.h"` works.)

- [ ] **Step 4: Build**

```sh
cmake --build build
```

Expected: text ~13 KB, flash usage ~2.6 %.

- [ ] **Step 5: Commit**

```sh
git add src/ui/buttons.c src/ui/buttons.h CMakeLists.txt
git commit -m "M2.5: PC3 ladder + PC9 button decoder"
```

---

### Task 6: Integrate buttons + ADC dump into `io_task`

**Files:**
- Modify: `src/tasks/io_task.c`

- [ ] **Step 1: Restructure `io_task_run()` to a 50 ms tick that does heartbeat, button poll, and 5 s ADC dump**

Replace the body of `src/tasks/io_task.c`:

```c
#include "io_task.h"
#include "core/pin_map.h"
#include "ui/buttons.h"
#include "../hal/uart.h"
#include "../hal/adc_scan.h"
#include "gd32f20x.h"

#define IO_TICK_MS    50
#define HB_TOGGLE_MS  500
#define DUMP_MS       5000

static void adc_dump(void)
{
    uint16_t b[ADC_RANKS];
    adc_scan_latest(b);
    printk("ADC: AC=%u NTC1=%u CT=%u LCT=%u CPR=%u CC=%u PE=%u "
           "NTC2=%u UNUSED=%u BTN=%u VREF=%u\n",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10]);
}

static void io_task_run(void *arg)
{
    (void)arg;
    buttons_init();

    TickType_t last_wake = xTaskGetTickCount();
    unsigned ms = 0;

    for (;;) {
        /* Heartbeat: toggle PD4 every HB_TOGGLE_MS (1 Hz LED cycle). */
        if ((ms % HB_TOGGLE_MS) == 0) {
            static int level = 0;
            level ^= 1;
            if (level) gpio_bit_set(PIN_HEARTBEAT_PORT, PIN_HEARTBEAT_PIN);
            else       gpio_bit_reset(PIN_HEARTBEAT_PORT, PIN_HEARTBEAT_PIN);
        }

        /* Buttons every tick. */
        buttons_poll();

        /* ADC dump every DUMP_MS. */
        if ((ms % DUMP_MS) == 0) adc_dump();

        ms += IO_TICK_MS;
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(IO_TICK_MS));
    }
}

void io_task_create(void)
{
    xTaskCreate(io_task_run, "io",
                IO_TASK_STACK_WORDS, NULL, IO_TASK_PRIORITY, NULL);
}
```

- [ ] **Step 2: Build**

```sh
cmake --build build
```

Expected: text ~13.5 KB, flash usage ~2.7 %.

- [ ] **Step 3: PAUSE — full M2 bench validation**

Wire host RX ← chip PA9 again, terminal at 115200. Flash:

```sh
./tools/flash.sh
```

Expected terminal output within 250 ms of reset:
```
--- OpenEVCharger M2 boot, SystemCoreClock=120000000 Hz ---
STRAPS: dip=???? pb7=? pb8=0 pe2=1 pb14=?
ADC scan armed: 11 ranks @ ~3.6 kHz
scheduler starting
ADC: AC=... NTC1=... CT=... ...
```

Expected interactive behaviour:
- PD4 blinks at 1 Hz.
- Press each ladder button. Observe `BTN press top (raw=…)` etc., release.
- Press the on-board PC9 button. Observe `BTN press pc9` / `BTN release pc9`.
- Every 5 s a fresh ADC dump line appears.
- Unit runs ≥ 5 minutes, no spontaneous resets.

If any band threshold is wrong (e.g. all four buttons fall outside the bands), record actual raw values from the ADC dump while pressing each button and update the `BAND_*` constants in `buttons.c`. Re-flash, re-test.

If PC9 reads inverted (idle reports as "pressed"), invert the polarity in `buttons_poll()`'s `pressed` line.

- [ ] **Step 4: Commit**

```sh
git add src/tasks/io_task.c
git commit -m "M2.6: io_task drives heartbeat + button poll + ADC dump"
```

If button thresholds were tuned in Step 3, also commit `buttons.c`:

```sh
git add src/ui/buttons.c
git commit -m "M2.6.1: tune ladder thresholds to bench-measured values"
```

---

### Task 7: Bring-up log + tag

**Files:**
- Modify: `docs/bring-up.md`

- [ ] **Step 1: Append M2 entry to `docs/bring-up.md`**

Pattern to follow (mirror M0/M1 entries already in the file):

```markdown
## M2 — GPIO + ADC scan + buttons + USART1 console

**Date completed:** YYYY-MM-DD
**Spec section:** § 9 M2
**Plan:** docs/superpowers/plans/2026-05-02-m2-gpio-adc-uart.md

### Success criterion
USART1 boot banner visible at 115200, all 4 buttons (PC3 ladder × 3 + PC9) decode to unique printk lines, ADC dump prints plausible values every 5 s, watchdog still healthy across ≥ 5 min uptime.

### Observed result
- Boot banner: **YES** — appears within ___ ms of reset
- PD4 1 Hz blink: **YES** (M1 regression)
- Per-button decode:
  - BTN top: raw = ____
  - BTN mid: raw = ____
  - BTN bot: raw = ____
  - BTN pc9: PC9 idles ____, pressed ____
- ADC dump (sample, no AC connected on bench):
  ```
  ADC: AC=... NTC1=... CT=... LCT=... CPR=... CC=... PE=... NTC2=... UNUSED=... BTN=... VREF=...
  ```
- Output idle states (DMM):
  - PE12 (relay main): _____ V
  - PE0 (relay aux): _____ V
  - PE3 (GFCI CAL): _____ V
  - PB2 (buzzer): _____ V
  - PE1 (FC41D supply): _____ V
  - PB6 (W25Q CS): _____ V
- Continuous-uptime stability: **YES / NO** for ≥ 5 min

### Build size
- text ~13.5 KB, flash usage ~2.7 %.

### Hardware notes / deviations from plan
(Anything that surprised the bench engineer — threshold tuning numbers, idle-polarity flip on PC9, FC41D module behaviour with VEN held low, ADC noise floor observations, etc.)

### Next milestone
M3: TIM1 CP PWM at 1 kHz + injected ADC trigger. Plan to be written next.
```

- [ ] **Step 2: Commit bring-up notes**

```sh
git add docs/bring-up.md
git commit -m "M2.7: bring-up log — M2 validated on bench"
```

- [ ] **Step 3: Tag M2**

```sh
git tag -a m2-gpio-adc-uart -m "M2 complete: GPIO + ADC scan + buttons + USART1 console, validated on bench"
```

- [ ] **Step 4: (User-confirmed) push**

```sh
git push origin main
git push origin m2-gpio-adc-uart
```

Don't push without user confirmation.

---

## After M2

1. **Update memory** with anything new — typically:
   - PC3 ladder idle polarity (0 V vs 3.3 V) and the as-measured threshold mV values
   - PC9 button function inferred from any UI experiments (factory reset? Pairing?)
   - Strap reading for PB7/PB8/PE2/PB14 (lock these in `pin_map.h` comments if a meaning emerges)
   - Real VREFINT reading (sanity-check ADC scaling for future milestones)
2. **Write M3 plan.** M3 = TIM1 full-remap → PE13 PWM at 1 kHz + injected ADC trigger on TIM1 update event + CP state classifier (states A/B/C/E/F per voltage band). M3 finally exercises the TIM1 + ADC injected combination the spec architecture rests on.

---

## Self-review

**Spec coverage check (M2 only — § 9 M2 row):**

| Spec requirement (M2) | Plan task |
|---|---|
| All pins configured per pin map | Task 1 (`pin_map.h`) + Task 3 (`gpio_init_all()`) |
| DMA scan ADC running (replicate stock 0x20002a80 layout) | Task 4 (`adc_scan_init()` with rank order matching the spec table) |
| 3-button ladder reading PC3 | Task 5 (`buttons.c` ladder bands) |
| **SUCCESS = pressing each button prints unique ID** | Task 6 (io_task wires `printk` to `BTN press/release`) |
| Implicit prerequisite for "press logged via debug UART" | Task 2 (USART1 + `printk`) |

**Placeholder scan.** No "TODO", "TBD", or "implement later" tokens in code. Threshold bands are explicit numeric defaults; the plan calls out that the engineer will bench-tune and commit revised values in Step 6.3 if needed (this is the normal kind of "may need to adjust based on physical reading" — not a placeholder).

**Type/name consistency.**
- `PIN_*` macros all follow `PIN_<role>_PORT/_PIN/_RCU` convention.
- `adc_scan_init` / `adc_scan_latest` / `adc_scan_rank` consistently used between header, .c, and consumer (`io_task.c`, `buttons.c`).
- `buttons_init` / `buttons_poll` consistent across header, .c, and `io_task.c`.
- ADC rank numbers in `ADC_RANK_*` enum match the rank arguments passed to `adc_regular_channel_config()` 1-for-1.
- Button enum ordering (`BTN_NONE` first) matches the `s_committed = BTN_NONE` initialiser pattern.

**Gaps.** None for M2 success criterion. Items deliberately deferred:
- Real ADC scaling/calibration (just raw 12-bit values printed) → M3 (CP needs voltage-to-state mapping).
- Real button-action wiring (a press doesn't start charging or anything yet) → M9 (UI polish).
- USART1 RX path (printk is TX-only in M2) → M8 if needed for a future console; not blocking.
- FC41D module wakeup (PE1=low keeps module off in M2) → M8.
- WS2812 (PA15 stays input float) → M9.

**Single-writer discipline preserved:** `gpio_init_all()` is the only place that *configures* output pins. After it returns, no M2 code *writes* any of the relay/GFCI/buzzer/FC41D outputs. The `init_outputs_safe_low()` BR/BSRR pattern guarantees pins go through "drive low → configure as output", never "configure as output → drive low" (which would race a momentary high).
