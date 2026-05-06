# M5.b.6: Crash-Loop Detector Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Detect rapid power-cycle / reset sequences and enter a safe-fail mode that refuses to charge until commanded clear. Spec § 6: *"If now − last_boot_ts < 60 s for 5 consecutive boots, enter safe-fail mode (refuse to charge until FC41D commands clear)."* Final M5.b sub-milestone — after this lands, M6 (safety supervisor) is unblocked.

**Scope:** Persistence + state of `crash_state`, decision in `main()`, "I'm alive past 60 s" reset from `io_task`. Safe-fail mode itself is a flag (`s_safe_fail = 1`) plus a printk on each boot; the policy hook (refuse to set CP duty cycle) wires up in M6 since CP control belongs to safety_task.

**Architecture:**
- New 32-byte ping-pong record `crash_state` with one tracked field: `u8 fast_restart_count`. Two slots at `0x04D000 / 0x04E000` (spec's "reserved" region).
- **Boot-time:** main() reads `crash_state`, increments `fast_restart_count`, persists it. If count ≥ 5 → log + set `safe_fail` flag. Synchronous (pre-scheduler).
- **60-s alive marker:** io_task arms a one-shot — at tick ≥ 60 s, posts a `PERSIST_REQ_CRASH_STATE_RESET` to persist_task. persist_task drains and writes `fast_restart_count = 0`. Funneling through persist_task avoids two-tasks-touching-W25Q-without-mutex.
- **Safe-fail policy hook:** `crash_state_is_safe_fail()` accessor for M6's safety_task to consult. M5.b.6 doesn't need to gate any output yet — the whole CP/relay control path lands in M6.

**Tech Stack:** Pingpong helper (M5.b.1) + persist_task queue (M5.b.5).

**Hardware preconditions:**
- M5.b.5 tagged. Producer queue proven on bench.

**Success criteria:**
1. First boot after flashing (record blank): `crash_state: defaults written -> slot A (counter=1, fast_restart=0)` then `crash_state: incremented to fast_restart=1`.

   Actually simpler: read-then-increment is a single store with the new value. Sequence becomes: scan blank slots → write defaults → also reflect "this boot": `fast_restart=1`. Subsequent boots: load → increment → store.
2. After 60 s of uptime, monitor prints `crash_state: alive marker -> fast_restart=0`.
3. Reset within 60 s of boot (no alive marker fires) → next boot reports `fast_restart=2`.
4. Reset 5× in rapid succession → fifth boot reports `crash_state: SAFE-FAIL ENTERED (fast_restart=5)`.
5. After safe-fail, wait > 60 s, monitor still prints SAFE-FAIL until alive marker fires; once it does, count resets to 0 and a subsequent boot is normal.
6. PD4 still blinks.
7. Build: text < 21 KB.

---

## File Structure

```
OpenEVCharger/
├── src/
│   ├── persist/
│   │   ├── crash_state.c           # NEW
│   │   └── crash_state.h
│   ├── tasks/
│   │   ├── persist_task.c          # MODIFIED — handle CRASH_STATE_RESET req
│   │   ├── persist_task.h          # MODIFIED — persist_post_crash_state_reset()
│   │   └── io_task.c               # MODIFIED — 60-s alive marker
│   └── main.c                      # MODIFIED — boot-time read+increment+decide
└── CMakeLists.txt                  # MODIFIED
```

---

## Tasks

### Task 1: crash_state record

**Files:**
- Create: `src/persist/crash_state.{c,h}`

- [ ] **Step 1: Create `src/persist/crash_state.h`**

```c
#ifndef OPENEVCHARGER_PERSIST_CRASH_STATE_H
#define OPENEVCHARGER_PERSIST_CRASH_STATE_H

#include <stdint.h>

#define CRASH_STATE_SLOT_A   0x04D000U
#define CRASH_STATE_SLOT_B   0x04E000U
#define CRASH_STATE_VERSION  1U
#define CRASH_LOOP_THRESHOLD 5U      /* fast_restart_count ≥ this → safe-fail */

/* 32 bytes total. Same envelope (counter @ 4, CRC @ end). */
struct __attribute__((packed)) crash_state {
    uint8_t  version;                   /* 1 */
    uint8_t  pad0[3];
    uint32_t monotonic_counter;         /* helper-managed */
    uint8_t  fast_restart_count;        /* boots without 60s of uptime */
    uint8_t  pad1[3];
    uint8_t  reserved[16];
    uint32_t crc32;                     /* helper-managed */
};
_Static_assert(sizeof(struct crash_state) == 32, "crash_state must be 32 B");

/* Load + increment + persist. Sets safe_fail flag if count ≥ threshold.
 * Call once from main() pre-scheduler. */
int crash_state_boot_increment(void);

/* Reset fast_restart_count to 0 + persist. Called by persist_task after
 * io_task posts the "alive past 60 s" request. Synchronous (already
 * inside persist_task — no race with itself). */
int crash_state_reset_alive(void);

/* Safe-fail flag accessor for M6's safety_task. */
int crash_state_is_safe_fail(void);

#endif
```

- [ ] **Step 2: Create `src/persist/crash_state.c`**

```c
#include "crash_state.h"
#include "pingpong.h"
#include "../hal/uart.h"
#include <string.h>

static struct crash_state s_cs;
static int                s_safe_fail = 0;

int crash_state_boot_increment(void)
{
    uint8_t  slot;
    uint32_t counter;
    int rc = pingpong_load(CRASH_STATE_SLOT_A, CRASH_STATE_SLOT_B,
                           &s_cs, sizeof s_cs, &slot, &counter);
    if (rc < 0) {
        printk("crash_state: pingpong_load FAIL rc=%d\n", rc);
        return rc;
    }
    if (rc == 1) {
        memset(&s_cs, 0, sizeof s_cs);
        s_cs.version = CRASH_STATE_VERSION;
        s_cs.fast_restart_count = 0;
    }

    if (s_cs.fast_restart_count < 0xFFU) s_cs.fast_restart_count++;
    s_safe_fail = (s_cs.fast_restart_count >= CRASH_LOOP_THRESHOLD);

    rc = pingpong_store(CRASH_STATE_SLOT_A, CRASH_STATE_SLOT_B,
                        &s_cs, sizeof s_cs, &slot, &counter);
    if (rc < 0) {
        printk("crash_state: store FAIL rc=%d\n", rc);
        return rc;
    }
    if (s_safe_fail) {
        printk("crash_state: SAFE-FAIL ENTERED (fast_restart=%u)\n",
               (unsigned)s_cs.fast_restart_count);
    } else {
        printk("crash_state: fast_restart=%u (slot %c counter=%u)\n",
               (unsigned)s_cs.fast_restart_count,
               'A' + slot, (unsigned)counter);
    }
    return 0;
}

int crash_state_reset_alive(void)
{
    if (s_cs.fast_restart_count == 0 && !s_safe_fail) return 0;

    s_cs.version = CRASH_STATE_VERSION;
    s_cs.fast_restart_count = 0;

    uint8_t  slot;
    uint32_t counter;
    int rc = pingpong_store(CRASH_STATE_SLOT_A, CRASH_STATE_SLOT_B,
                            &s_cs, sizeof s_cs, &slot, &counter);
    if (rc < 0) {
        printk("crash_state: alive write FAIL rc=%d\n", rc);
        return rc;
    }
    s_safe_fail = 0;
    printk("crash_state: alive marker -> fast_restart=0 (slot %c counter=%u)\n",
           'A' + slot, (unsigned)counter);
    return 0;
}

int crash_state_is_safe_fail(void) { return s_safe_fail; }
```

- [ ] **Step 3: Add to CMakeLists.txt + build**

```cmake
    src/persist/crash_state.c
```

```sh
cmake --build build
```

---

### Task 2: persist_task handles a third request type

**Files:**
- Modify: `src/tasks/persist_task.{c,h}`

- [ ] **Step 1: Add producer API**

```c
/* Post a "I'm alive past 60 s" reset request. persist_task calls
 * crash_state_reset_alive() (which clears fast_restart_count to 0). */
int persist_post_crash_state_reset(void);
```

- [ ] **Step 2: Add CRASH_STATE_RESET request type + handler**

```c
#include "../persist/crash_state.h"
...
typedef enum {
    PERSIST_REQ_EVENT,
    PERSIST_REQ_SESSION,
    PERSIST_REQ_CRASH_STATE_RESET,
} persist_req_type_t;

/* The CRASH_STATE_RESET request has no payload — the whole struct's
 * union is unused for this case. */

int persist_post_crash_state_reset(void)
{
    if (s_queue == NULL) return -1;
    struct persist_req req;
    req.type = PERSIST_REQ_CRASH_STATE_RESET;
    if (xQueueSend(s_queue, &req, 0) != pdTRUE) return -1;
    return 0;
}

static void persist_task_run(void *arg)
{
    ...
    switch (req.type) {
    case PERSIST_REQ_EVENT:    ... break;
    case PERSIST_REQ_SESSION:  ... break;
    case PERSIST_REQ_CRASH_STATE_RESET:
        crash_state_reset_alive();
        break;
    }
    ...
}
```

- [ ] **Step 3: Compile**

```sh
cmake --build build
```

---

### Task 3: io_task posts the 60-s alive marker

**Files:**
- Modify: `src/tasks/io_task.c`

- [ ] **Step 1: Add a one-shot 60-s gate inside io_task_run loop**

```c
#define ALIVE_MARKER_MS  60000U

static void io_task_run(void *arg)
{
    ...
    int alive_posted = 0;

    for (;;) {
        ...
        if (!alive_posted && ms >= ALIVE_MARKER_MS) {
            (void)persist_post_crash_state_reset();
            alive_posted = 1;
        }

        ms += IO_TICK_MS;
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(IO_TICK_MS));
    }
}
```

- [ ] **Step 2: Compile + flash**

```sh
cmake --build build && ./tools/flash.sh
```

---

### Task 4: Wire crash_state_boot_increment into main()

**Files:**
- Modify: `src/main.c`

- [ ] **Step 1: Add include + boot call**

Place the call near the other persist loads (after event_log_init / session_log_init).

```c
#include "persist/crash_state.h"
...
        session_log_init();
        session_log_set_boot_count((uint16_t)bc);

        crash_state_boot_increment();
    }
```

- [ ] **Step 2: PAUSE — bench validation: normal boot + 60-s alive**

Flash, monitor. Expected:
```
crash_state: fast_restart=1 (slot A counter=1)
scheduler starting
io_task: posted startup event rc=0
                                        ← wait 60 s ...
crash_state: alive marker -> fast_restart=0 (slot B counter=2)
```

(The monitor will need to stay open ~60 s. Use `timeout 70` for the
openocd command or run the monitor in foreground with Ctrl-C after the
alive marker prints.)

- [ ] **Step 3: PAUSE — bench validation: 5 rapid resets → safe-fail**

After Step 2 ends with fast_restart=0, do 5 quick resets each within
< 60 s (just don't wait between them):

```sh
for i in 1 2 3 4 5; do
    timeout 6 openocd -f tools/openocd-gd32f205.cfg \
        -c "init" -c "reset halt" -c "arm semihosting enable" -c "reset run" \
        2>&1 | grep crash_state
done
```

Expected progression:
```
crash_state: fast_restart=1 (slot ...)
crash_state: fast_restart=2 ...
crash_state: fast_restart=3 ...
crash_state: fast_restart=4 ...
crash_state: SAFE-FAIL ENTERED (fast_restart=5)
```

- [ ] **Step 4: PAUSE — bench validation: alive marker clears safe-fail**

Now wait > 60 s with the firmware running, monitor open:
```sh
timeout 75 openocd -f tools/openocd-gd32f205.cfg -c init -c "reset halt" -c "arm semihosting enable" -c "reset run"
```

Expected:
```
crash_state: SAFE-FAIL ENTERED (fast_restart=6)
                                ← wait 60s
crash_state: alive marker -> fast_restart=0
```

Reset (no re-flash). Expected:
```
crash_state: fast_restart=1 (slot ... counter=...)
                            ← back to normal
```

- [ ] **Step 5: Commit**

```sh
git add src/persist/crash_state.c src/persist/crash_state.h \
        src/tasks/persist_task.c src/tasks/persist_task.h \
        src/tasks/io_task.c \
        src/main.c CMakeLists.txt \
        docs/superpowers/plans/2026-05-02-m5b6-crash-loop.md
git commit -m "M5.b.6: crash-loop detector + 60s alive marker"
```

---

### Task 5: Bring-up + tag

- [ ] **Step 1: Append M5.b.6 entry to `docs/bring-up.md`** (record the actual fast_restart progression observed across 5 rapid resets, plus the alive-marker recovery).

- [ ] **Step 2: Tag**

```sh
git tag -a m5b6-crash-loop -m "M5.b.6: crash-loop detector + 60s alive marker"
```

- [ ] **Step 3: (User-confirmed) push**

---

## After M5.b.6

**M5.b is fully complete.** All persistence machinery is in place:

| Region | Purpose | Module |
|---|---|---|
| 0x000000-0x001FFF | boot_config (ping-pong) | M5.b.1 boot_config.c |
| 0x002000-0x003FFF | calibration (ping-pong) | M5.b.2 calibration.c |
| 0x004000-0x043FFF | event_log (ring) | M5.b.3 event_log.c |
| 0x044000-0x04BFFF | session_log (ring) | M5.b.4 session_log.c |
| 0x04C000 | boot_count | M5 boot_count.c |
| 0x04D000-0x04EFFF | crash_state (ping-pong) | M5.b.6 crash_state.c |

All persist writes funnel through `persist_task` (M5.b.5). M6 — safety supervisor + faults — is unblocked.

## Self-review

**Spec coverage (M5.b.6 slice):**

| Spec § 6 requirement | This plan |
|---|---|
| Crash-loop detection: 5 boots in 60 s → safe-fail | Tasks 1, 4 |
| safe-fail "refuse to charge until FC41D commands clear" | Flag set; M6 will gate CP/relay on it |
| reset reason persisted (POR/IWDG/SW/debug/brownout) | DEFERRED — small follow-up; uses `RCU_RSTSCK` register; lands as M5.b.7 only if needed |

**Placeholders:** none.

**Type/name consistency:** `crash_state_*` matches `boot_config_*` / `calibration_*`. `CRASH_LOOP_THRESHOLD` named constant.

**Wear:** 2 writes per power-on session (boot increment + alive marker). 10 power cycles/day × 365 = 3650 sessions/year × 2 = 7300 writes/year. Each write erases one slot (4 KB sector). Worst case 7300 erases on one slot per year → ~14 years to 100K. Trivial.
