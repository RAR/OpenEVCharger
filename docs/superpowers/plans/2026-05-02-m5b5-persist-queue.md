# M5.b.5: persist_task FreeRTOS Queue Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wrap the synchronous `event_log_append` / `session_log_append` in a FreeRTOS queue drained by `persist_task`. After this milestone, any task can `persist_post_event(rec)` / `persist_post_session(rec)` and continue immediately; persist_task (priority 1, lowest) drains the queue and runs the actual W25Q I/O. Frees `safety_task` from blocking on flash writes — required before M6 raises faults at real-time pace.

**Scope:** persist_task queue + producer API. Boot-time calls (pre-scheduler) remain on the synchronous append path. Post-scheduler producers use the queue. Crash-loop detection (M5.b.6) becomes possible because boot timestamps can now be queue-posted from io_task (or the new boot timestamp can stay synchronous — decided in M5.b.6).

**Architecture:**
- **Single tagged-union queue.** One `QueueHandle_t` carries a `struct persist_req` with a type tag + union of records. Queue depth 8 (each item ≈ 36 bytes → 288 B + ~80 B FreeRTOS overhead).
- **Producer API in persist_task.h.**
  - `persist_post_event(struct event_record *rec)` — copies into queue, returns 0 on success, -1 if queue full (non-blocking; drops on overflow).
  - `persist_post_session(struct session_record *rec)`.
  - Producers fill all fields; persist_task hands the record to event_log_append/session_log_append which finishes the boot_count + crc16 stamp.
- **Consumer.** `persist_task_run()` blocks on `xQueueReceive(..., portMAX_DELAY)` and dispatches by tag.
- **Boot-time path.** Pre-scheduler init code keeps using the synchronous _append APIs (none does today, but boot self-test events in M6 will). Post-scheduler producers use the queue.
- **Backpressure.** Queue full → drop, return -1, log once. Fault flood is the only realistic full-queue scenario; spec accepts losing a few events over blocking the safety loop.

**Tech Stack:** FreeRTOS queues + existing event_log + session_log.

**Hardware preconditions:**
- M5.b.4 tagged. session_log proven on bench.
- Bench unit on USB 5 V is sufficient.

**Success criteria:**
1. `persist_init()` creates the queue. persist_task starts and waits.
2. From `io_task`, post 1 event_record at task-start; verify it appears in event_log on next reboot's scan (`valid` count grows by 1, head advances by 1).
3. `persist_post_event()` returns 0 on success; logs printk on failure.
4. With queue depth 8 and a tight burst test (post 16 in a row from io_task at 1 ms intervals before persist_task drains): expect first 8 succeed, next 8 fail with "queue full" warning (one warning per overflow burst, not per drop).
5. PD4 still blinks; uptime ≥ 1 min, no resets.
6. Build: text < 19 KB.

---

## File Structure

```
OpenBHZD/
├── src/
│   ├── tasks/
│   │   ├── persist_task.c          # MODIFIED — real body
│   │   └── persist_task.h          # MODIFIED — producer API
│   └── tasks/io_task.c             # MODIFIED — post 1 test event at startup
└── (no new files)
```

---

## Tasks

### Task 1: Producer API + queue + drain loop

**Files:**
- Modify: `src/tasks/persist_task.{c,h}`

- [ ] **Step 1: Update `persist_task.h`**

```c
#ifndef OPENBHZD_TASKS_PERSIST_TASK_H
#define OPENBHZD_TASKS_PERSIST_TASK_H

#include "FreeRTOS.h"
#include "task.h"
#include "../persist/event_log.h"
#include "../persist/session_log.h"

#define PERSIST_TASK_STACK_WORDS  256U
#define PERSIST_TASK_PRIORITY     1U
#define PERSIST_QUEUE_DEPTH       8U

/* Create the queue + spawn persist_task. Call from main() before
 * vTaskStartScheduler(). */
void persist_task_create(void);

/* Post an event_record copy onto the persist queue. Helper handles
 * boot_count + crc16 inside event_log_append (called by persist_task).
 * Returns 0 on success, -1 if queue full. Non-blocking. */
int persist_post_event(const struct event_record *rec);

/* Same for session_records. */
int persist_post_session(const struct session_record *rec);

#endif
```

- [ ] **Step 2: Update `persist_task.c`**

```c
#include "persist_task.h"
#include "queue.h"
#include "../hal/uart.h"
#include <string.h>

typedef enum {
    PERSIST_REQ_EVENT,
    PERSIST_REQ_SESSION,
} persist_req_type_t;

struct persist_req {
    persist_req_type_t type;
    union {
        struct event_record   event;
        struct session_record session;
    } payload;
};

static QueueHandle_t s_queue;
static volatile uint8_t s_overflow_warned = 0;

int persist_post_event(const struct event_record *rec)
{
    if (!s_queue || rec == NULL) return -1;
    struct persist_req req;
    req.type = PERSIST_REQ_EVENT;
    memcpy(&req.payload.event, rec, sizeof *rec);
    if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
        if (!s_overflow_warned) {
            printk("persist: queue full — events dropped\n");
            s_overflow_warned = 1;
        }
        return -1;
    }
    return 0;
}

int persist_post_session(const struct session_record *rec)
{
    if (!s_queue || rec == NULL) return -1;
    struct persist_req req;
    req.type = PERSIST_REQ_SESSION;
    memcpy(&req.payload.session, rec, sizeof *rec);
    if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
        if (!s_overflow_warned) {
            printk("persist: queue full — sessions dropped\n");
            s_overflow_warned = 1;
        }
        return -1;
    }
    return 0;
}

static void persist_task_run(void *arg)
{
    (void)arg;
    struct persist_req req;
    for (;;) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) == pdTRUE) {
            switch (req.type) {
            case PERSIST_REQ_EVENT:
                if (event_log_append(&req.payload.event) != 0) {
                    printk("persist: event_log_append FAIL\n");
                }
                break;
            case PERSIST_REQ_SESSION:
                if (session_log_append(&req.payload.session) != 0) {
                    printk("persist: session_log_append FAIL\n");
                }
                break;
            }
            /* Reset overflow latch once we drain: future overflows get
             * a fresh warning. Cheap signalling for log-rate-limiting. */
            s_overflow_warned = 0;
        }
    }
}

void persist_task_create(void)
{
    s_queue = xQueueCreate(PERSIST_QUEUE_DEPTH, sizeof(struct persist_req));
    if (s_queue == NULL) {
        printk("persist: xQueueCreate FAIL\n");
        return;
    }
    xTaskCreate(persist_task_run,
                "persist",
                PERSIST_TASK_STACK_WORDS,
                NULL,
                PERSIST_TASK_PRIORITY,
                NULL);
}
```

- [ ] **Step 3: Compile**

```sh
cmake --build build
```

Expected: clean build. text grows ~600 B.

---

### Task 2: io_task posts 1 startup event

**Files:**
- Modify: `src/tasks/io_task.c`

- [ ] **Step 1: Add an event_record post on io_task entry**

```c
#include "io_task.h"
#include "persist_task.h"
#include "../hal/adc_inject.h"
...
static void io_task_run(void *arg)
{
    (void)arg;
    buttons_init();

    /* M5.b.5 self-test: post one event at io_task startup. After a
     * reboot, scan should find this record (valid count +1). */
    {
        struct event_record rec = {
            .timestamp = (uint32_t)xTaskGetTickCount(),
            .fault_id  = 0xB001U,           /* "io startup" sentinel */
            .j1772_state = 0xA0U,
            .evse_state  = 0xB1U,
            .cp_mv = (int16_t)cp_high_mv(),
            .cc_amps = 0,
            .ntc1_dC = 0, .ntc2_dC = 0, .active_amps_x10 = 0,
        };
        int rc = persist_post_event(&rec);
        printk("io_task: posted startup event rc=%d\n", rc);
    }

    TickType_t last_wake = xTaskGetTickCount();
    ...
```

- [ ] **Step 2: Compile + flash**

```sh
cmake --build build && ./tools/flash.sh
```

- [ ] **Step 3: PAUSE — bench validation: queue post + drain**

Capture monitor. Expected:
```
event_log: scan complete: valid=3 corrupt=0 blank=8189 head=0x004060 slot=3
session_log: scan complete: valid=2 corrupt=0 blank=1022 head=0x044040 slot=2
scheduler starting
io_task: posted startup event rc=0
```

- [ ] **Step 4: PAUSE — bench validation: reset and verify the new event landed**

Reset (no re-flash). Expected:
```
event_log: scan complete: valid=4 corrupt=0 blank=8188 head=0x004080 slot=4
io_task: posted startup event rc=0
```

valid grows by 1 each reboot (because io_task re-posts on every boot). head advances by 1.

- [ ] **Step 5: PAUSE — bench validation: queue overflow path (optional)**

Burst-test: temporarily change io_task to post 16 events back-to-back instead of 1. Expected:
```
io_task: posted startup event rc=0   ← × 8
io_task: posted startup event rc=-1
persist: queue full — events dropped  ← single warning for the burst
io_task: posted startup event rc=-1
io_task: posted startup event rc=-1
... (× 8 of -1)
```

After ~50 ms, persist_task drains and the warning latch clears. Revert
to single post.

- [ ] **Step 6: Trim io_task back to single post (or remove if M6 will overwrite this code)**

Keep the single post — it's a useful "boot signature" event for production
that proves the queue is alive each session.

- [ ] **Step 7: Commit**

```sh
git add src/tasks/persist_task.c src/tasks/persist_task.h \
        src/tasks/io_task.c \
        docs/superpowers/plans/2026-05-02-m5b5-persist-queue.md
git commit -m "M5.b.5: persist_task FreeRTOS queue + producer API"
```

---

### Task 3: Bring-up + tag

- [ ] **Step 1: Append M5.b.5 entry to `docs/bring-up.md`**

- [ ] **Step 2: Tag**

```sh
git tag -a m5b5-persist-queue -m "M5.b.5: persist_task queue + producer API"
```

- [ ] **Step 3: (User-confirmed) push**

---

## After M5.b.5

1. **Plan M5.b.6 — crash-loop detector.** With the queue in place, `safety_task` (or main pre-scheduler) records boot timestamps and detects 5 boots in 60 s → safe-fail mode. Final M5.b piece.
2. **M6 prerequisites are satisfied.** safety_task can post fault event_records to persist_post_event without blocking on W25Q.

## Self-review

**Spec coverage (M5.b.5 slice):**

| Spec § 6 / § 5.5 requirement | This plan |
|---|---|
| persist_task drains a queue of W25Q write requests | Tasks 1–2 |
| Lowest-priority worker | Already at PERSIST_TASK_PRIORITY=1 |
| Other tasks queue write requests | Producer API in persist_task.h |
| Spec § 5.5 "lowest-priority so it never starves higher work" | priority 1 < safety/io/comms |
| crash-loop detector | DEFERRED to M5.b.6 |

**Placeholders:** none.

**Type/name consistency:** `persist_post_event` / `persist_post_session` symmetric. Queue depth named constant. Internal tag enum localized to .c.

**Concurrency:** `persist_post_*` is task-safe (xQueueSend is task-safe). Not ISR-safe — would need `xQueueSendFromISR` variants. Spec § 5.4 says GFCI is the only ISR-driven path; M6 will route GFCI fault into safety_task via task notification, then safety_task does the post. So no fromISR needed in M5.b.5.
