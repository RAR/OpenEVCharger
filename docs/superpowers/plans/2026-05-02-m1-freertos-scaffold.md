# M1: FreeRTOS Scaffold + IWDG Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring up FreeRTOS on the GD32F205VG with the four-task safety-core skeleton (`safety_task`, `io_task`, `comms_task`, `persist_task`) running at the priorities defined in the spec. The 1 Hz heartbeat blink moves from `main()` into `io_task`. `safety_task` arms the FWDGT (independent watchdog) and kicks it on every 20 ms tick.

**Architecture:** FreeRTOS V11 LTS (Cortex-M3 GCC port) added as a git submodule under `third_party/FreeRTOS-Kernel/`. `heap_4` for dynamic allocation. Three task functions are stubs in M1 (just `vTaskDelay` forever) — the structure exists so M2+ can fill in real bodies. Single-writer discipline established but not yet load-bearing (no relays driven yet). Watchdog kicked only by `safety_task`; if `safety_task` ever blocks > 1 s the chip resets.

**Tech Stack:** FreeRTOS-Kernel V11.x (LTS), `portable/GCC/ARM_CM3`, `portable/MemMang/heap_4.c`, GD32 SPL FWDGT driver, existing arm-none-eabi-gcc + CMake setup.

**Hardware preconditions:**
- M0 complete and tagged `m0-toolchain-bootstrap`. PD4 blink validated.
- Bench unit powered, SWD probe connected. Same setup as M0.
- No new pins or peripherals — M1 is purely software layering.

**Success criterion:** After flashing M1 firmware, PD4 still blinks at 1 Hz (now driven by `io_task`), and the unit runs continuously for ≥ 60 seconds without resetting (proves `safety_task` is alive and kicking the watchdog every 20 ms). Optional bonus: deliberately hang `safety_task` in a one-off test build, observe LED stop blinking and the chip reset within ~1 second, then revert.

---

## File Structure

```
OpenEVCharger/
├── third_party/
│   └── FreeRTOS-Kernel/                # NEW — git submodule, V11.x LTS
├── src/
│   ├── main.c                          # MODIFIED — creates tasks, starts scheduler
│   ├── FreeRTOSConfig.h                # NEW — kernel tuning + handler aliases
│   ├── tasks/
│   │   ├── safety_task.c               # NEW — 50 Hz tick stub, kicks WDG
│   │   ├── safety_task.h
│   │   ├── io_task.c                   # NEW — 1 Hz heartbeat blink
│   │   ├── io_task.h
│   │   ├── comms_task.c                # NEW — stub, vTaskDelay forever
│   │   ├── comms_task.h
│   │   ├── persist_task.c              # NEW — stub, vTaskDelay forever
│   │   └── persist_task.h
│   └── hal/
│       ├── wdg.c                       # NEW — FWDGT init + kick wrapper
│       └── wdg.h
└── CMakeLists.txt                      # MODIFIED — link FreeRTOS, add tasks/, hal/
```

**Why each file is separate:**

- `safety_task.c` is the one task with safety-critical responsibility. Keeping it isolated makes auditing trivially easy: every line that runs in safety context is in this file (or files it calls).
- `io_task.c`, `comms_task.c`, `persist_task.c` get separate files so they're independently editable in later milestones without merge conflicts and so M2+ implementers can hold one task in context at a time.
- `hal/wdg.c` is a thin wrapper. The watchdog is the only safety mechanism that survives main-clock failure; isolating its driver from the SPL keeps options open if we ever need to bypass the SPL (e.g. to enable IWDG before any peripheral clock is enabled).
- `FreeRTOSConfig.h` lives in `src/` so the include path is application-controlled. The kernel includes `"FreeRTOSConfig.h"` from any `freertos/include`-relative file, and our CMake adds `src/` to the kernel target's include path.

---

## Task scaffold details (informational, not code yet)

| Task | Stack words | Priority | Period | M1 body |
|---|---|---|---|---|
| `safety_task` | 256 | 4 (highest) | 20 ms via `vTaskDelayUntil` | kick FWDGT, no other work |
| `io_task` | 256 | 3 | 33 ms (~30 Hz) | toggle PD4 every 500 ms (LED at 1 Hz) |
| `comms_task` | 192 | 2 | n/a | `vTaskDelay(portMAX_DELAY)` forever |
| `persist_task` | 192 | 1 | n/a | `vTaskDelay(portMAX_DELAY)` forever |

`configMAX_PRIORITIES = 5` (idle = 0, our 4 tasks at 1–4).

`configMINIMAL_STACK_SIZE = 128` (words = 512 B). Per-task stack overrides above.

`configTOTAL_HEAP_SIZE = 16384` (16 KB). With 4 tasks (TCB + stacks ≈ 4 KB) plus FreeRTOS internals, ~12 KB free for queues/mutexes added in later milestones.

---

## Tasks

### Task 1: Add FreeRTOS-Kernel as a submodule

**Files:**
- New: `OpenEVCharger/third_party/FreeRTOS-Kernel/` (submodule contents — many files)
- Modify: `OpenEVCharger/.gitmodules` (auto-created by `git submodule add`)

- [ ] **Step 1: Add the submodule pinned to a current LTS tag**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
git submodule add https://github.com/FreeRTOS/FreeRTOS-Kernel.git third_party/FreeRTOS-Kernel
cd third_party/FreeRTOS-Kernel
# Check what tags exist
git tag --sort=-v:refname | head -10
```

- [ ] **Step 2: Pin to V11.1.0 (LTS)**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger/third_party/FreeRTOS-Kernel
git checkout V11.1.0
cd ../..
```

If `V11.1.0` isn't a tag (FreeRTOS may have moved on), use the most recent `V11.*` tag. Document the chosen version in the next commit message.

- [ ] **Step 3: Verify the Cortex-M3 GCC port files exist**

```sh
ls /home/rar/device-configs/esphome/rippleon/OpenEVCharger/third_party/FreeRTOS-Kernel/portable/GCC/ARM_CM3/
ls /home/rar/device-configs/esphome/rippleon/OpenEVCharger/third_party/FreeRTOS-Kernel/portable/MemMang/
ls /home/rar/device-configs/esphome/rippleon/OpenEVCharger/third_party/FreeRTOS-Kernel/include/FreeRTOS.h
```

Expected:
- `portable/GCC/ARM_CM3/`: contains `port.c` and `portmacro.h`
- `portable/MemMang/`: contains `heap_4.c` (among others)
- `include/FreeRTOS.h`: exists

If the layout has changed in V11, adapt the include paths in Task 5's CMakeLists changes accordingly.

- [ ] **Step 4: Commit submodule addition**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
git add .gitmodules third_party/FreeRTOS-Kernel
git commit -m "M1.1: add FreeRTOS-Kernel V11.1.0 as submodule"
```

---

### Task 2: Write `FreeRTOSConfig.h`

**Files:**
- New: `OpenEVCharger/src/FreeRTOSConfig.h`

- [ ] **Step 1: Write the config header**

```c
/* FreeRTOSConfig.h for OpenEVCharger on GD32F205VG (Cortex-M3 @ 120 MHz).
 *
 * Tuned for safety-core scope: 4 tasks (idle + safety + io + comms + persist),
 * heap_4, mutexes for state-snapshot guarding, no software timers in M1
 * (added later if needed), preemptive scheduling, stack-overflow checking
 * level 2 enabled (most thorough), no idle hook in M1.
 *
 * The kernel ISR handler names are aliased here to the Cortex-M default
 * names that the vendor startup vector table uses, so we don't have to
 * patch startup_gd32f20x_cl.S.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

extern uint32_t SystemCoreClock;

#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_TICKLESS_IDLE                 0

#define configCPU_CLOCK_HZ                      (SystemCoreClock)
#define configTICK_RATE_HZ                      ((TickType_t)1000)
#define configMAX_PRIORITIES                    5
#define configMINIMAL_STACK_SIZE                ((uint16_t)128)   /* words = 512 B */
#define configTOTAL_HEAP_SIZE                   ((size_t)16384)
#define configMAX_TASK_NAME_LEN                 16
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TASK_NOTIFICATIONS            1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               4
#define configUSE_QUEUE_SETS                    0
#define configUSE_TIMERS                        0
#define configENABLE_BACKWARD_COMPATIBILITY     0

/* Hooks */
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1

/* Run-time stats / trace — off in M1, can enable per-milestone for debug */
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

/* Co-routine support — not used */
#define configUSE_CO_ROUTINES                   0

/* Software timer config — disabled (configUSE_TIMERS = 0) */

/* API inclusions */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     0
#define INCLUDE_vTaskCleanUpResources           0
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_eTaskGetState                   0
#define INCLUDE_xTimerPendFunctionCall          0

/* Cortex-M3 NVIC priority bits — GD32F205 uses 4 bits (16 priority levels) */
#define configPRIO_BITS                         4

/* The lowest interrupt priority that can be used in a call to a "set priority"
 * function. */
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY     15

/* The highest interrupt priority that can be used by any interrupt service
 * routine that makes calls to interrupt-safe FreeRTOS API functions. DO NOT
 * CALL FREERTOS API FUNCTIONS FROM ANY INTERRUPT THAT HAS A HIGHER PRIORITY
 * THAN THIS! (lower numbers = higher priority on Cortex-M) */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5

#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* assert macro — fall through to a simple infinite loop on failure;
 * a debugger can break in here and inspect the stack. */
#define configASSERT(x) \
    do { if (!(x)) { __asm volatile("cpsid i"); for (;;) {} } } while (0)

/* Map FreeRTOS handler names to Cortex-M vector-table names so the vendor
 * startup file (which references SVC_Handler/PendSV_Handler/SysTick_Handler)
 * picks up the FreeRTOS implementations without modifying startup_gd32f20x_cl.S */
#define vPortSVCHandler         SVC_Handler
#define xPortPendSVHandler      PendSV_Handler
#define xPortSysTickHandler     SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
```

- [ ] **Step 2: Commit**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
git add src/FreeRTOSConfig.h
git commit -m "M1.2: FreeRTOSConfig.h tuned for Cortex-M3 + safety-core scope"
```

---

### Task 3: Watchdog HAL (`src/hal/wdg.{c,h}`)

**Files:**
- New: `OpenEVCharger/src/hal/wdg.c`
- New: `OpenEVCharger/src/hal/wdg.h`

GD32F205 FWDGT (free watchdog timer = STM32-equivalent IWDG). LSI clock is ~40 kHz nominal. With prescaler /32 and reload = 1250, timeout = 1250 × 32 / 40000 ≈ 1 s.

- [ ] **Step 1: Write `src/hal/wdg.h`**

```c
/* Watchdog HAL — independent watchdog (FWDGT in GD32 nomenclature, IWDG in
 * STM32). Owned by safety_task: no other code calls wdg_kick().
 */

#ifndef OPENEVCHARGER_HAL_WDG_H
#define OPENEVCHARGER_HAL_WDG_H

#include <stdint.h>

/* Initialise the watchdog with ~1 second timeout.
 * Once enabled, the watchdog cannot be disabled — it must be kicked
 * every < 1 s or the chip resets. Call this AFTER FreeRTOS is started
 * but BEFORE safety_task enters its main loop.
 */
void wdg_init(void);

/* Reset the watchdog countdown. Call from safety_task only, on every tick. */
void wdg_kick(void);

/* Returns 1 if the most recent reset was due to watchdog, 0 otherwise.
 * Read this once at boot before clearing the reset flags.
 */
int wdg_was_last_reset(void);

#endif /* OPENEVCHARGER_HAL_WDG_H */
```

- [ ] **Step 2: Write `src/hal/wdg.c`**

```c
#include "wdg.h"
#include "gd32f20x.h"

/* LSI nominal = 40 kHz. With prescaler /32 and reload counter = 1250:
 *   timeout = 1250 * 32 / 40000 = 1.0 second
 *
 * That gives safety_task plenty of margin: it ticks every 20 ms, so
 * the watchdog has 50× safety margin on a healthy run.
 */
#define WDG_PRESCALER  FWDGT_PSC_DIV32
#define WDG_RELOAD     1250U

void wdg_init(void)
{
    /* Enable LSI (IRC40K) — required clock source for FWDGT */
    rcu_osci_on(RCU_IRC40K);
    while (rcu_osci_stab_wait(RCU_IRC40K) == ERROR) {
        /* spin until LSI is stable; should be < 1 ms */
    }

    /* Configure prescaler and reload */
    fwdgt_write_enable();
    (void)fwdgt_config(WDG_RELOAD, WDG_PRESCALER);
    fwdgt_write_disable();

    /* Halt the watchdog when the core is halted by SWD — keeps debugger
     * sessions from triggering resets. DBGMCU bit 8 = FWDGT_HOLD. */
    DBG_CTL |= DBG_CTL_FWDGT_HOLD;

    fwdgt_enable();
}

void wdg_kick(void)
{
    fwdgt_counter_reload();
}

int wdg_was_last_reset(void)
{
    /* RSTSCK / RSR_FWDGTRST bit. The vendor define is RCU_RSTSCK
     * with bit FWDGTRSTF (== STM32 IWDGRSTF). */
    if (RCU_RSTSCK & RCU_RSTSCK_FWDGTRSTF) {
        return 1;
    }
    return 0;
}
```

- [ ] **Step 3: Smoke-build (still using main.c blink, no FreeRTOS hookup yet)**

The wdg module isn't called yet, but adding it to the build catches any header issues now. Skip ahead to Task 5's CMakeLists update — once the build target includes `wdg.c` and `tasks/*.c`, run `cmake --build build` to verify everything compiles.

- [ ] **Step 4: Commit (if compiles cleanly after Task 5 lands)**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
git add src/hal/wdg.c src/hal/wdg.h
git commit -m "M1.3: FWDGT (independent watchdog) HAL — 1 s timeout, halt-safe"
```

If compilation reveals that `DBG_CTL_FWDGT_HOLD` isn't defined in the vendor headers, look in `gd32f20x_dbg.h`/`gd32f20x.h` for the actual symbol (alternatives: `DBG_FWDGT_HOLD`, `DBG_CTL_FWDGTHOLD`). Substitute and recompile.

---

### Task 4: Task scaffolds

**Files:**
- New: `OpenEVCharger/src/tasks/safety_task.{c,h}`
- New: `OpenEVCharger/src/tasks/io_task.{c,h}`
- New: `OpenEVCharger/src/tasks/comms_task.{c,h}`
- New: `OpenEVCharger/src/tasks/persist_task.{c,h}`

- [ ] **Step 1: `src/tasks/safety_task.h`**

```c
#ifndef OPENEVCHARGER_TASKS_SAFETY_TASK_H
#define OPENEVCHARGER_TASKS_SAFETY_TASK_H

#include "FreeRTOS.h"
#include "task.h"

#define SAFETY_TASK_STACK_WORDS  256U
#define SAFETY_TASK_PRIORITY     4U
#define SAFETY_TASK_PERIOD_MS    20U

void safety_task_create(void);

#endif
```

- [ ] **Step 2: `src/tasks/safety_task.c`**

```c
#include "safety_task.h"
#include "../hal/wdg.h"

static void safety_task_run(void *arg)
{
    (void)arg;

    /* Initialise the watchdog as the very first thing this task does.
     * From this point on the chip will reset if we don't loop within 1 s. */
    wdg_init();

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        /* M1: nothing safety-related to do yet. Just kick. */
        wdg_kick();

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

- [ ] **Step 3: `src/tasks/io_task.h`**

```c
#ifndef OPENEVCHARGER_TASKS_IO_TASK_H
#define OPENEVCHARGER_TASKS_IO_TASK_H

#include "FreeRTOS.h"
#include "task.h"

#define IO_TASK_STACK_WORDS  256U
#define IO_TASK_PRIORITY     3U

void io_task_create(void);

#endif
```

- [ ] **Step 4: `src/tasks/io_task.c`**

```c
#include "io_task.h"
#include "gd32f20x.h"

#define HEARTBEAT_PORT GPIOD
#define HEARTBEAT_PIN  GPIO_PIN_4
#define HEARTBEAT_RCU  RCU_GPIOD

static void heartbeat_init(void)
{
    rcu_periph_clock_enable(HEARTBEAT_RCU);
    gpio_init(HEARTBEAT_PORT,
              GPIO_MODE_OUT_PP,
              GPIO_OSPEED_2MHZ,
              HEARTBEAT_PIN);
    gpio_bit_reset(HEARTBEAT_PORT, HEARTBEAT_PIN);
}

static void io_task_run(void *arg)
{
    (void)arg;
    heartbeat_init();

    for (;;) {
        gpio_bit_set(HEARTBEAT_PORT, HEARTBEAT_PIN);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_bit_reset(HEARTBEAT_PORT, HEARTBEAT_PIN);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void io_task_create(void)
{
    xTaskCreate(io_task_run,
                "io",
                IO_TASK_STACK_WORDS,
                NULL,
                IO_TASK_PRIORITY,
                NULL);
}
```

- [ ] **Step 5: `src/tasks/comms_task.h`**

```c
#ifndef OPENEVCHARGER_TASKS_COMMS_TASK_H
#define OPENEVCHARGER_TASKS_COMMS_TASK_H

#include "FreeRTOS.h"
#include "task.h"

#define COMMS_TASK_STACK_WORDS  192U
#define COMMS_TASK_PRIORITY     2U

void comms_task_create(void);

#endif
```

- [ ] **Step 6: `src/tasks/comms_task.c`**

```c
#include "comms_task.h"

static void comms_task_run(void *arg)
{
    (void)arg;
    /* M1 stub — real body lands in M8 (FC41D TLV protocol). */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

void comms_task_create(void)
{
    xTaskCreate(comms_task_run,
                "comms",
                COMMS_TASK_STACK_WORDS,
                NULL,
                COMMS_TASK_PRIORITY,
                NULL);
}
```

- [ ] **Step 7: `src/tasks/persist_task.h`**

```c
#ifndef OPENEVCHARGER_TASKS_PERSIST_TASK_H
#define OPENEVCHARGER_TASKS_PERSIST_TASK_H

#include "FreeRTOS.h"
#include "task.h"

#define PERSIST_TASK_STACK_WORDS  192U
#define PERSIST_TASK_PRIORITY     1U

void persist_task_create(void);

#endif
```

- [ ] **Step 8: `src/tasks/persist_task.c`**

```c
#include "persist_task.h"

static void persist_task_run(void *arg)
{
    (void)arg;
    /* M1 stub — real body lands in M5 (W25Q persistence). */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

void persist_task_create(void)
{
    xTaskCreate(persist_task_run,
                "persist",
                PERSIST_TASK_STACK_WORDS,
                NULL,
                PERSIST_TASK_PRIORITY,
                NULL);
}
```

- [ ] **Step 9: Commit (deferred until Task 5 lands so this compiles)**

---

### Task 5: Update `main.c` and `CMakeLists.txt`

**Files:**
- Modify: `OpenEVCharger/src/main.c`
- Modify: `OpenEVCharger/CMakeLists.txt`

- [ ] **Step 1: Rewrite `src/main.c` to start FreeRTOS**

Replace the entire file with:

```c
/* OpenEVCharger M1 — FreeRTOS scaffold + 4-task skeleton.
 *
 * main() does no real work: it creates the four safety-core tasks then
 * starts the scheduler. PD4 heartbeat moves into io_task. safety_task
 * arms the watchdog as its first action.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "tasks/safety_task.h"
#include "tasks/io_task.h"
#include "tasks/comms_task.h"
#include "tasks/persist_task.h"

/* Newlib's __libc_init_array references _init/_fini; we have no C++
 * static ctors so empty stubs are fine. */
void _init(void) {}
void _fini(void) {}

/* FreeRTOS hooks. configCHECK_FOR_STACK_OVERFLOW = 2 means this fires
 * if any task touches a low-water canary. configUSE_MALLOC_FAILED_HOOK = 1
 * means this fires if pvPortMalloc returns NULL. We trap into an infinite
 * loop in both cases — a debugger session can break in and inspect. */

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

int main(void)
{
    /* Vendor SystemInit() has already run from startup_gd32f20x_cl.S
     * (clock tree at 120 MHz). No further pre-scheduler init needed. */

    safety_task_create();
    io_task_create();
    comms_task_create();
    persist_task_create();

    vTaskStartScheduler();

    /* Should never return. If we get here, the scheduler ran out of
     * heap before creating the idle task. Trap. */
    for (;;) {}
}
```

- [ ] **Step 2: Update `CMakeLists.txt`**

Replace the whole file with:

```cmake
cmake_minimum_required(VERSION 3.20)

# The toolchain file MUST be set on the cmake command line via
# -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
project(openevcharger C ASM)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ---------- Vendor lib paths ----------

set(VENDOR_LIB ${CMAKE_SOURCE_DIR}/third_party/GD32F20x_Firmware_Library)
set(CMSIS_CORE ${VENDOR_LIB}/cmsis/cores/gd32)
set(GD_HEADERS ${VENDOR_LIB}/cmsis/variants/gd32f20x)
set(GD_STARTUP ${VENDOR_LIB}/cmsis/startup_files)
set(STDP_INC   ${VENDOR_LIB}/spl/inc)
set(STDP_SRC   ${VENDOR_LIB}/spl/src)

set(GD_STARTUP_SRC ${GD_STARTUP}/startup_gd32f20x_cl.S)
set(GD_SYSTEM_SRC  ${GD_HEADERS}/system_gd32f20x.c)

set(STDP_SRCS
    ${STDP_SRC}/gd32f20x_gpio.c
    ${STDP_SRC}/gd32f20x_rcu.c
    ${STDP_SRC}/gd32f20x_misc.c
    ${STDP_SRC}/gd32f20x_fwdgt.c
)

# ---------- FreeRTOS ----------

set(FREERTOS_DIR ${CMAKE_SOURCE_DIR}/third_party/FreeRTOS-Kernel)
set(FREERTOS_PORT_DIR ${FREERTOS_DIR}/portable/GCC/ARM_CM3)

set(FREERTOS_SRCS
    ${FREERTOS_DIR}/tasks.c
    ${FREERTOS_DIR}/queue.c
    ${FREERTOS_DIR}/list.c
    ${FREERTOS_DIR}/timers.c
    ${FREERTOS_DIR}/event_groups.c
    ${FREERTOS_DIR}/stream_buffer.c
    ${FREERTOS_PORT_DIR}/port.c
    ${FREERTOS_DIR}/portable/MemMang/heap_4.c
)

# ---------- Application sources ----------

set(APP_SRCS
    src/main.c
    src/hal/wdg.c
    src/tasks/safety_task.c
    src/tasks/io_task.c
    src/tasks/comms_task.c
    src/tasks/persist_task.c
)

# ---------- Target ----------

set(TARGET ${PROJECT_NAME})

add_executable(${TARGET}
    ${APP_SRCS}
    ${FREERTOS_SRCS}
    ${GD_STARTUP_SRC}
    ${GD_SYSTEM_SRC}
    ${STDP_SRCS}
)

target_include_directories(${TARGET} PRIVATE
    src
    ${CMSIS_CORE}
    ${GD_HEADERS}
    ${STDP_INC}
    ${FREERTOS_DIR}/include
    ${FREERTOS_PORT_DIR}
)

target_compile_definitions(${TARGET} PRIVATE
    GD32F20X_CL=1
    HXTAL_VALUE=8000000
)

set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/linker/gd32f205vc.ld)
target_link_options(${TARGET} PRIVATE
    -T${LINKER_SCRIPT}
    -Wl,-Map=${TARGET}.map,--cref
)
set_target_properties(${TARGET} PROPERTIES
    SUFFIX ".elf"
    LINK_DEPENDS ${LINKER_SCRIPT}
)

add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${TARGET}> ${TARGET}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${TARGET}> ${TARGET}.hex
    COMMAND ${CMAKE_SIZE}   $<TARGET_FILE:${TARGET}>
    BYPRODUCTS ${TARGET}.bin ${TARGET}.hex
    COMMENT "Generating ${TARGET}.bin/.hex and printing size"
)
```

- [ ] **Step 3: Build**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
rm -rf build
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
cmake --build build 2>&1 | tail -20
```

Expected: clean build with size report. Flash usage will jump from ~1 KB to maybe 10–15 KB (FreeRTOS adds ~6–8 KB code + heap_4 + port). RAM usage will jump to ~20 KB (16 KB heap reservation + task TCBs + stacks).

- [ ] **Step 4: Resolve any build errors**

Common issues and fixes:
1. `'configMINIMAL_STACK_SIZE' undeclared` from `port.c` → `FreeRTOSConfig.h` not on include path. Verify `target_include_directories` lists `src`.
2. Multiple definition of `SVC_Handler` → vendor startup file has a non-weak definition. Check `startup_gd32f20x_cl.S`; if SVC_Handler isn't `.weak`, need to comment out the vendor one or use `-Wl,--allow-multiple-definition` (last resort).
3. `'DBG_CTL_FWDGT_HOLD' undeclared` → vendor symbol has a different name. `grep -rE 'FWDGT.*HOLD|HOLD.*FWDGT' third_party/GD32F20x_Firmware_Library/` to find the right name.
4. Heap overflow at link → we asked for 16 KB heap, total RAM usage exceeds 128 KB. Lower `configTOTAL_HEAP_SIZE` or task stacks.

- [ ] **Step 5: Commit M1.3+M1.4+M1.5 together once it compiles**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
git add src/hal/ src/tasks/ src/main.c src/FreeRTOSConfig.h CMakeLists.txt
git status
git commit -m "M1.3-5: FreeRTOS scaffold — wdg HAL, 4 tasks, scheduler bootstrap"
```

(M1.2 was already committed alone in Task 2 — that's fine. The combined commit covers the rest.)

---

### Task 6: Flash and verify (hardware validation gate)

- [ ] **Step 1: Flash**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
./tools/flash.sh
```

Expected: `Programming Finished`, `Verified OK`, the script reports "Heartbeat LED on PD4 should be blinking at 1 Hz."

- [ ] **Step 2: Visual verify**

Watch PD4 LED. Should still blink at 1 Hz, exactly as in M0. The blink is now driven by `io_task` running under FreeRTOS preemption.

- [ ] **Step 3: Stability test**

Watch the LED for 60 seconds. It must not stutter, freeze, or restart. A reset would manifest as the LED going off briefly (during reset hardware) then resuming the blink pattern from the start.

- [ ] **Step 4: Halt-state check**

Halt the chip and confirm the kernel is running (PC inside FreeRTOS code or our task code, NOT inside the default infinite-loop hard-fault handler):

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
openocd -f tools/openocd-gd32f205.cfg -c "init; halt; reg pc; reg lr; targets; shutdown" 2>&1 | tail -10
```

Expected: `pc` somewhere in `0x080xxxxx` (our flash region), state `halted`. Reading `lr` gives a hint about who called the current function. If you load the ELF in `arm-none-eabi-addr2line` or `objdump`, you can resolve the PC to a function name:

```sh
arm-none-eabi-addr2line -e build/openevcharger.elf <pc-value>
```

Likely results: PC inside `prvIdleTask`, `xTaskGetTickCount`, or `vPortSuppressTicksAndSleep` (idle task code). Any of those proves the kernel is alive.

- [ ] **Step 5: Watchdog negative test (optional but recommended)**

Build a one-off test variant that deliberately hangs `safety_task` to prove the watchdog reset works. Steps:

1. Edit `src/tasks/safety_task.c`: comment out the `wdg_kick()` line (or replace the whole loop body with `for(;;){}`).
2. Build, flash.
3. Watch the LED: it should blink for ~1 second (the io_task keeps running because the kernel is fine, just safety_task isn't kicking the watchdog), then the chip resets, then the blink restarts. Cycle repeats every ~1 second. This proves: watchdog is armed AND the only thing keeping it kicked is safety_task.
4. Revert the change. Build, flash. LED should blink steadily again.

If you skip this test, you save 5 minutes but don't have positive proof the watchdog is functioning. Recommended to do it.

---

### Task 7: Update `docs/bring-up.md` with M1 entry

**Files:**
- Modify: `OpenEVCharger/docs/bring-up.md`

- [ ] **Step 1: Append the M1 entry**

After the M0 section, append:

```markdown
## M1 — FreeRTOS Scaffold + IWDG

**Date completed:** YYYY-MM-DD (fill in)
**Spec section:** § 9 M1
**Plan:** docs/superpowers/plans/2026-05-02-m1-freertos-scaffold.md

### Success criterion
PD4 heartbeat blink driven by `io_task` under FreeRTOS, with `safety_task`
kicking the FWDGT watchdog every 20 ms. Unit runs ≥ 60 s without resetting.

### Observed result
- LED blinks at 1 Hz from io_task: YES / NO
- 60 s stability test passed (no resets): YES / NO
- Watchdog negative test (deliberately hung safety_task) confirmed reset behavior: YES / NO / SKIPPED
- Halt-state PC + addr2line: ____________________________

### Build size
- text: _____ B
- data: _____ B
- bss:  _____ B
- flash usage: ____% of 512 KB linker region
- RAM usage: ____% of 128 KB

### Deviations from plan
(fill in as discovered)

### Next milestone
M2: GPIO + ADC scan + button. Plan to be written after M1 validates.
```

- [ ] **Step 2: Fill in actual measurements**

After Task 6 validates, populate the checkboxes and numbers.

- [ ] **Step 3: Commit**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
git add docs/bring-up.md
git commit -m "M1.6: bring-up log — M1 validated on bench"
```

---

### Task 8: Tag M1

- [ ] **Step 1: Tag**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
git tag -a m1-freertos-scaffold -m "M1 complete: FreeRTOS scaffold + IWDG, validated on bench"
```

- [ ] **Step 2: (User-confirmed) push**

```sh
git push origin main
git push origin m1-freertos-scaffold
```

Don't push without user confirmation.

---

## After M1

1. **Update memory** with any new discoveries (likely: FreeRTOS heap usage observations, FWDGT vs DBGMCU symbol names).
2. **Write M2 plan.** M2 = GPIO config for full pin map + DMA-driven ADC scan replicating stock's 0x20002a80 layout + 3-button ladder reading PC3. Adds:
   - `src/hal/gpio.c` initialising every pin per `src/core/pin_map.h`
   - `src/hal/adc_scan.c` running 11-channel DMA scan at ~1 kHz
   - `src/ui/buttons.c` decoding the 3-button ladder + PC9
   - First debug UART (USART1 PA9/PA10) so `vTaskList` and other diagnostics can print

---

## Self-review

**Spec coverage check (M1 only — § 9 of the spec, M1 row):**

| Spec requirement (M1) | Plan task |
|---|---|
| FreeRTOS submodule + vendor port | Task 1 |
| Task-driven heartbeat | Task 4 (io_task), Task 5 (main.c) |
| IWDG armed, safety_task kicks | Task 3 (wdg HAL), Task 4 (safety_task) |
| **SUCCESS = LED blinks (under FreeRTOS), kernel alive** | Task 6 |

`vTaskList over an init-time UART log shows tasks` from the spec is dropped from M1 in favour of the LED + halt-state-PC validation, since UART setup is M2 work. The proof-of-life is equivalent (LED proves io_task runs; PC inside kernel code proves kernel runs; 60 s stability proves watchdog kicking works).

**Placeholder scan.** No "TODO", "TBD", or "implement later" tokens in code. All file contents written out in full. Stub task bodies for `comms_task` and `persist_task` are intentional (M2+ fills them) and explicitly labelled `M1 stub`.

**Type/name consistency.**
- `safety_task_create` / `io_task_create` / `comms_task_create` / `persist_task_create` — same naming pattern across all four headers and the `main()` calls.
- `xTaskCreate` task names: `"safety"`, `"io"`, `"comms"`, `"persist"` — match the create-function prefixes.
- `HEARTBEAT_PORT/PIN/RCU` macros in `io_task.c` match the M0 names from the previous `main.c` (the macros migrate cleanly).
- `wdg_init` / `wdg_kick` / `wdg_was_last_reset` referenced consistently between header and call sites.
- `configMAX_PRIORITIES = 5` in `FreeRTOSConfig.h` matches: idle(0) + persist(1) + comms(2) + io(3) + safety(4) = 5 distinct priorities.

**Gaps.** None for M1. The spec's optional `vTaskList` UART output is deferred to M2 with a substitute proof of life that's just as conclusive.
