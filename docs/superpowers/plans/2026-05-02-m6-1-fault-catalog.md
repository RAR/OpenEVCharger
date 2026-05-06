# M6.1 Fault Catalog + EVSE State Machine Skeleton

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Inline execution.

**Goal:** Land the fault module (`core/fault.{c,h}`), the EVSE state enum (`core/evse_state.h`), and wire both into `safety_task` so the supervisor maintains a fault bitmap and an EVSE state, raises `FAULT_CRASH_LOOP_SAFE_FAIL` when `crash_state_is_safe_fail()`, and prints state transitions over semihost. No fault detection logic beyond safe-fail yet — that's M6.2+.

**Architecture:**
- `fault_id_t` enum: latched faults first, self-clearing after, sentinel `FAULT_COUNT` last. Sequential values used as bit indices into a `uint32_t` active-bitmap.
- `fault_state_t` holds the bitmap + first-raised tracker. Pure C, host-testable.
- `evse_state_t` mirrors spec § 5: BOOT → SELF_TEST → READY → CHARGING → USER_PAUSED → COOLING_DOWN → FAULT.
- `safety_task` owns a static `fault_state_t` and `evse_state_t`. Each tick: read inputs (just CP for now), classify J1772, run a check matrix (currently a one-line "safe-fail?"), update evse_state, log on edges.

**Tech stack:** C11 + FreeRTOS, no new dependencies. CMakeLists.txt adds `core/fault.c`.

---

### Task 1: Create `core/fault.h`

**Files:**
- Create: `src/core/fault.h`

- [ ] **Step 1: Write the header**

```c
#ifndef OPENEVCHARGER_CORE_FAULT_H
#define OPENEVCHARGER_CORE_FAULT_H

#include <stdint.h>

/* Fault catalog. Numeric values double as bit indices into the active
 * bitmap, so enum order matters. Latched faults come first to keep the
 * latched/self-clearing split contiguous. */
typedef enum {
    FAULT_NONE = 0,

    /* Latched (cleared only by power-cycle for GFCI, by FC41D
     * CLEAR_FAULT TLV otherwise). Bit 0 unused. */
    FAULT_GFCI                  = 1,
    FAULT_RELAY_WELD            = 2,
    FAULT_RELAY_STUCK_OPEN      = 3,
    FAULT_PE_CONTINUITY         = 4,
    FAULT_CP_NO_PILOT           = 5,   /* J1772 state E sustained */
    FAULT_DIODE_CHECK           = 6,
    FAULT_BOOT_SELF_TEST        = 7,
    FAULT_GFCI_SELF_TEST        = 8,
    FAULT_RELAY_WELD_AT_BOOT    = 9,
    FAULT_RELAY_OPEN_AT_BOOT    = 10,
    FAULT_ADC_OUT_OF_RANGE      = 11,
    FAULT_HARD_OVER_CURRENT     = 12,
    FAULT_CRASH_LOOP_SAFE_FAIL  = 13,

    /* Self-clearing (auto-clear when the underlying condition removes,
     * with hysteresis where applicable). */
    FAULT_OVER_TEMP             = 16,
    FAULT_SOFT_OVER_CURRENT     = 17,
    FAULT_CC_OUT_OF_RANGE       = 18,
    FAULT_AC_ABSENT             = 19,
    FAULT_CP_REGRESSION         = 20,

    FAULT_COUNT                 = 21
} fault_id_t;

#define FAULT_FIRST_SELF_CLEARING  FAULT_OVER_TEMP

typedef struct {
    uint32_t   active_bits;       /* bit n = fault id n is currently raised */
    fault_id_t first_raised;      /* first fault that drove the EVSE into FAULT */
} fault_state_t;

void fault_init(fault_state_t *s);

/* Returns 1 if this is a new raise (bit was clear), 0 if already set,
 * <0 on invalid id. */
int  fault_raise(fault_state_t *s, fault_id_t id);

/* Returns 1 if this cleared (bit was set), 0 if already clear,
 * <0 on invalid id or if id is GFCI (which is power-cycle only). */
int  fault_clear(fault_state_t *s, fault_id_t id);

int  fault_is_active(const fault_state_t *s, fault_id_t id);
int  fault_any_active(const fault_state_t *s);
int  fault_any_latched_active(const fault_state_t *s);

/* True if id is a latched-class fault (vs self-clearing). */
int  fault_is_latched_kind(fault_id_t id);

/* Clear all FC41D-clearable latched faults (everything latched except
 * GFCI). Returns count cleared. Does not touch self-clearing faults. */
int  fault_clear_all_clearable(fault_state_t *s);

/* Stable short string for logging / TLV. Never returns NULL. */
const char *fault_name(fault_id_t id);

#endif /* OPENEVCHARGER_CORE_FAULT_H */
```

- [ ] **Step 2: Commit not yet — finish the .c next.**

---

### Task 2: Create `core/fault.c`

**Files:**
- Create: `src/core/fault.c`

- [ ] **Step 1: Implementation**

```c
#include "fault.h"

#define FID_OK(id)  ((id) > FAULT_NONE && (id) < FAULT_COUNT)

void fault_init(fault_state_t *s)
{
    s->active_bits  = 0u;
    s->first_raised = FAULT_NONE;
}

int fault_is_latched_kind(fault_id_t id)
{
    if (!FID_OK(id)) return 0;
    return (id < FAULT_FIRST_SELF_CLEARING) ? 1 : 0;
}

int fault_is_active(const fault_state_t *s, fault_id_t id)
{
    if (!FID_OK(id)) return 0;
    return ((s->active_bits >> (uint32_t)id) & 1u) ? 1 : 0;
}

int fault_any_active(const fault_state_t *s)
{
    return s->active_bits ? 1 : 0;
}

int fault_any_latched_active(const fault_state_t *s)
{
    /* Latched bits are 1..FAULT_FIRST_SELF_CLEARING-1. */
    uint32_t mask = 0u;
    for (uint32_t i = 1u; i < (uint32_t)FAULT_FIRST_SELF_CLEARING; ++i) {
        mask |= (1u << i);
    }
    return (s->active_bits & mask) ? 1 : 0;
}

int fault_raise(fault_state_t *s, fault_id_t id)
{
    if (!FID_OK(id)) return -1;
    uint32_t mask = 1u << (uint32_t)id;
    if (s->active_bits & mask) return 0;
    s->active_bits |= mask;
    if (s->first_raised == FAULT_NONE) s->first_raised = id;
    return 1;
}

int fault_clear(fault_state_t *s, fault_id_t id)
{
    if (!FID_OK(id)) return -1;
    if (id == FAULT_GFCI) return -1;     /* GFCI is power-cycle only */
    uint32_t mask = 1u << (uint32_t)id;
    if (!(s->active_bits & mask)) return 0;
    s->active_bits &= ~mask;
    if (s->first_raised == id) {
        /* Promote next-active fault to first_raised, or NONE. */
        s->first_raised = FAULT_NONE;
        for (uint32_t i = 1u; i < (uint32_t)FAULT_COUNT; ++i) {
            if (s->active_bits & (1u << i)) {
                s->first_raised = (fault_id_t)i;
                break;
            }
        }
    }
    return 1;
}

int fault_clear_all_clearable(fault_state_t *s)
{
    int n = 0;
    /* Everything latched except GFCI. */
    for (uint32_t i = 1u; i < (uint32_t)FAULT_FIRST_SELF_CLEARING; ++i) {
        if ((fault_id_t)i == FAULT_GFCI) continue;
        if (s->active_bits & (1u << i)) {
            s->active_bits &= ~(1u << i);
            ++n;
        }
    }
    /* Recompute first_raised. */
    s->first_raised = FAULT_NONE;
    for (uint32_t i = 1u; i < (uint32_t)FAULT_COUNT; ++i) {
        if (s->active_bits & (1u << i)) {
            s->first_raised = (fault_id_t)i;
            break;
        }
    }
    return n;
}

const char *fault_name(fault_id_t id)
{
    switch (id) {
    case FAULT_NONE:                  return "none";
    case FAULT_GFCI:                  return "GFCI";
    case FAULT_RELAY_WELD:            return "RELAY_WELD";
    case FAULT_RELAY_STUCK_OPEN:      return "RELAY_STUCK_OPEN";
    case FAULT_PE_CONTINUITY:         return "PE_CONTINUITY";
    case FAULT_CP_NO_PILOT:           return "CP_NO_PILOT";
    case FAULT_DIODE_CHECK:           return "DIODE_CHECK";
    case FAULT_BOOT_SELF_TEST:        return "BOOT_SELF_TEST";
    case FAULT_GFCI_SELF_TEST:        return "GFCI_SELF_TEST";
    case FAULT_RELAY_WELD_AT_BOOT:    return "RELAY_WELD_AT_BOOT";
    case FAULT_RELAY_OPEN_AT_BOOT:    return "RELAY_OPEN_AT_BOOT";
    case FAULT_ADC_OUT_OF_RANGE:      return "ADC_OUT_OF_RANGE";
    case FAULT_HARD_OVER_CURRENT:     return "HARD_OVER_CURRENT";
    case FAULT_CRASH_LOOP_SAFE_FAIL:  return "CRASH_LOOP_SAFE_FAIL";
    case FAULT_OVER_TEMP:             return "OVER_TEMP";
    case FAULT_SOFT_OVER_CURRENT:     return "SOFT_OVER_CURRENT";
    case FAULT_CC_OUT_OF_RANGE:       return "CC_OUT_OF_RANGE";
    case FAULT_AC_ABSENT:             return "AC_ABSENT";
    case FAULT_CP_REGRESSION:         return "CP_REGRESSION";
    default:                          return "?";
    }
}
```

---

### Task 3: Create `core/evse_state.h`

**Files:**
- Create: `src/core/evse_state.h`

- [ ] **Step 1: Header only — no implementation needed yet.**

```c
#ifndef OPENEVCHARGER_CORE_EVSE_STATE_H
#define OPENEVCHARGER_CORE_EVSE_STATE_H

#include <stdint.h>

/* Top-level supervisor states. Numeric values match spec § 5. */
typedef enum {
    EVSE_BOOT          = 0,
    EVSE_SELF_TEST     = 1,
    EVSE_READY         = 2,
    EVSE_CHARGING      = 3,
    EVSE_USER_PAUSED   = 4,
    EVSE_COOLING_DOWN  = 5,
    EVSE_FAULT         = 6,
} evse_state_t;

static inline const char *evse_state_name(evse_state_t s)
{
    switch (s) {
    case EVSE_BOOT:         return "BOOT";
    case EVSE_SELF_TEST:    return "SELF_TEST";
    case EVSE_READY:        return "READY";
    case EVSE_CHARGING:     return "CHARGING";
    case EVSE_USER_PAUSED:  return "USER_PAUSED";
    case EVSE_COOLING_DOWN: return "COOLING_DOWN";
    case EVSE_FAULT:        return "FAULT";
    default:                return "?";
    }
}

#endif /* OPENEVCHARGER_CORE_EVSE_STATE_H */
```

---

### Task 4: Wire fault + evse_state into `safety_task`

**Files:**
- Modify: `src/tasks/safety_task.c`

- [ ] **Step 1: Replace the body**

```c
#include "safety_task.h"
#include "../hal/wdg.h"
#include "../hal/uart.h"
#include "../hal/adc_inject.h"
#include "../core/j1772.h"
#include "../core/fault.h"
#include "../core/evse_state.h"
#include "../persist/crash_state.h"

#define J1772_DEBOUNCE_N  3U

static void evse_transition(evse_state_t *cur, evse_state_t next)
{
    if (*cur == next) return;
    printk("EVSE state %s -> %s\n", evse_state_name(*cur), evse_state_name(next));
    *cur = next;
}

static void check_safe_fail(fault_state_t *fs, evse_state_t *es)
{
    if (crash_state_is_safe_fail() &&
        !fault_is_active(fs, FAULT_CRASH_LOOP_SAFE_FAIL)) {
        if (fault_raise(fs, FAULT_CRASH_LOOP_SAFE_FAIL) == 1) {
            printk("FAULT raised: %s (first=%s)\n",
                   fault_name(FAULT_CRASH_LOOP_SAFE_FAIL),
                   fault_name(fs->first_raised));
        }
        evse_transition(es, EVSE_FAULT);
    }
}

static void safety_task_run(void *arg)
{
    (void)arg;
    wdg_init();

    j1772_ctx_t cp;
    j1772_init(&cp);
    j1772_state_t last_logged_j1772 = J1772_STATE_INVALID;

    fault_state_t fs;
    fault_init(&fs);
    evse_state_t  es = EVSE_BOOT;

    /* Boot path: BOOT -> SELF_TEST -> (FAULT if safe-fail else READY).
     * SELF_TEST is a placeholder until M6.6 lands the real self-test. */
    evse_transition(&es, EVSE_SELF_TEST);
    check_safe_fail(&fs, &es);
    if (es != EVSE_FAULT) {
        evse_transition(&es, EVSE_READY);
    }

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        wdg_kick();

        int32_t cp_mv = cp_high_mv();
        j1772_state_t s = j1772_step(&cp, cp_mv, J1772_DEBOUNCE_N);

        if (s != last_logged_j1772 && s != J1772_STATE_INVALID) {
            printk("J1772 state=%s cp=%d mV\n",
                   j1772_state_name(s), (int)cp_mv);
            last_logged_j1772 = s;
        }

        /* Re-check safe-fail every tick — defensive (already raised at
         * boot if applicable, but cheap). */
        check_safe_fail(&fs, &es);

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

---

### Task 5: Add `core/fault.c` to CMake

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add to APP_SRCS list (sorted alphabetically with existing core/ entries)**

Add `src/core/fault.c` next to `src/core/j1772.c`.

---

### Task 6: Build + flash + bench validate

- [ ] **Build:** `cmake --build build` — should compile clean.
- [ ] **Flash:** `tools/flash.sh`.
- [ ] **Monitor:** start `tools/openocd-monitor.sh` in background, capture log for ≥ 5 s.
- [ ] **Expected output (normal boot, fast_restart < 5):**

```
--- OpenEVCharger M2 boot, ...
boot_count = N
crash_state: fast_restart=K (slot ...) [K < 5]
EVSE state BOOT -> SELF_TEST
EVSE state SELF_TEST -> READY
J1772 state=A cp=11... mV
```

- [ ] **Expected output (forced safe-fail by repeated quick resets):**

```
crash_state: fast_restart=5+ ... safe-fail latched
EVSE state BOOT -> SELF_TEST
FAULT raised: CRASH_LOOP_SAFE_FAIL (first=CRASH_LOOP_SAFE_FAIL)
EVSE state SELF_TEST -> FAULT
```

(We do NOT need to force safe-fail on this milestone — M5.b.6 already validated the state machine end-to-end. If fast_restart counter ramps during validation cycles and eventually reaches threshold, that's fine as long as we observe the FAULT path log line once.)

---

### Task 7: bring-up.md entry + commit + push + tag

- [ ] **Step 1:** append M6.1 entry to `docs/bring-up.md` with bench output.
- [ ] **Step 2:** `git add` + commit:

```
M6.1: fault catalog + EVSE state machine skeleton

- src/core/fault.{c,h}: 19-fault catalog (13 latched + 5 self-clearing),
  uint32 active-bitmap, raise/clear/is_active/clear_all_clearable API,
  stable fault_name() for logging.
- src/core/evse_state.h: 7 supervisor states matching spec § 5.
- src/tasks/safety_task.c: integrate fault_state + evse_state. Boot
  transitions BOOT -> SELF_TEST -> READY (or FAULT if crash_state safe-
  fail latched). Logs every fault raise + every EVSE transition.

Pure-logic core/fault.c is host-testable per spec § 11; safety_task is
the only consumer for now. Detection logic for individual faults lands
in M6.2..M6.5; boot self-test in M6.6.
```

- [ ] **Step 3:** push origin main, tag `m6-1-fault-catalog`, push tag.
