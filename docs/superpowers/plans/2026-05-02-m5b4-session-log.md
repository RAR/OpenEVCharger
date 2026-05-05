# M5.b.4: session_log Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persistent ring buffer for charging-session summary records. After this milestone the firmware can `session_log_append(rec)` to record an ended session; appended records survive reboot; the next boot's scan finds the most-recent record and resumes appending. Spec § 6 calls this `session_log` — 8 sectors at `0x044000`–`0x04BFFF`, ~1024 record capacity.

**Scope:** session_log only. Same shape as M5.b.3's event_log (scan-on-boot + sector-aware append) but with a different region, different record layout, and a different latest-record tiebreak. **No refactoring of event_log into a generic ring helper** — there are only two ring callers and they differ in the tiebreak key, so duplication is cheaper than abstraction (per YAGNI). If a third ring lands, refactor then.

**Architecture:**
- 32 KB region (8 × 4 KB sectors) of 32-byte records (128 per sector → 1024 total).
- Same scan-on-boot pattern as event_log: classify each slot as valid/blank/corrupt, find the most-recent valid record, head = next slot.
- **Latest tiebreak: (boot_count, start_ts).** Spec's `session_record` did not include a boot counter; this plan adds `u16 boot_count` (matching `event_record`'s field) so the scan picks a stable latest across power cycles. Without it, an unsynced wall clock could roll back start_ts across reboots and break head discovery.
- CRC16-CCITT-FALSE — same poly as event_log.

**Tech Stack:** Reuses M5.b.3's CRC16 helper + W25Q HAL (M4).

**Hardware preconditions:**
- M5.b.3 tagged. event_log proven on bench.
- Bench unit on USB 5 V is sufficient.

**Success criteria:**
1. First boot (region empty/non-session-record): `session_log: scan complete, head=0x044000 slot=0`.
2. 2 programmatic `session_log_append()` calls from a one-shot main self-test → head_slot=2.
3. Reset → scan finds 2 valid records, head_slot still 2.
4. Read-back via `session_log_read_nth(0..1)` returns the test records with correct boot_count + start_ts + mwh_delivered.
5. Build: text < 18 KB.

---

## File Structure

```
OpenEVCharger/
├── src/
│   ├── persist/
│   │   ├── session_log.c           # NEW
│   │   └── session_log.h
│   └── main.c                      # MODIFIED — call session_log_init + test appends
└── CMakeLists.txt                  # MODIFIED
```

---

## Tasks

### Task 1: session_log

**Files:**
- Create: `src/persist/session_log.{c,h}`

- [ ] **Step 1: Create `src/persist/session_log.h`**

```c
#ifndef OPENBHZD_PERSIST_SESSION_LOG_H
#define OPENBHZD_PERSIST_SESSION_LOG_H

#include <stddef.h>
#include <stdint.h>

#define SESSION_LOG_BASE                  0x044000U
#define SESSION_LOG_SECTORS               8U
#define SESSION_LOG_SECTOR_SIZE           4096U
#define SESSION_LOG_REGION_SIZE           (SESSION_LOG_SECTORS * SESSION_LOG_SECTOR_SIZE)
#define SESSION_LOG_RECORD_SIZE           32U
#define SESSION_LOG_RECORDS_PER_SECTOR    (SESSION_LOG_SECTOR_SIZE / SESSION_LOG_RECORD_SIZE)
#define SESSION_LOG_TOTAL_RECORDS         (SESSION_LOG_SECTORS * SESSION_LOG_RECORDS_PER_SECTOR)

/* 32 bytes total. Layout per spec § 6 with the addition of a u16
 * boot_count for cross-reboot ring tiebreak (matches event_record). */
struct __attribute__((packed)) session_record {
    uint32_t start_ts;
    uint32_t end_ts;
    uint32_t mwh_delivered;
    uint8_t  end_reason;
    uint8_t  j1772_max_state_seen;
    uint16_t fault_count;
    uint16_t max_temp_dC;
    uint16_t boot_count;
    uint8_t  reserved[8];
    uint16_t crc16;
};
_Static_assert(sizeof(struct session_record) == 32, "session_record must be 32 B");

/* Scan to find the head. Logs result. Returns 0 always. */
int session_log_init(void);

/* Caller supplies current boot_count, used to stamp every appended
 * record. Set once at boot after boot_count_increment(). */
void session_log_set_boot_count(uint16_t boot_count);

/* Append a record. Caller fills all fields EXCEPT boot_count and crc16
 * (helper sets those). Returns 0 on success, <0 on W25Q error. */
int session_log_append(struct session_record *rec);

/* Debug: read n-th slot. 0 = valid, 1 = blank/corrupt, <0 = range error. */
int session_log_read_nth(uint32_t n, struct session_record *out_rec);

uint32_t session_log_head_slot(void);

#endif
```

- [ ] **Step 2: Create `src/persist/session_log.c`**

```c
#include "session_log.h"
#include "crc16.h"
#include "../hal/w25q.h"
#include "../hal/uart.h"
#include <string.h>

static uint32_t s_head_slot   = 0;
static uint16_t s_boot_count  = 0;
static int      s_initialized = 0;

static uint32_t slot_to_addr(uint32_t slot)
{
    return SESSION_LOG_BASE + slot * SESSION_LOG_RECORD_SIZE;
}

static int classify(const struct session_record *rec)
{
    const uint8_t *b = (const uint8_t *)rec;
    int all_ff = 1;
    for (size_t i = 0; i < sizeof *rec; ++i) {
        if (b[i] != 0xFFu) { all_ff = 0; break; }
    }
    if (all_ff) return 0;
    uint16_t expected = crc16_ccitt(rec, sizeof *rec - 2U);
    return (rec->crc16 == expected) ? 1 : -1;
}

int session_log_init(void)
{
    uint32_t valid = 0, corrupt = 0, blank = 0;
    uint32_t latest_boot = 0;
    uint32_t latest_ts   = 0;
    uint32_t latest_slot = 0;
    int      seen_valid  = 0;

    static uint8_t sector_buf[SESSION_LOG_SECTOR_SIZE];

    for (uint32_t s = 0; s < SESSION_LOG_SECTORS; ++s) {
        w25q_read(SESSION_LOG_BASE + s * SESSION_LOG_SECTOR_SIZE,
                  sector_buf, SESSION_LOG_SECTOR_SIZE);
        for (uint32_t r = 0; r < SESSION_LOG_RECORDS_PER_SECTOR; ++r) {
            const struct session_record *rec =
                (const struct session_record *)(sector_buf + r * SESSION_LOG_RECORD_SIZE);
            int c = classify(rec);
            if (c == 0) { ++blank; continue; }
            if (c < 0) { ++corrupt; continue; }
            ++valid;

            uint32_t key_boot = rec->boot_count;
            uint32_t key_ts   = rec->start_ts;
            uint32_t slot     = s * SESSION_LOG_RECORDS_PER_SECTOR + r;
            if (!seen_valid ||
                key_boot > latest_boot ||
                (key_boot == latest_boot && key_ts >= latest_ts)) {
                latest_boot = key_boot;
                latest_ts   = key_ts;
                latest_slot = slot;
                seen_valid  = 1;
            }
        }
    }

    s_head_slot = seen_valid
        ? (latest_slot + 1U) % SESSION_LOG_TOTAL_RECORDS
        : 0;

    printk("session_log: scan complete: valid=%u corrupt=%u blank=%u head=0x%06x slot=%u\n",
           (unsigned)valid, (unsigned)corrupt, (unsigned)blank,
           (unsigned)slot_to_addr(s_head_slot), (unsigned)s_head_slot);

    s_initialized = 1;
    return 0;
}

void session_log_set_boot_count(uint16_t boot_count)
{
    s_boot_count = boot_count;
}

int session_log_append(struct session_record *rec)
{
    if (!s_initialized || rec == NULL) return -1;

    rec->boot_count = s_boot_count;
    rec->crc16 = crc16_ccitt(rec, sizeof *rec - 2U);

    uint32_t addr = slot_to_addr(s_head_slot);

    if (s_head_slot % SESSION_LOG_RECORDS_PER_SECTOR == 0U) {
        struct session_record probe;
        w25q_read(addr, &probe, sizeof probe);
        const uint8_t *b = (const uint8_t *)&probe;
        int needs_erase = 0;
        for (size_t i = 0; i < sizeof probe; ++i) {
            if (b[i] != 0xFFu) { needs_erase = 1; break; }
        }
        if (needs_erase && w25q_erase_sector(addr) != 0) return -2;
    }

    if (w25q_program(addr, rec, sizeof *rec) != 0) return -3;

    s_head_slot = (s_head_slot + 1U) % SESSION_LOG_TOTAL_RECORDS;
    return 0;
}

int session_log_read_nth(uint32_t n, struct session_record *out_rec)
{
    if (n >= SESSION_LOG_TOTAL_RECORDS || out_rec == NULL) return -1;
    w25q_read(slot_to_addr(n), out_rec, sizeof *out_rec);
    return classify(out_rec) == 1 ? 0 : 1;
}

uint32_t session_log_head_slot(void) { return s_head_slot; }
```

- [ ] **Step 3: Add to CMakeLists.txt + build**

```cmake
    src/persist/session_log.c
```

```sh
cmake --build build
```

---

### Task 2: Wire main + bench validate

- [ ] **Step 1: Add include + init + 2 self-test appends**

```c
#include "persist/session_log.h"
...
        event_log_init();
        event_log_set_boot_count((uint16_t)bc);

        session_log_init();
        session_log_set_boot_count((uint16_t)bc);

        if (session_log_head_slot() == 0) {
            for (int i = 0; i < 2; ++i) {
                struct session_record rec = {
                    .start_ts        = (uint32_t)(0x20000000U + i * 1000U),
                    .end_ts          = (uint32_t)(0x20000200U + i * 1000U),
                    .mwh_delivered   = (uint32_t)(7000U * (i + 1U)),
                    .end_reason      = (uint8_t)(0xC0U + i),
                    .j1772_max_state_seen = 'C',
                    .fault_count     = 0,
                    .max_temp_dC     = (uint16_t)(450U + i * 10U),
                };
                if (session_log_append(&rec) != 0) {
                    printk("session_log: test append %d FAIL\n", i);
                }
            }
            printk("session_log: 2 self-test records appended; head_slot=%u\n",
                   (unsigned)session_log_head_slot());
        }
```

- [ ] **Step 2: PAUSE — bench validation: first boot writes 2 records**

```sh
cmake --build build && ./tools/flash.sh
timeout 8 openocd -f tools/openocd-gd32f205.cfg -c 'init' -c 'reset halt' -c 'arm semihosting enable' -c 'reset run'
```

Expected:
```
session_log: scan complete: valid=0 corrupt=N blank=M head=0x044000 slot=0
session_log: 2 self-test records appended; head_slot=2
```

- [ ] **Step 3: PAUSE — bench validation: reload finds 2 records**

Reset (no re-flash). Expected:
```
session_log: scan complete: valid=2 corrupt=0 blank=1022 head=0x044040 slot=2
```

- [ ] **Step 4: PAUSE — bench validation: dump first 4 records**

Add to main after `session_log_init()`:
```c
for (uint32_t n = 0; n < 4; ++n) {
    struct session_record r;
    int rc = session_log_read_nth(n, &r);
    printk("ses[%u]: rc=%d boot=%u start=0x%08x mwh=%u end=0x%02x\n",
           (unsigned)n, rc, (unsigned)r.boot_count,
           (unsigned)r.start_ts, (unsigned)r.mwh_delivered,
           (unsigned)r.end_reason);
}
```

Build, flash, monitor. Expected:
```
ses[0]: rc=0 boot=N start=0x20000000 mwh=7000  end=0xc0
ses[1]: rc=0 boot=N start=0x200003e8 mwh=14000 end=0xc1
ses[2]: rc=1 boot=65535 start=0xffffffff mwh=4294967295 end=0xff
ses[3]: rc=1 boot=65535 start=0xffffffff mwh=4294967295 end=0xff
```

- [ ] **Step 5: Remove the test + dump blocks from main()**

Keep `session_log_init()` and `session_log_set_boot_count()`. Delete the test gate and dump loop.

- [ ] **Step 6: Compile + flash final + commit**

```sh
cmake --build build && ./tools/flash.sh
git add src/persist/session_log.c src/persist/session_log.h \
        src/main.c CMakeLists.txt \
        docs/superpowers/plans/2026-05-02-m5b4-session-log.md
git commit -m "M5.b.4: session_log scan-on-boot + append"
```

---

### Task 3: Bring-up + tag

- [ ] **Step 1: Append M5.b.4 entry to `docs/bring-up.md`**

Mirror M5.b.3 style. Note the spec drift (added u16 boot_count to session_record).

- [ ] **Step 2: Tag**

```sh
git tag -a m5b4-session-log -m "M5.b.4: session_log scan-on-boot + append"
```

- [ ] **Step 3: (User-confirmed) push**

---

## After M5.b.4

1. **Plan M5.b.5 — persist_task FreeRTOS queue.** Wraps event_log_append + session_log_append in a queue drained by persist_task. safety_task and other producers post a write request and continue without blocking on W25Q I/O.

## Self-review

**Spec coverage (M5.b.4 slice):**

| Spec § 6 requirement | This plan |
|---|---|
| session_log: 8 sectors × 128 records | Tasks 1–2 |
| 32-byte record per spec layout | Task 1 (`struct session_record` — adds u16 boot_count) |
| Scan-on-boot head discovery | Task 1 |
| Append-only ring; erase next sector on wrap | Task 1 |
| persist_task queue | DEFERRED to M5.b.5 |

**Placeholders:** none.

**Type/name consistency:** `session_log_*` matches `event_log_*` shape. Constants follow `SESSION_LOG_*` naming. Record field `j1772_max_state_seen` matches spec.

**Wear:** ~10 sessions/day → 365 × 10 = 3,650 records/year. 1024-record ring → 3.6 lap-equivalents/year → ~3.6 sector erases/sector/year. Trivial against 100K cycles.
