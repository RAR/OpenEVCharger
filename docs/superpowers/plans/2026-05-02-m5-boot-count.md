# M5: Boot-Count Persistence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist a boot counter across power cycles in W25Q sector 0x04C000. At every cold boot the firmware reads the prior count, increments it, and writes the new value back; printk reports `boot_count = N`. Power-cycle 5× → see `boot_count = 5`.

**Scope:** This is a **scoped subset of spec § 6 / § 9 M5**. The spec calls for the full persistence layer in M5: boot_config + calibration ping-pong, event_log + session_log ring buffers, scan-on-boot head discovery. The SUCCESS criterion in spec § 9 M5 is narrower ("boot_count = 5 after 5 reboots"); this plan satisfies that and leaves the rest for an M5.b in a future session. Once M5.b lands, boot_count gets folded into the event_log scheme and this dedicated record disappears.

**Architecture:** Single 32-byte struct stored in sector `0x04C000` (the spec's dedicated `boot_count + last_fault` sector). At boot, read 32 bytes; if CRC32 valid, increment; if invalid, start fresh at 1. Then erase the sector and write the new record. Synchronous-blocking, run from `main()` pre-scheduler. No `persist_task` queue yet — that comes when other tasks need to enqueue writes (M5.b).

**Tech Stack:** Existing W25Q HAL from M4, new CRC32 helper.

**Hardware preconditions:**
- M4 complete and tagged `m4-spi3-w25q`. W25Q round-trip validated.
- Bench unit on USB 5 V is sufficient.

**Success criterion:**
1. First boot after flashing prints `boot_count = 1`.
2. Reset (via SWD `reset run` or power cycle) → second boot prints `boot_count = 2`.
3. Five power cycles → fifth boot prints `boot_count = 5`.
4. PD4 still blinks; uptime ≥ 5 min, no resets.
5. Build: text < 16 KB.

---

## File Structure

```
OpenEVCharger/
├── src/
│   ├── persist/
│   │   ├── crc.c                  # NEW — crc32
│   │   ├── crc.h
│   │   ├── boot_count.c           # NEW — read/increment/write
│   │   └── boot_count.h
│   └── main.c                     # MODIFIED — call boot_count_increment_and_log()
└── CMakeLists.txt                 # MODIFIED — add new .c
```

---

## Tasks

### Task 1: CRC32 helper

**Files:**
- Create: `src/persist/crc.{c,h}`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create `src/persist/crc.h`**

```c
#ifndef OPENEVCHARGER_PERSIST_CRC_H
#define OPENEVCHARGER_PERSIST_CRC_H

#include <stddef.h>
#include <stdint.h>

/* IEEE 802.3 CRC32 (polynomial 0xEDB88320, reflected). Standard
 * crc32 used by zlib / Ethernet / W25Q config records. Initial
 * value 0xFFFFFFFF, final XOR 0xFFFFFFFF. Software-only — no
 * peripheral CRC unit. Bit-banged byte loop; ~50 cycles/byte. */
uint32_t crc32(const void *data, size_t len);

#endif
```

- [ ] **Step 2: Create `src/persist/crc.c`**

```c
#include "crc.h"

uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; ++i) {
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
        }
    }
    return ~crc;
}
```

---

### Task 2: boot_count

**Files:**
- Create: `src/persist/boot_count.{c,h}`

- [ ] **Step 1: Create `src/persist/boot_count.h`**

```c
#ifndef OPENEVCHARGER_PERSIST_BOOT_COUNT_H
#define OPENEVCHARGER_PERSIST_BOOT_COUNT_H

#include <stdint.h>

/* Read the current boot_count from W25Q, increment, write back, return
 * the new (post-increment) value. On invalid/blank read, returns 1.
 * On W25Q error (erase or program timeout), returns 0xFFFFFFFF.
 *
 * Called once from main() before tasks start. */
uint32_t boot_count_increment(void);

#endif
```

- [ ] **Step 2: Create `src/persist/boot_count.c`**

```c
#include "boot_count.h"
#include "crc.h"
#include "../hal/w25q.h"
#include <string.h>

#define BOOT_COUNT_SECTOR  0x04C000U

/* 32-byte record. Layout matches the spec's "boot_count + last_fault"
 * description but only `count` is used in M5 — `last_fault` will be
 * populated by M6's safety supervisor. */
struct __attribute__((packed)) boot_count_record {
    uint8_t  version;          /* 1 */
    uint8_t  pad0[3];
    uint32_t count;
    uint16_t last_fault_id;    /* M6: most recent latched fault */
    uint8_t  reserved[18];
    uint32_t crc32;
};

uint32_t boot_count_increment(void)
{
    struct boot_count_record rec;
    w25q_read(BOOT_COUNT_SECTOR, &rec, sizeof rec);

    uint32_t computed = crc32(&rec, sizeof rec - sizeof(uint32_t));
    uint32_t cur = (rec.version == 1 && rec.crc32 == computed) ? rec.count : 0;

    /* Build new record */
    memset(&rec, 0, sizeof rec);
    rec.version = 1;
    rec.count   = cur + 1;
    rec.crc32   = crc32(&rec, sizeof rec - sizeof(uint32_t));

    if (w25q_erase_sector(BOOT_COUNT_SECTOR) != 0) return 0xFFFFFFFFu;
    if (w25q_program(BOOT_COUNT_SECTOR, &rec, sizeof rec) != 0) return 0xFFFFFFFFu;

    return rec.count;
}
```

---

### Task 3: Wire into main()

**Files:**
- Modify: `src/main.c`, `CMakeLists.txt`

- [ ] **Step 1: Add to `CMakeLists.txt` `APP_SRCS`**

```cmake
    src/persist/crc.c
    src/persist/boot_count.c
```

- [ ] **Step 2: Replace M4 self-test with boot_count call in `main.c`**

The M4 self-test was always one-shot; remove it now that the W25Q chain is proven. Also remove the M4 test function.

```c
#include "persist/boot_count.h"
...
/* (Remove m4_self_test entirely — keep its include of hal/w25q.h) */
...
    spi3_init();
    if (w25q_init() != 0) {
        printk("W25Q: JEDEC init FAIL — boot_count not persisted\n");
    } else {
        uint32_t bc = boot_count_increment();
        if (bc == 0xFFFFFFFFu) printk("W25Q: boot_count write FAIL\n");
        else                   printk("boot_count = %u\n", (unsigned)bc);
    }

    safety_task_create();
```

- [ ] **Step 3: Build**

```sh
cmake --build build
```

Expected: text grows by ~600 B (CRC32 + boot_count.c) but the M4 self-test removal trims ~1000 B → net shrink.

- [ ] **Step 4: PAUSE — bench validation: 5 power cycles**

```sh
./tools/flash.sh
./tools/openocd-monitor.sh
```

Expected first boot output:
```
W25Q: JEDEC ID = 0xc84017 (unrecognised — non-Winbond or different capacity)
boot_count = 1
scheduler starting
```

Stop the monitor (Ctrl-C). Run a reset (no re-flash):
```sh
openocd -f tools/openocd-gd32f205.cfg -c "init; reset run; exit"
./tools/openocd-monitor.sh
```

Expect `boot_count = 2`. Repeat reset+monitor 3 more times → `boot_count = 5`.

If stuck at 1 every time, the W25Q write isn't persisting → debug via halt-and-peek of sector 0x04C000.

If counts skip / regress, CRC math is wrong → check the `sizeof rec - sizeof(uint32_t)` reach (32 - 4 = 28 bytes covered).

- [ ] **Step 5: Commit**

```sh
git add src/persist/crc.c src/persist/crc.h src/persist/boot_count.c src/persist/boot_count.h src/main.c CMakeLists.txt
git commit -m "M5: boot_count persisted in W25Q sector 0x04C000"
```

---

### Task 4: Bring-up log + tag

- [ ] **Step 1: Append M5 entry to `docs/bring-up.md`** (mirror prior milestones; record the actual sequence of boot_counts observed across the 5 reboots).

- [ ] **Step 2: Tag**

```sh
git tag -a m5-boot-count -m "M5 (scoped): boot_count persisted across reboots; full persistence machinery deferred to M5.b"
```

- [ ] **Step 3: (User-confirmed) push**

---

## After M5

1. **Update memory** with any new boot-count behaviour (e.g. if PD0 reset cycles count differently than power cycles).
2. **Plan M5.b** for the full persistence machinery: ping-pong helper for boot_config + calibration, event_log scan-on-boot + append, session_log append, integrate with persist_task's queue. M5.b lands before M6 because M6's fault state machine writes to event_log on every fault raise.

## Self-review

**Spec coverage (scoped M5):**

| Spec requirement | This plan |
|---|---|
| boot_config ping-pong | DEFERRED to M5.b |
| calibration ping-pong | DEFERRED to M5.b |
| event_log + session_log append | DEFERRED to M5.b |
| reset reason persisted | DEFERRED to M5.b |
| **SUCCESS = boot_count = 5 after 5 reboots** | Tasks 1–3 |

**No placeholders** in the M5 deliverable. The deferred items are explicitly out-of-scope, not stubbed.

**Type/name consistency:** `boot_count_increment` / `crc32` / `BOOT_COUNT_SECTOR` consistent across files.
