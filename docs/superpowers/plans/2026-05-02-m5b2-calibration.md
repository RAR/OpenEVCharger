# M5.b.2: Calibration Record Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the M3 hard-coded CP read-back anchors out of `adc_inject.c` and into a persistent W25Q `calibration` record. After this milestone the firmware reads the calibration record at boot, populates an in-RAM cache, and the ADC EOC ISR consumes the cached values via `volatile` integers. A debug API lets us program new anchors without re-flashing.

**Scope:** Spec § 6 calibration ping-pong + slope/anchor parameterisation of the M3 calibration. Other calibration fields the spec lists (`ct902_zero_offset`, `leakage_ct_zero_offset`, `ntc_pullup_trim_pct`) are reserved in the struct but not read by anyone yet — those wire up in M6/M7 when the consuming code lands.

**Architecture:**
- Slots `0x002000` / `0x003000` (per spec § 6 memory map).
- Same record convention as M5.b.1 (`u32 monotonic_counter` at offset 4, `u32 crc32` at end, `_Static_assert(sizeof == 32)`).
- `calibration.c/h` with a RAM cache. `adc_inject.c`'s ISR reads three `volatile` ints (slope_num / slope_den / anchor_raw) maintained by the cache layer. No locks: writes to the three ints from the calibration setter are not atomic across all three, so the setter copies new values into a "pending" struct and atomically swaps a pointer; ISR reads the active pointer once per ISR. Simple SWP-style swap is overkill for a debug-rate setter — use a sequence counter pattern (write counter, write fields, write counter again; reader retries if counters disagree).

  *Update on reflection:* the calibration setter is debug-only / very rare (manual or M7-bench triggered, not on every charge). Bench validates that "torn read" is impossible at our duty. Use the simpler approach: setter does `__disable_irq()` around the three field updates; ISR reads them with no locking. Cost: < 1 µs IRQ blackout per setter call. Document the choice.

**Tech Stack:** Pingpong helper (M5.b.1) + CRC32 (M5) + W25Q HAL (M4).

**Hardware preconditions:**
- M5.b.1 tagged. Pingpong proven on bench.
- Bench unit on USB 5 V is sufficient.

**Success criteria:**
1. First boot after flashing: `calibration: defaults written -> slot A (counter=1, anchor=1462, slope=3540/459)`.
2. Reset: `calibration: loaded from slot A (counter=1, anchor=1462, slope=3540/459)`.
3. CP behaviour unchanged: state A still reports `cp=12000 mV`; 2.2 kΩ across CP↔PE still reports `cp≈8445 mV` (state B).
4. After programmatic `calibration_set_cp(1500, 3600, 460)` (debug-only one-shot in main): record stored to slot B with new values; **bench reading should now misclassify slightly** — proves the cache is live (revert immediately to defaults via `calibration_set_cp(1462, 3540, 459)`).
5. PD4 still blinks; uptime ≥ 5 min, no resets.
6. Build: text < 18 KB.

---

## File Structure

```
OpenEVCharger/
├── src/
│   ├── persist/
│   │   ├── calibration.c           # NEW
│   │   └── calibration.h
│   ├── hal/
│   │   └── adc_inject.c            # MODIFIED — read anchors from cache
│   └── main.c                      # MODIFIED — call calibration_load()
└── CMakeLists.txt                  # MODIFIED
```

---

## Tasks

### Task 1: calibration record + cache

**Files:**
- Create: `src/persist/calibration.{c,h}`

- [ ] **Step 1: Create `src/persist/calibration.h`**

```c
#ifndef OPENEVCHARGER_PERSIST_CALIBRATION_H
#define OPENEVCHARGER_PERSIST_CALIBRATION_H

#include <stdint.h>

#define CALIBRATION_SLOT_A   0x002000U
#define CALIBRATION_SLOT_B   0x003000U
#define CALIBRATION_VERSION  1U

/* Defaults committed M3.4.5 (bench Rippleon ROC001 2026-05-02). */
#define CAL_DEFAULT_CP_ANCHOR_RAW   1462
#define CAL_DEFAULT_CP_SLOPE_NUM    3540
#define CAL_DEFAULT_CP_SLOPE_DEN     459

/* 32 bytes total. Same envelope convention as boot_config (counter @ 4,
 * CRC @ end). The CT/leakage/NTC trim fields are reserved for M6/M7
 * and not yet read by any consumer. */
struct __attribute__((packed)) calibration {
    uint8_t  version;                   /* 1 */
    uint8_t  pad0[3];
    uint32_t monotonic_counter;         /* helper-managed */
    int16_t  cp_anchor_raw;             /* raw ADC value at +12 V */
    int16_t  cp_slope_num;              /* mV/raw numerator */
    int16_t  cp_slope_den;              /* mV/raw denominator */
    int16_t  ct902_zero_offset;         /* reserved (M6) */
    int16_t  leakage_ct_zero_offset;    /* reserved (M6) */
    int16_t  ntc_pullup_trim_pct;       /* reserved (M6) */
    uint8_t  reserved[4];
    uint32_t crc32;                     /* helper-managed */
};
_Static_assert(sizeof(struct calibration) == 32, "calibration must be 32 B");

/* Load the calibration record into the in-RAM cache. If both slots are
 * invalid, writes defaults to slot A. Updates the volatile ISR-visible
 * cache (cp_anchor / cp_slope). Returns 0 on success, <0 on error. */
int calibration_load(void);

/* Replace the CP anchor + slope and persist. Atomic w.r.t. the ADC
 * EOC ISR (interrupts disabled around the three field writes — < 1 µs).
 * Returns 0 on success, <0 on error. */
int calibration_set_cp(int16_t anchor_raw, int16_t slope_num, int16_t slope_den);

/* ISR-side accessors. Inlineable; read once per call. The variables
 * underlying these are `volatile int32_t`. */
int32_t calibration_cp_anchor_raw(void);
int32_t calibration_cp_slope_num(void);
int32_t calibration_cp_slope_den(void);

#endif
```

- [ ] **Step 2: Create `src/persist/calibration.c`**

```c
#include "calibration.h"
#include "pingpong.h"
#include "../hal/uart.h"
#include <string.h>

/* gd32f20x.h pulls in the CMSIS __disable_irq / __enable_irq intrinsics. */
#include "gd32f20x.h"

static struct calibration s_cal;

/* ISR-visible cache. Updated by load() and set_cp() with IRQs masked.
 * Promoted to int32 for the ISR's arithmetic to avoid sign-extension
 * surprises with the int16 storage. */
static volatile int32_t s_anchor_raw = CAL_DEFAULT_CP_ANCHOR_RAW;
static volatile int32_t s_slope_num  = CAL_DEFAULT_CP_SLOPE_NUM;
static volatile int32_t s_slope_den  = CAL_DEFAULT_CP_SLOPE_DEN;

int32_t calibration_cp_anchor_raw(void) { return s_anchor_raw; }
int32_t calibration_cp_slope_num(void)  { return s_slope_num; }
int32_t calibration_cp_slope_den(void)  { return s_slope_den; }

static void publish_to_isr(void)
{
    __disable_irq();
    s_anchor_raw = s_cal.cp_anchor_raw;
    s_slope_num  = s_cal.cp_slope_num;
    s_slope_den  = s_cal.cp_slope_den;
    __enable_irq();
}

int calibration_load(void)
{
    uint8_t  slot = 0;
    uint32_t counter = 0;
    int rc = pingpong_load(CALIBRATION_SLOT_A, CALIBRATION_SLOT_B,
                           &s_cal, sizeof s_cal, &slot, &counter);
    if (rc < 0) {
        printk("calibration: pingpong_load FAIL rc=%d\n", rc);
        return rc;
    }
    if (rc == 1) {
        memset(&s_cal, 0, sizeof s_cal);
        s_cal.version        = CALIBRATION_VERSION;
        s_cal.cp_anchor_raw  = CAL_DEFAULT_CP_ANCHOR_RAW;
        s_cal.cp_slope_num   = CAL_DEFAULT_CP_SLOPE_NUM;
        s_cal.cp_slope_den   = CAL_DEFAULT_CP_SLOPE_DEN;

        rc = pingpong_store(CALIBRATION_SLOT_A, CALIBRATION_SLOT_B,
                            &s_cal, sizeof s_cal, &slot, &counter);
        if (rc < 0) {
            printk("calibration: defaults write FAIL rc=%d\n", rc);
            return rc;
        }
        printk("calibration: defaults written -> slot %c (counter=%u, anchor=%d, slope=%d/%d)\n",
               'A' + slot, (unsigned)counter,
               (int)s_cal.cp_anchor_raw,
               (int)s_cal.cp_slope_num,
               (int)s_cal.cp_slope_den);
        publish_to_isr();
        return 0;
    }

    if (s_cal.version != CALIBRATION_VERSION) {
        printk("calibration: unknown version=%u, using as-is\n",
               (unsigned)s_cal.version);
    }
    printk("calibration: loaded from slot %c (counter=%u, anchor=%d, slope=%d/%d)\n",
           'A' + slot, (unsigned)counter,
           (int)s_cal.cp_anchor_raw,
           (int)s_cal.cp_slope_num,
           (int)s_cal.cp_slope_den);
    publish_to_isr();
    return 0;
}

int calibration_set_cp(int16_t anchor_raw, int16_t slope_num, int16_t slope_den)
{
    if (slope_den == 0) return -1;          /* divide-by-zero guard */

    if (s_cal.cp_anchor_raw == anchor_raw &&
        s_cal.cp_slope_num  == slope_num &&
        s_cal.cp_slope_den  == slope_den) {
        return 0;                            /* idempotent */
    }

    s_cal.version       = CALIBRATION_VERSION;
    s_cal.cp_anchor_raw = anchor_raw;
    s_cal.cp_slope_num  = slope_num;
    s_cal.cp_slope_den  = slope_den;

    uint8_t  slot = 0;
    uint32_t counter = 0;
    int rc = pingpong_store(CALIBRATION_SLOT_A, CALIBRATION_SLOT_B,
                            &s_cal, sizeof s_cal, &slot, &counter);
    if (rc < 0) {
        printk("calibration: store FAIL rc=%d\n", rc);
        return rc;
    }
    printk("calibration: stored -> slot %c (counter=%u, anchor=%d, slope=%d/%d)\n",
           'A' + slot, (unsigned)counter,
           (int)anchor_raw, (int)slope_num, (int)slope_den);
    publish_to_isr();
    return 0;
}
```

- [ ] **Step 3: Add to CMakeLists.txt `APP_SRCS`**

```cmake
    src/persist/calibration.c
```

---

### Task 2: Wire ISR to read from cache

**Files:**
- Modify: `src/hal/adc_inject.c`

- [ ] **Step 1: Replace hard-coded constants in EOC ISR**

```c
#include "adc_inject.h"
#include "gd32f20x.h"
#include "../persist/calibration.h"

static volatile uint16_t s_cp_raw = 0;
static volatile int32_t  s_cp_mv  = 0;

void adc_inject_init(void) {
    /* (unchanged — ADC injected setup) */
    ...
}

uint16_t cp_high_raw(void) { return s_cp_raw; }
int32_t  cp_high_mv(void)  { return s_cp_mv; }

void ADC0_1_IRQHandler(void)
{
    if (RESET != adc_interrupt_flag_get(ADC0, ADC_INT_FLAG_EOIC)) {
        adc_interrupt_flag_clear(ADC0, ADC_INT_FLAG_EOIC);

        uint16_t raw = adc_inserted_data_read(ADC0, ADC_INSERTED_CHANNEL_0);
        s_cp_raw = raw;

        /* Read calibration cache (volatile) once per ISR. */
        int32_t anchor = calibration_cp_anchor_raw();
        int32_t num    = calibration_cp_slope_num();
        int32_t den    = calibration_cp_slope_den();

        int32_t mv = ((int32_t)raw - anchor) * num / den + 12000;
        if (mv >  12000) mv =  12000;
        if (mv < -12000) mv = -12000;
        s_cp_mv = mv;
    }
}
```

The 5-line calibration-comment block in the prior version moves to a top-of-file comment + a one-liner above the ISR formula. Original anchor values become defaults in `calibration.h` (already done in Task 1).

- [ ] **Step 2: Compile**

```sh
cmake --build build
```

Expected: clean build. Text grows ~600 B (calibration.c) but the ISR's
hard-coded constants disappear.

---

### Task 3: Wire calibration_load into main()

**Files:**
- Modify: `src/main.c`

- [ ] **Step 1: Add include + load call**

Place the `calibration_load()` call between `boot_config_load()` and
the task creates. **Order matters** — `calibration_load()` must run
before `adc_inject_init()` if we want the ISR to see good values from
the very first conversion. But `adc_inject_init()` runs *before* the
W25Q is up (in current main.c order). Two options:

  (a) Reorder: call `spi3_init() / w25q_init() / calibration_load()`
      *before* `adc_inject_init()`. Risk: defaults work fine for a
      few ms, but cleaner.
  (b) Accept that the first ~few ADC ISRs run with the static
      `CAL_DEFAULT_*` initialisers in `calibration.c`, then switch to
      stored values after `calibration_load()` runs. Defaults match
      the bench unit's anchors exactly (3540/459/1462), so no
      observable mis-classification.

Choose (b): simpler, no init-order shuffle, defaults are already correct
for this bench. Document the choice.

```c
#include "persist/calibration.h"
...
        if (boot_config_load() < 0) {
            printk("boot_config: load failed; defaults uninitialised\n");
        }

        if (calibration_load() < 0) {
            printk("calibration: load failed; defaults active\n");
        }
    }
```

- [ ] **Step 2: Compile + flash**

```sh
cmake --build build
./tools/flash.sh
```

- [ ] **Step 3: PAUSE — bench validation: defaults written + reload**

Capture monitor output. Expected first run (post-flash, slots blank):
```
calibration: defaults written -> slot A (counter=1, anchor=1462, slope=3540/459)
J1772 state=A cp=12000 mV
```

Reset (no re-flash):
```
calibration: loaded from slot A (counter=1, anchor=1462, slope=3540/459)
J1772 state=A cp=12000 mV
```

- [ ] **Step 4: PAUSE — bench validation: state B unchanged**

With 2.2 kΩ across CP↔PE, expect `J1772 state=B cp=8445 mV` (matches M3 bench result, proves the cache feed didn't break anything).

- [ ] **Step 5: PAUSE — bench validation: write cycle proves cache is live**

Add a one-shot test in main (gated, run-once):

```c
        if (calibration_cp_slope_num() == CAL_DEFAULT_CP_SLOPE_NUM) {
            (void)calibration_set_cp(1500, 3600, 460);
        }
```

Compile, flash, monitor. Expect:
```
calibration: loaded from slot A (counter=1, anchor=1462, slope=3540/459)
calibration: stored -> slot B (counter=2, anchor=1500, slope=3600/460)
```

Now state A reading shifts: with anchor=1500 and raw still ~1462, formula
returns `(1462 - 1500) * 3600/460 + 12000 = -297 + 12000 = 11703 mV`.
Expect `J1772 state=A cp≈11703 mV` — proves the ISR really uses the cache.

- [ ] **Step 6: Restore defaults via the same path**

Replace the test gate with:
```c
        if (calibration_cp_slope_num() != CAL_DEFAULT_CP_SLOPE_NUM) {
            (void)calibration_set_cp(CAL_DEFAULT_CP_ANCHOR_RAW,
                                     CAL_DEFAULT_CP_SLOPE_NUM,
                                     CAL_DEFAULT_CP_SLOPE_DEN);
        }
```

Compile, flash, monitor. Expect:
```
calibration: loaded from slot B (counter=2, anchor=1500, slope=3600/460)
calibration: stored -> slot A (counter=3, anchor=1462, slope=3540/459)
J1772 state=A cp=12000 mV
```

- [ ] **Step 7: Remove the test block from main()**

Delete the gated `calibration_set_cp(...)` call. Future setters come from
M7's calibration menu (out of scope here).

- [ ] **Step 8: Commit**

```sh
git add src/persist/calibration.c src/persist/calibration.h \
        src/hal/adc_inject.c src/main.c CMakeLists.txt
git commit -m "M5.b.2: calibration record + cache (parameterise CP anchors)"
```

---

### Task 4: Bring-up + tag

- [ ] **Step 1: Append M5.b.2 entry to `docs/bring-up.md`**

Mirror M5.b.1 style. Record the four observed boots.

- [ ] **Step 2: Tag**

```sh
git tag -a m5b2-calibration -m "M5.b.2: calibration record (CP anchors parameterised)"
```

- [ ] **Step 3: (User-confirmed) push**

---

## After M5.b.2

1. **Memory entry refresh.** `project_openevcharger_cp_calibration` says "calibration constants belong in M5.b's calibration record" — update to "DONE" and point at sector 0x002000.
2. **Plan M5.b.3 — CRC16 + event_log scan-on-boot + append.** event_log is the first ring-buffer record type and the trickiest piece of M5.b. Spec § 6: scan-on-boot head discovery, append-at-head, sector-erase-on-boundary. CRC16 (not 32) per spec.

## Self-review

**Spec coverage (M5.b.2 slice):**

| Spec § 6 requirement | This plan |
|---|---|
| calibration ping-pong (4 KB × 2) | Tasks 1–3 |
| `cp_divider_trim_mv` field | DROPPED in favour of explicit (anchor_raw, slope_num, slope_den) — the actual M3 fit is non-additive, so an "additive trim" doesn't fit; storing the fit parameters directly is honest |
| ct902 / leakage / ntc trim | RESERVED (struct fields present, no consumer yet) |
| event_log + session_log | DEFERRED to M5.b.3/4 |
| persist_task queue | DEFERRED to M5.b.5 |
| crash-loop detector | DEFERRED to M5.b.6 |

**Placeholders:** none.

**Type/name consistency:** `calibration_*` matches `boot_config_*` shape. Slot constants `CALIBRATION_SLOT_A/B`, version `CALIBRATION_VERSION`. ISR cache accessors return int32 (one-word atomic on Cortex-M3).

**Wear:** Calibration writes are debug-only / one-shot per bench. Realistic ≤ 5 writes/year. Trivial against W25Q's 100 K cycles/sector.
