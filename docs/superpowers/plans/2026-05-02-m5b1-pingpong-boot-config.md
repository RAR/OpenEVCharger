# M5.b.1: Pingpong Helper + boot_config Record Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Generic ping-pong storage helper + first user (`boot_config`). After this milestone the firmware reads a persistent `fc41d_advertised_amps` value from W25Q `0x000000–0x001FFF` at boot, defaults it to `0` if both slots are invalid, and exposes a `boot_config_set_advertised_amps()` API for FC41D plumbing in later milestones. Power-cycle a unit with a stored value → see the same value reported on the next boot.

**Scope:** This is the first slice of the spec § 6 persistence layer (the M5.b chain). It builds the *generic* ping-pong helper that all four user records (boot_config, calibration, event_log head record, boot_count) will eventually share, then proves the pattern with `boot_config`. Calibration record, ring-buffered logs, persist_task queue, and crash-loop detection follow in M5.b.2 – M5.b.6.

**Architecture:** Two 4 KB sectors per logical record (slot A + slot B). At rest exactly one slot holds a CRC-valid record; the other is erased (all 0xFF). Writer:
1. Determines current-valid slot (or none).
2. Increments monotonic counter, recomputes CRC.
3. Erases the *other* slot, writes record there, verify-reads.
4. Erases the previous slot.

Reader:
1. Reads both slots.
2. Picks the slot whose CRC validates; if both valid (rare — recovery from crash mid-erase), picks the higher monotonic counter.
3. If neither valid, returns `not-found`.

The helper is record-agnostic: caller supplies a buffer + size; counter sits at offset 4 (after `version` + `pad0[3]`); CRC32 sits at `size − 4`. All persisted record types in the codebase follow this convention.

**Tech Stack:** Existing W25Q HAL (M4) + CRC32 helper (M5) — no new dependencies.

**Hardware preconditions:**
- M5 complete and tagged `m5-boot-count`. W25Q sector erase + page program proven on bench.
- Bench unit on USB 5 V is sufficient (no AC needed).

**Success criteria:**
1. First flash: monitor prints `boot_config: defaults written → slot A (counter=1, advertised_amps=0)`.
2. Reset (no re-flash): monitor prints `boot_config: loaded from slot A (counter=1, advertised_amps=0)`.
3. After calling the (test-only) `boot_config_set_advertised_amps(32)` from main: monitor prints `boot_config: stored → slot B (counter=2, advertised_amps=32)`. Reset → `boot_config: loaded from slot B (counter=2, advertised_amps=32)`.
4. PD4 still blinks; uptime ≥ 5 min, no resets, `boot_count` keeps incrementing each reboot (M5 chain unbroken).
5. Build: text < 18 KB.

---

## File Structure

```
OpenEVCharger/
├── src/
│   ├── persist/
│   │   ├── pingpong.c              # NEW — generic 2-slot helper
│   │   ├── pingpong.h
│   │   ├── boot_config.c           # NEW — first user of pingpong
│   │   └── boot_config.h
│   └── main.c                      # MODIFIED — load boot_config + one-shot test write
└── CMakeLists.txt                  # MODIFIED — add new .c
```

The spec lists `event_log.c/h` and `session_log.c/h` under `persist/`; both come in M5.b.3/4. `pingpong.c/h` is the foundation.

---

## Tasks

### Task 1: Pingpong helper

**Files:**
- Create: `src/persist/pingpong.{c,h}`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create `src/persist/pingpong.h`**

```c
#ifndef OPENBHZD_PERSIST_PINGPONG_H
#define OPENBHZD_PERSIST_PINGPONG_H

#include <stddef.h>
#include <stdint.h>

/* Two-slot ping-pong over W25Q. Each "logical record" lives in one of
 * two 4 KB sectors. At rest exactly one slot is CRC-valid; the other is
 * erased.
 *
 * Record convention (enforced by this helper, no offsets passed in):
 *   bytes [0]            = version (caller-managed)
 *   bytes [1..3]         = pad
 *   bytes [4..7]         = u32 monotonic_counter (managed by helper)
 *   bytes [8..size-5]    = caller payload
 *   bytes [size-4..size-1] = u32 crc32 (covers bytes 0..size-5)
 *
 * `record_size` MUST be ≥ 12 (room for header + counter + crc) and
 * ≤ 256 (one W25Q page program). All records in this codebase are 32 B.
 */

/* Read the newer-valid slot's bytes into out_buf.
 *   0  → out_buf populated, *out_slot = 0 (A) or 1 (B)
 *   1  → both slots invalid; out_buf zeroed, *out_slot undefined
 *  <0  → W25Q error
 */
int pingpong_load(uint32_t addr_a, uint32_t addr_b,
                  void *out_buf, size_t record_size,
                  uint8_t *out_slot, uint32_t *out_counter);

/* Erase the *other* slot (relative to current valid one), program the
 * supplied record there, verify-read. The helper updates the counter
 * (= prior + 1, or 1 on first write) and CRC inline; caller must
 * leave those bytes uninitialized (or zero — they'll be overwritten).
 *
 *   0  → success, *out_slot = slot just written, *out_counter = its value
 *  <0  → W25Q error or verify mismatch
 */
int pingpong_store(uint32_t addr_a, uint32_t addr_b,
                   void *record, size_t record_size,
                   uint8_t *out_slot, uint32_t *out_counter);

#endif
```

- [ ] **Step 2: Create `src/persist/pingpong.c`**

```c
#include "pingpong.h"
#include "crc.h"
#include "../hal/w25q.h"
#include <string.h>

#define COUNTER_OFF   4U

static uint32_t le32_load(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void le32_store(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v       & 0xFF);
    p[1] = (uint8_t)((v >>  8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* Read sector at `addr` into `buf` and check CRC. Returns 1 if valid,
 * 0 if invalid (blank or CRC mismatch), <0 on W25Q error. */
static int read_and_validate(uint32_t addr, uint8_t *buf, size_t size,
                             uint32_t *out_counter)
{
    if (w25q_read(addr, buf, size) != 0) return -1;

    uint32_t stored_crc = le32_load(buf + size - 4);
    uint32_t expected   = crc32(buf, size - 4);
    if (stored_crc != expected) return 0;

    *out_counter = le32_load(buf + COUNTER_OFF);
    return 1;
}

int pingpong_load(uint32_t addr_a, uint32_t addr_b,
                  void *out_buf, size_t record_size,
                  uint8_t *out_slot, uint32_t *out_counter)
{
    if (record_size < 12U || record_size > 256U) return -1;

    uint8_t buf_a[256], buf_b[256];
    uint32_t cnt_a = 0, cnt_b = 0;

    int va = read_and_validate(addr_a, buf_a, record_size, &cnt_a);
    int vb = read_and_validate(addr_b, buf_b, record_size, &cnt_b);
    if (va < 0 || vb < 0) return -1;

    if (va && (!vb || cnt_a >= cnt_b)) {
        memcpy(out_buf, buf_a, record_size);
        if (out_slot)    *out_slot = 0;
        if (out_counter) *out_counter = cnt_a;
        return 0;
    }
    if (vb) {
        memcpy(out_buf, buf_b, record_size);
        if (out_slot)    *out_slot = 1;
        if (out_counter) *out_counter = cnt_b;
        return 0;
    }
    memset(out_buf, 0, record_size);
    return 1;
}

int pingpong_store(uint32_t addr_a, uint32_t addr_b,
                   void *record, size_t record_size,
                   uint8_t *out_slot, uint32_t *out_counter)
{
    if (record_size < 12U || record_size > 256U) return -1;

    uint8_t *r = (uint8_t *)record;

    /* Discover current state to decide where to write. */
    uint8_t scratch[256];
    uint32_t cnt_a = 0, cnt_b = 0;
    int va = read_and_validate(addr_a, scratch, record_size, &cnt_a);
    int vb = read_and_validate(addr_b, scratch, record_size, &cnt_b);
    if (va < 0 || vb < 0) return -1;

    uint32_t cur_counter = 0;
    int      cur_slot    = -1;          /* -1 = none */
    if (va && (!vb || cnt_a >= cnt_b)) { cur_slot = 0; cur_counter = cnt_a; }
    else if (vb)                        { cur_slot = 1; cur_counter = cnt_b; }

    int target_slot = (cur_slot == 0) ? 1 : 0;   /* 0 if none → write A first */
    uint32_t target_addr = (target_slot == 0) ? addr_a : addr_b;
    uint32_t other_addr  = (target_slot == 0) ? addr_b : addr_a;

    /* Stamp counter + CRC into caller's record. */
    uint32_t new_counter = cur_counter + 1U;
    le32_store(r + COUNTER_OFF, new_counter);
    uint32_t crc = crc32(r, record_size - 4U);
    le32_store(r + record_size - 4U, crc);

    /* Write to the (already-erased or to-be-erased) target slot. */
    if (w25q_erase_sector(target_addr) != 0) return -2;
    if (w25q_program(target_addr, r, record_size) != 0) return -3;

    /* Verify-read. */
    if (w25q_read(target_addr, scratch, record_size) != 0) return -4;
    if (memcmp(scratch, r, record_size) != 0) return -5;

    /* Erase the prior slot (no-op if it was already blank). */
    if (cur_slot != -1) {
        if (w25q_erase_sector(other_addr) != 0) return -6;
    }

    if (out_slot)    *out_slot = (uint8_t)target_slot;
    if (out_counter) *out_counter = new_counter;
    return 0;
}
```

- [ ] **Step 3: Add to `CMakeLists.txt` `APP_SRCS`**

```cmake
    src/persist/pingpong.c
```

- [ ] **Step 4: Compile**

```sh
cmake --build build
```

Expected: clean build, text grows by ~600 B.

---

### Task 2: boot_config record

**Files:**
- Create: `src/persist/boot_config.{c,h}`

- [ ] **Step 1: Create `src/persist/boot_config.h`**

```c
#ifndef OPENBHZD_PERSIST_BOOT_CONFIG_H
#define OPENBHZD_PERSIST_BOOT_CONFIG_H

#include <stdint.h>

#define BOOT_CONFIG_SLOT_A  0x000000U
#define BOOT_CONFIG_SLOT_B  0x001000U
#define BOOT_CONFIG_VERSION 1U

/* 32 bytes total. Layout matches spec § 6 with the addition of a
 * `monotonic_counter` field at offset 4 (managed by pingpong helper). */
struct __attribute__((packed)) boot_config {
    uint8_t  version;                   /* 1 */
    uint8_t  pad0[3];
    uint32_t monotonic_counter;         /* helper-managed */
    uint8_t  fc41d_advertised_amps;     /* 0 = unset → fall back to DIP1 */
    uint8_t  pad1[3];
    uint8_t  reserved[12];
    uint32_t crc32;                     /* helper-managed */
};
_Static_assert(sizeof(struct boot_config) == 32, "boot_config must be 32 B");

/* Load the current boot_config into the in-RAM cache. If both slots
 * are invalid, writes a defaults record to slot A. Call once at boot
 * before any other boot_config_* function. Returns 0 on success,
 * <0 on W25Q error. */
int boot_config_load(void);

/* Accessor. Returns 0 (= unset) until boot_config_load() runs. */
uint8_t boot_config_advertised_amps(void);

/* Update advertised amps, then ping-pong-write to W25Q. Returns 0
 * on success, <0 on W25Q error. */
int boot_config_set_advertised_amps(uint8_t amps);

#endif
```

- [ ] **Step 2: Create `src/persist/boot_config.c`**

```c
#include "boot_config.h"
#include "pingpong.h"
#include "../hal/uart.h"
#include <string.h>

static struct boot_config s_cfg;

int boot_config_load(void)
{
    uint8_t  slot;
    uint32_t counter;
    int rc = pingpong_load(BOOT_CONFIG_SLOT_A, BOOT_CONFIG_SLOT_B,
                           &s_cfg, sizeof s_cfg, &slot, &counter);
    if (rc < 0) {
        printk("boot_config: W25Q read FAIL rc=%d\n", rc);
        return rc;
    }
    if (rc == 1) {
        /* Both slots invalid → write defaults. */
        memset(&s_cfg, 0, sizeof s_cfg);
        s_cfg.version = BOOT_CONFIG_VERSION;
        s_cfg.fc41d_advertised_amps = 0;

        rc = pingpong_store(BOOT_CONFIG_SLOT_A, BOOT_CONFIG_SLOT_B,
                            &s_cfg, sizeof s_cfg, &slot, &counter);
        if (rc < 0) {
            printk("boot_config: defaults write FAIL rc=%d\n", rc);
            return rc;
        }
        printk("boot_config: defaults written -> slot %c (counter=%u, advertised_amps=%u)\n",
               'A' + slot, (unsigned)counter,
               (unsigned)s_cfg.fc41d_advertised_amps);
        return 0;
    }

    if (s_cfg.version != BOOT_CONFIG_VERSION) {
        printk("boot_config: unknown version=%u, ignoring\n", s_cfg.version);
    }
    printk("boot_config: loaded from slot %c (counter=%u, advertised_amps=%u)\n",
           'A' + slot, (unsigned)counter,
           (unsigned)s_cfg.fc41d_advertised_amps);
    return 0;
}

uint8_t boot_config_advertised_amps(void)
{
    return s_cfg.fc41d_advertised_amps;
}

int boot_config_set_advertised_amps(uint8_t amps)
{
    if (s_cfg.fc41d_advertised_amps == amps) return 0; /* idempotent */

    s_cfg.version = BOOT_CONFIG_VERSION;
    s_cfg.fc41d_advertised_amps = amps;

    uint8_t  slot;
    uint32_t counter;
    int rc = pingpong_store(BOOT_CONFIG_SLOT_A, BOOT_CONFIG_SLOT_B,
                            &s_cfg, sizeof s_cfg, &slot, &counter);
    if (rc < 0) {
        printk("boot_config: store FAIL rc=%d\n", rc);
        return rc;
    }
    printk("boot_config: stored -> slot %c (counter=%u, advertised_amps=%u)\n",
           'A' + slot, (unsigned)counter, (unsigned)amps);
    return 0;
}
```

- [ ] **Step 3: Add to `CMakeLists.txt` `APP_SRCS`**

```cmake
    src/persist/boot_config.c
```

- [ ] **Step 4: Compile**

```sh
cmake --build build
```

Expected: clean build, text grows ~400 B more.

---

### Task 3: Wire into main()

**Files:**
- Modify: `src/main.c`

- [ ] **Step 1: Add include + load call in `main.c`**

Place the load call right after the `boot_count_increment()` block — both depend on W25Q being initialised, and boot_config wants to log alongside boot_count.

```c
#include "persist/boot_config.h"
...
        uint32_t bc = boot_count_increment();
        if (bc == 0xFFFFFFFFu) printk("W25Q: boot_count write FAIL\n");
        else                   printk("boot_count = %u\n", (unsigned)bc);

        if (boot_config_load() < 0) {
            printk("boot_config: load failed; defaults uninitialised\n");
        }
    }
```

- [ ] **Step 2: Compile + flash**

```sh
cmake --build build
./tools/flash.sh
```

- [ ] **Step 3: PAUSE — bench validation: first boot writes defaults**

Run monitor and capture output:

```sh
./tools/openocd-monitor.sh
```

Expected first-boot output (slot A blank because we haven't written yet OR carries the M4 self-test's leftover bytes — both are CRC-invalid → defaults path):

```
W25Q: JEDEC ID = 0xc84017 (...)
boot_count = N
boot_config: defaults written -> slot A (counter=1, advertised_amps=0)
scheduler starting
```

If the line reads `loaded from slot A` instead of `defaults written`, sector 0 happened to contain a CRC-valid record from prior testing — erase it and retry:

```sh
openocd -f tools/openocd-gd32f205.cfg -c "init; halt; flash erase_sector 0 0 1; reset run; exit"
```

(That uses the GD32 internal flash bank — wrong for our purposes since boot_config lives in W25Q. Instead, add a one-shot debug sequence to main that erases sector 0 + 0x1000 only on demand — or accept the existing record and verify Step 4 still works; that's the cleaner path.)

- [ ] **Step 4: PAUSE — bench validation: reload after reset**

Reset without re-flashing:

```sh
openocd -f tools/openocd-gd32f205.cfg -c "init; reset run; exit"
./tools/openocd-monitor.sh
```

Expected:
```
boot_count = N+1
boot_config: loaded from slot A (counter=1, advertised_amps=0)
scheduler starting
```

- [ ] **Step 5: PAUSE — bench validation: store-and-reload**

Add a one-shot test write after `boot_config_load()` (gated so we don't run it forever):

```c
        boot_config_load();
        if (boot_config_advertised_amps() == 0) {
            (void)boot_config_set_advertised_amps(32);
        }
```

Compile, flash, monitor:
```
boot_config: loaded from slot A (counter=1, advertised_amps=0)
boot_config: stored -> slot B (counter=2, advertised_amps=32)
```

Reset (no re-flash) → expect:
```
boot_config: loaded from slot B (counter=2, advertised_amps=32)
```

(The next boot's `boot_config_advertised_amps() == 0` test is now false, so no second store fires.)

Reset again → still slot B / counter=2 (idempotent — value unchanged, store skipped).

- [ ] **Step 6: Remove the test-only block from main()**

Once Step 5 passes, delete the `if (boot_config_advertised_amps() == 0) ...` lines. The store path is exercised; production `boot_config_set_advertised_amps()` callers come in M9 (FC41D TLV).

- [ ] **Step 7: Commit**

```sh
git add src/persist/pingpong.c src/persist/pingpong.h \
        src/persist/boot_config.c src/persist/boot_config.h \
        src/main.c CMakeLists.txt
git commit -m "M5.b.1: ping-pong helper + boot_config record"
```

---

### Task 4: Bring-up log + tag

- [ ] **Step 1: Append M5.b.1 entry to `docs/bring-up.md`**

Mirror prior milestones. Record observed slot/counter sequence across the two test reboots and the post-store reload.

- [ ] **Step 2: Tag**

```sh
git tag -a m5b1-pingpong-boot-config \
        -m "M5.b.1: generic ping-pong helper + boot_config record"
```

- [ ] **Step 3: (User-confirmed) push**

---

## After M5.b.1

1. **Update memory** if any pingpong behaviour surprised us (e.g. M4 leftover bytes happened to CRC-validate, or sector erase behaviour deviated).
2. **Plan M5.b.2 — calibration record.** Same record convention, slots 0x002000/0x003000. Migrates the M3 hard-coded CP anchors (slope=3540/459, anchor=1462) into a `calibration` record, with `calibration_load_or_default()` returning current values and `calibration_set_cp_anchors(slope_num, slope_den, anchor)` storing them. `adc_inject.c` reads anchors from the calibration cache in its ISR — must avoid cache thrash, so the compute path uses `volatile` cached integers updated by `calibration_set_*`.

## Self-review

**Spec coverage (M5.b.1 slice):**

| Spec § 6 requirement | This plan |
|---|---|
| boot_config ping-pong (4 KB × 2) | Tasks 1–3 |
| Reader: CRC-validate + monotonic-counter tiebreak | Task 1 (`pingpong_load`) |
| Writer: erase target → program → verify → erase prior | Task 1 (`pingpong_store`) |
| Both slots invalid → write defaults to slot A | Task 2 (`boot_config_load`) |
| calibration ping-pong | DEFERRED to M5.b.2 |
| event_log + session_log | DEFERRED to M5.b.3/4 |
| persist_task queue | DEFERRED to M5.b.5 |
| crash-loop detector | DEFERRED to M5.b.6 |
| reset reason persisted | DEFERRED (folded into M5.b.6 / M6) |

**Placeholders:** none.

**Type/name consistency:** `pingpong_load/store`, `boot_config_load/set/advertised_amps`, `BOOT_CONFIG_SLOT_A/B`, `BOOT_CONFIG_VERSION` consistent across files. Counter offset (`COUNTER_OFF=4`) and CRC offset (record_size − 4) are convention; documented in `pingpong.h`.

**Wear:** Each `boot_config_set_advertised_amps(new_value)` call costs 2 sector erases (1 write target + 1 prior). Realistic call rate ≤ 10/year (FC41D config changes). Negligible against W25Q's 100 K cycles/sector.
