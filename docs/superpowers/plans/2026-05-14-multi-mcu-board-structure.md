# Multi-MCU Board Structure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reconcile the `nexcyber-port-skeleton` island model into a real multi-MCU portability boundary — a `boards/<board>/` config tree and a `src/hal/` interface/per-chip-impl split — without regressing the bench-validated GD32/Rippleon firmware or losing any Nexcyber bring-up work.

**Architecture:** `CMakeLists.txt` becomes board-agnostic and `include()`s a per-board `board.cmake`. `src/hal/*.h` is the canonical HAL interface; chip implementations live in `src/hal/<chip>/`. The shared `src/main.c` is the production entry point; Nexcyber's 766-line bring-up harness becomes a separate bench CMake target. Each phase keeps both the `rippleon` and `nexcyber` builds green.

**Tech Stack:** CMake 3.20+ / Ninja, `arm-none-eabi-gcc` (Cortex-M3 + M4F toolchain files), FreeRTOS, GD32F20x SPL + Nations N32G45x SPL, host-side `ctest`.

**Reference spec:** `docs/superpowers/specs/2026-05-14-multi-mcu-board-structure-design.md`

**Branch:** all work on `multi-mcu-board-structure` (already created off `nexcyber-port-skeleton`).

---

## Shared validation commands

These are referenced by name throughout. Board slugs are `rippleon`/`nexcyber` **before Task 3** and `rippleon-roc001`/`nexcyber-zbu011k` **from Task 3 onward**.

- **BUILD-RIPPLEON:** `cmake -S . -B build/rippleon-roc001 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake -DOPENEVCHARGER_BOARD=<rippleon-slug> && cmake --build build/rippleon-roc001`
- **BUILD-NEXCYBER:** `cmake -S . -B build/nexcyber-zbu011k -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain-cm4f.cmake -DOPENEVCHARGER_BOARD=<nexcyber-slug> && cmake --build build/nexcyber-zbu011k`
- **HOST-TESTS:** `cmake -S tests -B build/host && cmake --build build/host && ctest --test-dir build/host --output-on-failure`
- **DISASM:** `arm-none-eabi-objdump -d <build-dir>/openevcharger.elf | sed '/OPENEVCHARGER_VERSION\|OPENEVCHARGER_GIT_SHA/d' > <name>.asm`

A fresh `cmake -S . -B …` is required after any task that moves files or edits `CMakeLists.txt`/`board.cmake` (delete the build dir first).

---

## File Structure

**New files:**
- `cmake/options.cmake` — the ~15 bench/debug `OPENEVCHARGER_*` feature flags, applied to the production target.
- `boards/rippleon-roc001/board.cmake` — GD32F205 SDK paths, sources, defs, linker.
- `boards/rippleon-roc001/pin_map.h` — moved from `src/core/pin_map.h`.
- `boards/rippleon-roc001/gd32f205vg.ld` — moved from `linker/gd32f205vc.ld`.
- `boards/nexcyber-zbu011k/board.cmake` — N32G45x SDK paths, sources, defs, linker, **and** the bench-harness target.
- `boards/nexcyber-zbu011k/bench/bringup_main.c` — moved from `boards/nexcyber/main.c`.
- `src/hal/oevc_hal_stub.h` — the `OEVC_HAL_STUB()` trap macro.
- `src/hal/n32g45x/*.c` — moved from `boards/nexcyber/hal/`, plus stub `.c` files for unported + divergent-API HAL.
- `src/hal/n32g45x/{adc_scan_nx,gfci_nx,relay_nx}.h` — the three genuinely divergent nexcyber HAL headers, renamed.
- `src/drivers/` — chip-independent external-device drivers promoted out of `src/hal/`.

**Heavily modified files:**
- `CMakeLists.txt` — collapses from 476 lines of `if/elseif` to a board-agnostic core.
- `src/main.c` — chip-specific early init extracted behind a `board_early_init()` hook.
- `.gitignore`, `.github/workflows/firmware.yml`, `BOARDS.md`, `README.md`, `boards/*/README.md`.

**Moved (git mv, content unchanged):**
- `src/hal/<chip-coupled>.c` → `src/hal/gd32f205/`
- `boards/nexcyber/hal/*` → `src/hal/n32g45x/`

---

## Phase 1 — CMake decomposition (no file moves)

### Task 1: Capture pre-refactor baseline

**Files:** none modified — produces reference artifacts under `/tmp`.

- [ ] **Step 1: Build both boards + host tests at the current tree**

Run BUILD-RIPPLEON and BUILD-NEXCYBER with the **current** slugs (`rippleon`, `nexcyber`), then HOST-TESTS.
Expected: all three succeed; `ctest` reports the full suite passing.

- [ ] **Step 2: Capture reference disassemblies**

```bash
arm-none-eabi-objdump -d build/rippleon-roc001/openevcharger.elf | sed '/OPENEVCHARGER_VERSION\|OPENEVCHARGER_GIT_SHA/d' > /tmp/baseline-rippleon.asm
arm-none-eabi-objdump -d build/nexcyber-zbu011k/openevcharger.elf  | sed '/OPENEVCHARGER_VERSION\|OPENEVCHARGER_GIT_SHA/d' > /tmp/baseline-nexcyber.asm
wc -l /tmp/baseline-*.asm
```
Expected: two non-empty `.asm` files. These are the regression/preservation references for Tasks 5 and 7.

- [ ] **Step 3: No commit** — baseline only; nothing changed in the tree.

---

### Task 2: Extract `cmake/options.cmake`

**Files:**
- Create: `cmake/options.cmake`
- Modify: `CMakeLists.txt` (remove the bench-flag blocks from the `rippleon` branch; add one `include()`)

- [ ] **Step 1: Create `cmake/options.cmake`**

Move every bench/debug feature-flag block out of the `rippleon` branch of `CMakeLists.txt` into this file verbatim. It is `include()`d after the production target exists, so it operates on `${TARGET}`:

```cmake
# cmake/options.cmake — build-time / bench feature flags.
#
# Included by CMakeLists.txt AFTER the production target is created, so
# every block here operates on ${TARGET}. These are transient debug/bench
# knobs, board-independent. Hardware-fact defaults (HXTAL, GFCI CAL
# timing, PE-continuity topology) live in boards/<board>/board.cmake, not
# here.

# Default OFF unless explicitly passed via cmake.
if(OPENEVCHARGER_SEMIHOSTING)
    target_compile_definitions(${TARGET} PRIVATE OPENEVCHARGER_SEMIHOSTING=1)
endif()
if(DEFINED OPENEVCHARGER_WS2812_LEDS)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_WS2812_LEDS=${OPENEVCHARGER_WS2812_LEDS})
endif()
if(DEFINED OPENEVCHARGER_WS2812_INVERT)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_WS2812_INVERT=${OPENEVCHARGER_WS2812_INVERT})
endif()
if(DEFINED OPENEVCHARGER_LED_PROTOCOL_UCS1903)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_LED_PROTOCOL_UCS1903=${OPENEVCHARGER_LED_PROTOCOL_UCS1903})
endif()
if(DEFINED OPENEVCHARGER_LED_PROTOCOL_APA106)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_LED_PROTOCOL_APA106=${OPENEVCHARGER_LED_PROTOCOL_APA106})
endif()
if(DEFINED OPENEVCHARGER_LED_PROTOCOL_SK6812_RGBW)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_LED_PROTOCOL_SK6812_RGBW=${OPENEVCHARGER_LED_PROTOCOL_SK6812_RGBW})
endif()
if(DEFINED OPENEVCHARGER_LED_FORCE_GREEN)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_LED_FORCE_GREEN=${OPENEVCHARGER_LED_FORCE_GREEN})
endif()
if(DEFINED OPENEVCHARGER_BENCH_CRASH_RESET)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_BENCH_CRASH_RESET=${OPENEVCHARGER_BENCH_CRASH_RESET})
endif()
if(DEFINED OPENEVCHARGER_REAL_120M_PLL)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_REAL_120M_PLL=${OPENEVCHARGER_REAL_120M_PLL})
endif()
if(DEFINED OPENEVCHARGER_BL0939_SMOKE)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_BL0939_SMOKE=${OPENEVCHARGER_BL0939_SMOKE})
endif()
if(DEFINED OPENEVCHARGER_CC_DETECTOR)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_CC_DETECTOR=${OPENEVCHARGER_CC_DETECTOR})
endif()
# Override of the board.cmake default (boards/*/board.cmake sets the
# hardware-fact default; this lets a bench run flip it).
if(DEFINED OPENEVCHARGER_GFCI_CAL_SELF_TEST)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_GFCI_CAL_SELF_TEST=${OPENEVCHARGER_GFCI_CAL_SELF_TEST})
endif()
if(NOT DEFINED OPENEVCHARGER_OTA_APPLY_ENABLED)
    set(OPENEVCHARGER_OTA_APPLY_ENABLED 1)
endif()
target_compile_definitions(${TARGET} PRIVATE
    OPENEVCHARGER_OTA_APPLY_ENABLED=${OPENEVCHARGER_OTA_APPLY_ENABLED})
if(DEFINED OPENEVCHARGER_OTA_TEST_MARKER)
    target_compile_definitions(${TARGET} PRIVATE
        OPENEVCHARGER_OTA_TEST_MARKER=${OPENEVCHARGER_OTA_TEST_MARKER})
endif()
if(NOT DEFINED OPENEVCHARGER_STACK_WATCH)
    set(OPENEVCHARGER_STACK_WATCH 1)
endif()
target_compile_definitions(${TARGET} PRIVATE
    OPENEVCHARGER_STACK_WATCH=${OPENEVCHARGER_STACK_WATCH})
```

- [ ] **Step 2: Remove those blocks from `CMakeLists.txt` and `include()` instead**

In the `rippleon` branch of `CMakeLists.txt`, delete the "Bench-time options" comment and every `if(...OPENEVCHARGER_*...)` block listed above (they now live in `options.cmake`). Leave the two hardware-fact defines in place for now — `GD32F20X_CL=1`, `HXTAL_VALUE=8000000`, `OPENEVCHARGER_GFCI_CAL_SELF_TEST=1`, `OPENEVCHARGER_PE_CONTINUITY_DETECTOR=0` (they move in Task 3).

In the shared tail of `CMakeLists.txt`, immediately after the `target_compile_definitions(${TARGET} PRIVATE OPENEVCHARGER_GIT_SHA=...)` block, add:

```cmake
# Build-time / bench feature flags (board-independent).
include(${CMAKE_SOURCE_DIR}/cmake/options.cmake)
```

- [ ] **Step 3: Build both boards + host tests**

Run BUILD-RIPPLEON, BUILD-NEXCYBER (current slugs), HOST-TESTS.
Expected: all green. `nexcyber` is unaffected (it never had the flag blocks).

- [ ] **Step 4: Disassembly check (rippleon)**

```bash
arm-none-eabi-objdump -d build/rippleon-roc001/openevcharger.elf | sed '/OPENEVCHARGER_VERSION\|OPENEVCHARGER_GIT_SHA/d' > /tmp/t2-rippleon.asm
diff /tmp/baseline-rippleon.asm /tmp/t2-rippleon.asm && echo "IDENTICAL"
```
Expected: `IDENTICAL` — moving the flag blocks to an `include()` changes no codegen with default flags.

- [ ] **Step 5: Commit**

```bash
git add cmake/options.cmake CMakeLists.txt
git commit -m "cmake: extract bench feature flags into cmake/options.cmake"
```

---

### Task 3: Extract per-board `board.cmake`, slim `CMakeLists.txt`, adopt new slugs

**Files:**
- Create: `boards/rippleon-roc001/board.cmake`, `boards/nexcyber-zbu011k/board.cmake`
- Modify: `CMakeLists.txt` (collapse the `if/elseif`)

This task introduces the new slugs `rippleon-roc001` / `nexcyber-zbu011k`. From here on, all validation uses the new slugs.

- [ ] **Step 1: Create `boards/rippleon-roc001/board.cmake`**

Lift the entire `if(OPENEVCHARGER_BOARD STREQUAL "rippleon")` body from `CMakeLists.txt` into this file, with these transformations:
- Keep the `set(VENDOR_LIB …)` / `set(STDP_SRCS …)` / `set(FREERTOS_* …)` / `set(APP_SRCS …)` blocks verbatim.
- Keep `add_executable(${TARGET} …)`, `target_include_directories(${TARGET} …)` verbatim.
- Keep the two hardware-fact `target_compile_definitions` (`GD32F20X_CL=1`, `HXTAL_VALUE=8000000`) and **move the board-fact safety defines here too**:
  ```cmake
  target_compile_definitions(${TARGET} PRIVATE
      GD32F20X_CL=1
      HXTAL_VALUE=8000000
      # Hardware-fact defaults for this PCBA. Overridable per bench run
      # via cmake/options.cmake.
      OPENEVCHARGER_GFCI_CAL_SELF_TEST=1
      OPENEVCHARGER_PE_CONTINUITY_DETECTOR=0
  )
  ```
- Keep `set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/linker/gd32f205vc.ld)` — path unchanged for now (Task 4 moves it).
- Do **not** include the bench-flag blocks (already in `options.cmake`).

- [ ] **Step 2: Create `boards/nexcyber-zbu011k/board.cmake`**

Lift the entire `elseif(OPENEVCHARGER_BOARD STREQUAL "nexcyber")` body into this file verbatim — the `NX_VENDOR` paths, `NX_SPL_SRCS`, `FREERTOS_*` (ARM_CM4F), `APP_SRCS` (with `boards/nexcyber/main.c` + `boards/nexcyber/hal/*` paths unchanged for now), `add_executable`, `target_include_directories`, `target_compile_definitions` (`N32G45X=1`, `HSE_VALUE=8000000`, `USE_STDPERIPH_DRIVER=1`), and `set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/boards/nexcyber/n32g457.ld)`.

- [ ] **Step 3: Collapse `CMakeLists.txt`**

Replace the whole `if(rippleon) … elseif(nexcyber) … endif()` block (everything from `# Board selection` setup's target-specific part through the `endif()` before the shared tail) with board selection + an `include`. The full new `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)

# The toolchain file MUST be set on the cmake command line:
#   -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake       (rippleon-roc001, Cortex-M3)
#   -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain-cm4f.cmake  (nexcyber-zbu011k, Cortex-M4F)
project(openevcharger C ASM)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ---------- Board selection ----------
if(NOT DEFINED OPENEVCHARGER_BOARD)
    set(OPENEVCHARGER_BOARD "rippleon-roc001")
endif()
set(OPENEVCHARGER_BOARD "${OPENEVCHARGER_BOARD}" CACHE STRING
    "Target board (a dir under boards/)")
set_property(CACHE OPENEVCHARGER_BOARD PROPERTY STRINGS
    rippleon-roc001 nexcyber-zbu011k)
message(STATUS "OpenEVCharger board: ${OPENEVCHARGER_BOARD}")
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/boards/${OPENEVCHARGER_BOARD}/board.cmake")
    message(FATAL_ERROR
        "Unknown OPENEVCHARGER_BOARD '${OPENEVCHARGER_BOARD}' — no "
        "boards/${OPENEVCHARGER_BOARD}/board.cmake")
endif()

set(TARGET ${PROJECT_NAME})

# ---------- Build info (board-independent) ----------
# [KEEP VERBATIM: the entire existing git describe / rev-parse /
#  configure_file(.git/HEAD) block, unchanged.]

# ---------- Board target ----------
# board.cmake creates ${TARGET} (the production firmware) and sets
# LINKER_SCRIPT. The nexcyber board.cmake additionally defines the
# bench-harness target.
include(${CMAKE_SOURCE_DIR}/boards/${OPENEVCHARGER_BOARD}/board.cmake)

# ---------- Shared: build-info defines, options, link, post-build ----------
# [KEEP VERBATIM: the existing _OEVC_GIT_VERSION / _OEVC_GIT_SHA
#  target_compile_definitions block.]

include(${CMAKE_SOURCE_DIR}/cmake/options.cmake)

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

The two `[KEEP VERBATIM]` blocks are the existing git-build-info code from the current `CMakeLists.txt` — copy them across unchanged.

- [ ] **Step 4: Build both boards (new slugs) + host tests**

Run BUILD-RIPPLEON and BUILD-NEXCYBER with the **new** slugs (`rippleon-roc001`, `nexcyber-zbu011k`), then HOST-TESTS.
Expected: all green.

- [ ] **Step 5: Disassembly check (both boards)**

```bash
arm-none-eabi-objdump -d build/rippleon-roc001/openevcharger.elf | sed '/OPENEVCHARGER_VERSION\|OPENEVCHARGER_GIT_SHA/d' > /tmp/t3-rippleon.asm
arm-none-eabi-objdump -d build/nexcyber-zbu011k/openevcharger.elf | sed '/OPENEVCHARGER_VERSION\|OPENEVCHARGER_GIT_SHA/d' > /tmp/t3-nexcyber.asm
diff /tmp/baseline-rippleon.asm /tmp/t3-rippleon.asm && echo "RIPPLEON IDENTICAL"
diff /tmp/baseline-nexcyber.asm /tmp/t3-nexcyber.asm && echo "NEXCYBER IDENTICAL"
```
Expected: both `IDENTICAL` — a pure CMake reorganisation changes no codegen.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt boards/rippleon-roc001/board.cmake boards/nexcyber-zbu011k/board.cmake
git commit -m "cmake: split if/elseif into per-board board.cmake; adopt PCBA slugs"
```

---

## Phase 2 — Relocate board config

### Task 4: Move pin maps + linkers into `boards/<board>/`

**Files:**
- Move: `src/core/pin_map.h` → `boards/rippleon-roc001/pin_map.h`
- Move: `linker/gd32f205vc.ld` → `boards/rippleon-roc001/gd32f205vg.ld`
- Move: `boards/nexcyber/pin_map.h` → `boards/nexcyber-zbu011k/pin_map.h`
- Move: `boards/nexcyber/n32g457.ld` → `boards/nexcyber-zbu011k/n32g45x.ld`
- Modify: both `board.cmake` files; any `#include "core/pin_map.h"` references

- [ ] **Step 1: Move the files with `git mv`**

```bash
git mv src/core/pin_map.h boards/rippleon-roc001/pin_map.h
git mv linker/gd32f205vc.ld boards/rippleon-roc001/gd32f205vg.ld
git mv boards/nexcyber/pin_map.h boards/nexcyber-zbu011k/pin_map.h
git mv boards/nexcyber/n32g457.ld boards/nexcyber-zbu011k/n32g45x.ld
rmdir linker
```

- [ ] **Step 2: Update `boards/rippleon-roc001/board.cmake`**

- Change the linker line to `set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/boards/rippleon-roc001/gd32f205vg.ld)`.
- In `target_include_directories(${TARGET} PRIVATE …)`, add `${CMAKE_SOURCE_DIR}/boards/rippleon-roc001` (so `#include "pin_map.h"` resolves).

- [ ] **Step 3: Update `boards/nexcyber-zbu011k/board.cmake`**

- Change the linker line to `set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/boards/nexcyber-zbu011k/n32g45x.ld)`.
- In `target_include_directories`, change the `boards/nexcyber` entry to `${CMAKE_SOURCE_DIR}/boards/nexcyber-zbu011k`.

- [ ] **Step 4: Fix the `pin_map.h` include in `src/main.c`**

`src/main.c` line 13 is `#include "core/pin_map.h"`. The rippleon board dir is now on the include path, so change it to:
```c
#include "pin_map.h"
```
Check for any other `core/pin_map.h` references: `grep -rn 'core/pin_map.h' src/` — fix each the same way (rippleon-side code only; nexcyber HAL already uses `"pin_map.h"`).

- [ ] **Step 5: Build both boards + host tests**

Delete `build/` dirs, run BUILD-RIPPLEON, BUILD-NEXCYBER, HOST-TESTS.
Expected: all green. (Host tests do not touch `pin_map.h`, so they are unaffected.)

- [ ] **Step 6: Disassembly check (both boards)**

Same as Task 3 Step 5 but write to `/tmp/t4-*.asm`. Expected: both `IDENTICAL` vs baseline.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "boards: relocate pin maps + linker scripts under boards/<board>/"
```

---

## Phase 3 — GD32 HAL into a per-chip directory

### Task 5: Move chip-coupled `src/hal/*.c` → `src/hal/gd32f205/`

**Files:**
- Move: 17 chip-coupled `src/hal/*.c` → `src/hal/gd32f205/` (headers stay in `src/hal/`)
- Modify: `boards/rippleon-roc001/board.cmake` (APP_SRCS paths + include dir)

- [ ] **Step 1: Move the chip-coupled `.c` files (keep the `.h` in `src/hal/`)**

```bash
mkdir -p src/hal/gd32f205
for f in clock wdg uart gpio adc_scan adc_inject cp_pwm relay gfci rtc bl0939 spi3 flash uart5 rfid ws2812 w25q; do
    git mv src/hal/$f.c src/hal/gd32f205/$f.c
done
```
All 17 `src/hal/*.h` files remain in `src/hal/` — they are the shared interface.

- [ ] **Step 2: Update `boards/rippleon-roc001/board.cmake` APP_SRCS**

In the `set(APP_SRCS …)` block, change every `src/hal/<x>.c` entry to `src/hal/gd32f205/<x>.c`. The 17 lines become:
```cmake
    src/hal/gd32f205/clock.c
    src/hal/gd32f205/wdg.c
    src/hal/gd32f205/uart.c
    src/hal/gd32f205/gpio.c
    src/hal/gd32f205/adc_scan.c
    src/hal/gd32f205/adc_inject.c
    src/hal/gd32f205/cp_pwm.c
    src/hal/gd32f205/relay.c
    src/hal/gd32f205/gfci.c
    src/hal/gd32f205/rtc.c
    src/hal/gd32f205/bl0939.c
    src/hal/gd32f205/spi3.c
    src/hal/gd32f205/flash.c
    src/hal/gd32f205/uart5.c
    src/hal/gd32f205/rfid.c
    src/hal/gd32f205/ws2812.c
    src/hal/gd32f205/w25q.c
```
`src/hal/` is already on the include path via the existing `src` entry plus an explicit `src/hal` is **not** needed (sources `#include "hal/clock.h"`). Confirm `target_include_directories` still has `src` — it does; no change needed there.

- [ ] **Step 3: Build rippleon + host tests**

Delete `build/rippleon-roc001`, run BUILD-RIPPLEON, HOST-TESTS.
Expected: green. (Nexcyber untouched this task — skip its build to save time, or run it; it must still pass.)

- [ ] **Step 4: GD32 regression gate (disassembly)**

```bash
arm-none-eabi-objdump -d build/rippleon-roc001/openevcharger.elf | sed '/OPENEVCHARGER_VERSION\|OPENEVCHARGER_GIT_SHA/d' > /tmp/t5-rippleon.asm
diff /tmp/baseline-rippleon.asm /tmp/t5-rippleon.asm && echo "RIPPLEON IDENTICAL"
```
Expected: `IDENTICAL`. A pure file move changes no codegen. **If this diffs, stop and investigate before continuing.**

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "hal: move GD32-coupled implementations into src/hal/gd32f205/"
```

---

### Task 6: Promote chip-independent drivers to `src/drivers/`

**Files:**
- Move: `src/hal/gd32f205/w25q.c` → `src/drivers/w25q.c`; `src/hal/w25q.h` → `src/drivers/w25q.h`
- Modify: `boards/rippleon-roc001/board.cmake`; `src/main.c` and any `#include "hal/w25q.h"`

Scope decision (from the spec's per-file rule): **only `w25q` is promoted in this task.** `bl0939` stays chip-coupled — the GD32 build bit-bangs SPI and the N32 build uses hardware SPI2; they are genuinely two implementations, not one portable driver (confirmed by the Task-8 header diff). `ws2812` stays chip-coupled (precise bit-bang timing). `rfid` stays chip-coupled (the `src/hal/rfid.c` vs `src/core/rfid.c` split is unresolved — out of scope here).

- [ ] **Step 1: Move w25q**

```bash
mkdir -p src/drivers
git mv src/hal/gd32f205/w25q.c src/drivers/w25q.c
git mv src/hal/w25q.h src/drivers/w25q.h
```

- [ ] **Step 2: Update includes**

`w25q.c` includes its own header — change `#include "hal/w25q.h"` (or `"w25q.h"`) inside `src/drivers/w25q.c` to `#include "w25q.h"`. In `src/main.c`, `tests/`, and anywhere else, change `#include "hal/w25q.h"` → `#include "drivers/w25q.h"`:
```bash
grep -rn 'hal/w25q.h' src/ tests/
```
Fix each hit to `drivers/w25q.h`. Note `tests/CMakeLists.txt` references `${SRC_DIR}/hal` on the include path — add `${SRC_DIR}/drivers` there too (Step 4).

- [ ] **Step 3: Update `boards/rippleon-roc001/board.cmake`**

Change `src/hal/gd32f205/w25q.c` in APP_SRCS to `src/drivers/w25q.c`. Add `src/drivers` is reachable via the existing `src` include entry (`#include "drivers/w25q.h"`), no extra include dir needed.

- [ ] **Step 4: Update `tests/CMakeLists.txt`**

In `target_include_directories(openevcharger_pure PUBLIC …)` and `target_include_directories(openevcharger_w25q_mock PUBLIC …)`, add `${SRC_DIR}/drivers`. The w25q mock (`tests/mocks/w25q_mock.c`) implements `hal/w25q.h`'s surface — it now needs to see `drivers/w25q.h`; update its `#include` accordingly.

- [ ] **Step 5: Build rippleon + host tests**

Delete `build/rippleon-roc001` and `build/host`, run BUILD-RIPPLEON, HOST-TESTS.
Expected: green — including the `pingpong` / `boot_config` suites that link the w25q mock.

- [ ] **Step 6: GD32 regression gate**

Disassembly diff vs `/tmp/baseline-rippleon.asm` → write `/tmp/t6-rippleon.asm`. Expected: `IDENTICAL`.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "drivers: promote w25q SPI-NOR driver to src/drivers/ (chip-independent)"
```

---

## Phase 4 — N32 HAL into a per-chip directory + bench harness

### Task 7: Move `boards/nexcyber/` HAL + harness into place

**Files:**
- Move: `boards/nexcyber/hal/*` → `src/hal/n32g45x/`
- Move: `boards/nexcyber/main.c` → `boards/nexcyber-zbu011k/bench/bringup_main.c`
- Move: `boards/nexcyber/README.md` → `boards/nexcyber-zbu011k/README.md`
- Modify: `boards/nexcyber-zbu011k/board.cmake`

This task is a pure relocation — header reconciliation is Task 8, so guards/shadowing are left alone here and the bench-harness target is wired to keep building byte-identically.

- [ ] **Step 1: Move the files**

```bash
mkdir -p src/hal/n32g45x boards/nexcyber-zbu011k/bench
git mv boards/nexcyber/hal/* src/hal/n32g45x/
git mv boards/nexcyber/main.c boards/nexcyber-zbu011k/bench/bringup_main.c
git mv boards/nexcyber/README.md boards/nexcyber-zbu011k/README.md
rmdir boards/nexcyber/hal boards/nexcyber
```

- [ ] **Step 2: Rewrite `boards/nexcyber-zbu011k/board.cmake` to define a bench-harness target**

The current nexcyber `board.cmake` builds `${TARGET}` (the `openevcharger` production name) from `boards/nexcyber/main.c` + the partial HAL. Re-point it to the new paths, and rename the executable target so it is unambiguously the bench harness — `${TARGET}` (the production name) is claimed by `src/main.c` in Task 10. New `boards/nexcyber-zbu011k/board.cmake` structure:

```cmake
# Nexcyber (Nations N32G45x / Cortex-M4F).
#
# Defines TWO targets:
#   openevcharger                  — production firmware (src/main.c + full
#                                    src/hal/n32g45x/). Wired in Task 10;
#                                    until then this file defines only the
#                                    bench harness so the board still builds.
#   openevcharger-nexcyber-bringup — the M0-M4 bring-up harness, the image
#                                    actually flashed during bench work.

# ---------- Vendor lib paths (Nations SDK) ----------
# [KEEP VERBATIM: the existing NX_VENDOR / NX_CORE / NX_VARIANT /
#  NX_STARTUP / NX_SPL_INC / NX_SPL_SRC / NX_STARTUP_SRC / NX_SYSTEM_SRC /
#  NX_SPL_SRCS blocks.]

# ---------- FreeRTOS (ARM_CM4F) ----------
# [KEEP VERBATIM: the existing FREERTOS_DIR / FREERTOS_PORT_DIR /
#  FREERTOS_SRCS blocks.]

# ---------- Bench-harness target ----------
set(BRINGUP_SRCS
    boards/nexcyber-zbu011k/bench/bringup_main.c
    src/hal/n32g45x/clock.c
    src/hal/n32g45x/uart.c
    src/hal/n32g45x/gpio.c
    src/hal/n32g45x/adc_scan.c
    src/hal/n32g45x/cp_pwm.c
    src/hal/n32g45x/spi2.c
    src/hal/n32g45x/bl0939.c
    src/hal/n32g45x/nextion.c
    src/hal/n32g45x/relay.c
    src/hal/n32g45x/gfci.c
    src/hal/n32g45x/led_ring.c
    src/core/j1772.c
)
add_executable(openevcharger-nexcyber-bringup
    ${BRINGUP_SRCS}
    ${FREERTOS_SRCS}
    ${NX_STARTUP_SRC}
    ${NX_SYSTEM_SRC}
    ${NX_SPL_SRCS}
)
target_include_directories(openevcharger-nexcyber-bringup PRIVATE
    src/hal/n32g45x          # board-specific HAL headers win over src/hal/*.h
    boards/nexcyber-zbu011k  # pin_map.h
    src
    ${NX_CORE} ${NX_VARIANT} ${NX_SPL_INC}
    ${FREERTOS_DIR}/include ${FREERTOS_PORT_DIR}
)
target_compile_definitions(openevcharger-nexcyber-bringup PRIVATE
    N32G45X=1
    HSE_VALUE=8000000
    USE_STDPERIPH_DRIVER=1
)
target_link_options(openevcharger-nexcyber-bringup PRIVATE
    -T${CMAKE_SOURCE_DIR}/boards/nexcyber-zbu011k/n32g45x.ld
    -Wl,-Map=openevcharger-nexcyber-bringup.map,--cref
)
set_target_properties(openevcharger-nexcyber-bringup PROPERTIES SUFFIX ".elf")
add_custom_command(TARGET openevcharger-nexcyber-bringup POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:openevcharger-nexcyber-bringup> openevcharger-nexcyber-bringup.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:openevcharger-nexcyber-bringup>
    BYPRODUCTS openevcharger-nexcyber-bringup.bin
)

# ---------- Production target placeholder ----------
# Wired in Task 10. For now alias it to the bench harness so CMakeLists.txt's
# shared tail (which expects ${TARGET}) has something to attach to, and the
# board still produces a build.
add_executable(${TARGET} ${BRINGUP_SRCS}
    ${FREERTOS_SRCS} ${NX_STARTUP_SRC} ${NX_SYSTEM_SRC} ${NX_SPL_SRCS})
target_include_directories(${TARGET} PRIVATE
    src/hal/n32g45x boards/nexcyber-zbu011k src
    ${NX_CORE} ${NX_VARIANT} ${NX_SPL_INC}
    ${FREERTOS_DIR}/include ${FREERTOS_PORT_DIR})
target_compile_definitions(${TARGET} PRIVATE
    N32G45X=1 HSE_VALUE=8000000 USE_STDPERIPH_DRIVER=1)
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/boards/nexcyber-zbu011k/n32g45x.ld)
```

The `[KEEP VERBATIM]` blocks are the existing nexcyber-branch code, copied across with no change.

- [ ] **Step 3: Build both boards + host tests**

Delete `build/` dirs, run BUILD-RIPPLEON, BUILD-NEXCYBER, HOST-TESTS.
Expected: all green. `cmake --build build/nexcyber-zbu011k` now builds two ELFs — `openevcharger.elf` and `openevcharger-nexcyber-bringup.elf`.

- [ ] **Step 4: Nexcyber preservation gate (disassembly)**

```bash
arm-none-eabi-objdump -d build/nexcyber-zbu011k/openevcharger-nexcyber-bringup.elf | sed '/OPENEVCHARGER_VERSION\|OPENEVCHARGER_GIT_SHA/d' > /tmp/t7-nexcyber.asm
diff /tmp/baseline-nexcyber.asm /tmp/t7-nexcyber.asm && echo "NEXCYBER BRINGUP IDENTICAL"
```
Expected: `IDENTICAL` — the bench harness is the same sources as the old `nexcyber` target, just relocated. **If this diffs, stop and investigate.**

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "hal: relocate Nexcyber HAL to src/hal/n32g45x/, harness to bench target"
```

---

### Task 8: Reconcile the shadow headers

**Files:**
- Delete: `src/hal/n32g45x/cp_pwm.h`, `src/hal/n32g45x/bl0939.h`
- Rename: `src/hal/n32g45x/adc_scan.h` → `adc_scan_nx.h`, `gfci.h` → `gfci_nx.h`, `relay.h` → `relay_nx.h`
- Modify: the matching `src/hal/n32g45x/*.c` includes; `boards/nexcyber-zbu011k/bench/bringup_main.c`; `src/hal/bl0939.h` (none needed — already the superset)

Per-file outcomes were determined by diffing each shadow against `src/hal/*.h` (see the spec's "Open items" and the design investigation):

| shadow | verdict | action |
|---|---|---|
| `cp_pwm.h` | surface identical (guard + comments only) | **delete shadow**, use shared `src/hal/cp_pwm.h` |
| `bl0939.h` | identical function surface + struct; `src/hal/bl0939.h` is a superset (extra reg `#define`s) | **delete shadow**, use shared `src/hal/bl0939.h` |
| `adc_scan.h` | divergent API (4-rank + `adc2_*` vs 11-rank) | **rename** to `adc_scan_nx.h` — board-specific |
| `gfci.h` | divergent API (`gfci_cal_pulse` vs `gfci_fault_active`/`gfci_self_test`) | **rename** to `gfci_nx.h` — board-specific |
| `relay.h` | divergent API (latch close-pulse/hold vs direct contactor) | **rename** to `relay_nx.h` — board-specific |

- [ ] **Step 1: Delete the two matching shadows**

```bash
git rm src/hal/n32g45x/cp_pwm.h src/hal/n32g45x/bl0939.h
```
`src/hal/n32g45x/cp_pwm.c` and `bl0939.c` already `#include "hal/cp_pwm.h"` / `"hal/bl0939.h"`; with the shadows gone these resolve to `src/hal/*.h`. No `.c` edits needed if the include path keeps `src/hal/n32g45x` *before* `src` — confirm `cp_pwm.c`/`bl0939.c` compile against the shared headers (they were written to "mirror the surface", so they should). If `bl0939.c` references a register `#define` only in the shared (superset) header, that is fine — it now sees more, not fewer.

- [ ] **Step 2: Rename the three divergent shadows**

```bash
git mv src/hal/n32g45x/adc_scan.h src/hal/n32g45x/adc_scan_nx.h
git mv src/hal/n32g45x/gfci.h     src/hal/n32g45x/gfci_nx.h
git mv src/hal/n32g45x/relay.h    src/hal/n32g45x/relay_nx.h
```
Update the include guards inside each to match (`OPENEVCHARGER_HAL_N32G45X_ADC_SCAN_H`, etc.).

- [ ] **Step 3: Update the `.c` includes for the renamed headers**

In `src/hal/n32g45x/adc_scan.c`, `gfci.c`, `relay.c`, change `#include "hal/adc_scan.h"` → `#include "adc_scan_nx.h"` (and `gfci_nx.h`, `relay_nx.h`). In `boards/nexcyber-zbu011k/bench/bringup_main.c`, change the three corresponding includes from `"hal/adc_scan.h"` / `"hal/gfci.h"` / `"hal/relay.h"` to `"adc_scan_nx.h"` / `"gfci_nx.h"` / `"relay_nx.h"`. The bench harness's include path has `src/hal/n32g45x` first, so the bare names resolve.

- [ ] **Step 4: Build both boards + host tests**

Delete `build/` dirs, run BUILD-RIPPLEON, BUILD-NEXCYBER, HOST-TESTS.
Expected: all green. The `openevcharger-nexcyber-bringup` ELF still builds; rippleon and host are untouched in spirit.

- [ ] **Step 5: Nexcyber preservation gate**

Disassembly diff `openevcharger-nexcyber-bringup.elf` vs `/tmp/baseline-nexcyber.asm` → `/tmp/t8-nexcyber.asm`. Expected: `IDENTICAL` — renaming headers and deleting redundant ones changes no codegen.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "hal: reconcile Nexcyber shadow headers — share cp_pwm/bl0939, isolate divergent adc_scan/gfci/relay"
```

---

## Phase 5 — Nexcyber production target

### Task 9: Add the HAL stub macro + `src/hal/n32g45x/` stubs

**Files:**
- Create: `src/hal/oevc_hal_stub.h`
- Create: `src/hal/n32g45x/{rtc,flash,spi3,uart5,rfid,ws2812,wdg,adc_inject}.c` (unported-HAL stubs)
- Create: `src/hal/n32g45x/{adc_scan,gfci,relay}_shared_stub.c` (shared-interface stubs for the divergent peripherals)

The production target (Task 10) compiles `src/main.c` + the full shared core, which references the **shared** `src/hal/*.h` surface for every HAL module. Nexcyber has real impls for some, divergent impls for three, and nothing for the rest. Every shared-interface function it does not genuinely implement gets an `OEVC_HAL_STUB()` body so the production target links. The bench harness does **not** use these stubs.

- [ ] **Step 1: Create `src/hal/oevc_hal_stub.h`**

```c
#ifndef OPENEVCHARGER_HAL_OEVC_HAL_STUB_H
#define OPENEVCHARGER_HAL_OEVC_HAL_STUB_H

/* Marks a HAL function that exists only so the production firmware target
 * for a board links — it is NOT a working implementation. Calling one at
 * runtime traps with interrupts disabled so a debugger breaks in cleanly.
 *
 * Greppable: `grep -rn OEVC_HAL_STUB src/hal/<chip>/` lists everything a
 * board still owes a real implementation. A board with zero hits is
 * feature-complete against the shared HAL interface.
 *
 * The production target for a board carrying these is a compile/link gate
 * only (see docs/superpowers/specs/2026-05-14-multi-mcu-board-structure-design.md);
 * the bench-harness target is what actually runs on hardware. */
#define OEVC_HAL_STUB() do { __asm volatile("cpsid i"); for (;;) {} } while (0)

#endif
```

- [ ] **Step 2: Create the unported-HAL stub `.c` files**

For each of `rtc`, `flash`, `spi3`, `uart5`, `rfid`, `ws2812`, `wdg`, `adc_inject`: create `src/hal/n32g45x/<name>.c` that includes the shared header and stubs every declared function. Example — `src/hal/n32g45x/wdg.c` (use the actual signatures from `src/hal/wdg.h`):

```c
/* src/hal/n32g45x/wdg.c — STUB. The shared src/hal/wdg.h interface is
 * not yet implemented for the N32G45x. Present only so the Nexcyber
 * production target links. See src/hal/oevc_hal_stub.h. */
#include "hal/wdg.h"
#include "oevc_hal_stub.h"

void wdg_init(void)   { OEVC_HAL_STUB(); }
void wdg_kick(void)   { OEVC_HAL_STUB(); }
/* ...one stub body per function declared in src/hal/wdg.h... */
```

For each file, open the corresponding `src/hal/<name>.h`, copy every function declaration, and emit a definition with an `OEVC_HAL_STUB();` body (return a zero value of the right type where the signature is non-void, after the trap — unreachable, but keeps the compiler quiet). Do **not** stub `bl0939`, `cp_pwm`, `clock`, `uart`, `gpio` — those have real N32 implementations already.

- [ ] **Step 3: Create the divergent-peripheral shared-interface stubs**

`adc_scan`, `gfci`, `relay` have real N32 impls, but against their *own* (`*_nx.h`) APIs — not the shared `src/hal/*.h` surface the core calls. Create `src/hal/n32g45x/adc_scan_shared_stub.c`, `gfci_shared_stub.c`, `relay_shared_stub.c`, each implementing the **shared** header's surface with `OEVC_HAL_STUB()` bodies. Example — `src/hal/n32g45x/relay_shared_stub.c`:

```c
/* src/hal/n32g45x/relay_shared_stub.c — STUB of the shared src/hal/relay.h
 * interface. The N32G45x has a real relay driver, but against a divergent
 * API (relay_nx.h: SR-latch close-pulse + hold model). Genuine
 * reconciliation of the two APIs is M5+ future work. Present only so the
 * Nexcyber production target links. See src/hal/oevc_hal_stub.h. */
#include "hal/relay.h"
#include "oevc_hal_stub.h"

void relay_main_open(void)            { OEVC_HAL_STUB(); }
void relay_main_close(void)           { OEVC_HAL_STUB(); }
int  relay_main_commanded(void)       { OEVC_HAL_STUB(); return 0; }
int  relay_main_sense_closed(void)    { OEVC_HAL_STUB(); return 0; }
uint16_t relay_main_sense_raw(void)   { OEVC_HAL_STUB(); return 0; }
void relay_force_open_latch(void)     { OEVC_HAL_STUB(); }
void relay_force_open_release(void)   { OEVC_HAL_STUB(); }
int  relay_force_open_active(void)    { OEVC_HAL_STUB(); return 0; }
void relay_aux_open(void)             { OEVC_HAL_STUB(); }
void relay_aux_close(void)            { OEVC_HAL_STUB(); }
int  relay_aux_commanded(void)        { OEVC_HAL_STUB(); return 0; }
```
Do the same for `adc_scan_shared_stub.c` (against `src/hal/adc_scan.h`'s `adc_scan_init` / `adc_scan_latest` / `adc_scan_rank` and the 11-rank enum) and `gfci_shared_stub.c` (against `src/hal/gfci.h`'s `gfci_fault_active` / `gfci_self_test`).

- [ ] **Step 4: Build both boards + host tests**

These new files are not yet in any target, so this is a no-op build check — confirm BUILD-RIPPLEON, BUILD-NEXCYBER, HOST-TESTS still pass after adding untracked source files.
Expected: green (new files inert until Task 10 wires them in).

- [ ] **Step 5: Commit**

```bash
git add src/hal/oevc_hal_stub.h src/hal/n32g45x/
git commit -m "hal/n32g45x: add OEVC_HAL_STUB macro + stubs for unported + divergent HAL"
```

---

### Task 10: Wire the Nexcyber production target

**Files:**
- Create: `src/hal/board_init.h` (the `board_early_init()` hook interface)
- Create: `src/hal/gd32f205/board_init.c`, `src/hal/n32g45x/board_init.c`
- Modify: `src/main.c` (extract chip-specific early init); `boards/rippleon-roc001/board.cmake`, `boards/nexcyber-zbu011k/board.cmake`

- [ ] **Step 1: Create `src/hal/board_init.h`**

```c
#ifndef OPENEVCHARGER_HAL_BOARD_INIT_H
#define OPENEVCHARGER_HAL_BOARD_INIT_H

/* Board/chip-specific early init, called once by main() immediately after
 * uart_init() and before any other HAL bring-up. Wraps everything in the
 * boot path that is not portable across MCUs: vendor clock fix-ups, debug-
 * pin remaps, AF clock enables. Implemented per-chip in
 * src/hal/<chip>/board_init.c. */
void board_early_init(void);

#endif
```

- [ ] **Step 2: Create `src/hal/gd32f205/board_init.c`** — the real GD32 early init, lifted from `src/main.c`

```c
/* src/hal/gd32f205/board_init.c — GD32F205 early init. */
#include "hal/board_init.h"
#include "hal/clock.h"
#include "gd32f20x.h"

void board_early_init(void)
{
    /* If built with OPENEVCHARGER_REAL_120M_PLL=1, swap the SDK's broken
     * 120m_hxtal config for a clean direct chain. No-op otherwise. */
    clock_real_120m_init();

    /* Release JTAG pins (PA15, PB3, PB4) so SPI3 (PB3/PB4/PB5) and
     * TIMER1_CH0 (PA15) can use them. SWDPENABLE keeps SWD alive
     * (PA13/PA14) for the OpenOCD probe. */
    rcu_periph_clock_enable(RCU_AF);
    gpio_pin_remap_config(GPIO_SWJ_SWDPENABLE_REMAP, ENABLE);
}
```

- [ ] **Step 3: Create `src/hal/n32g45x/board_init.c`** — stub

```c
/* src/hal/n32g45x/board_init.c — N32G45x early init. STUB: the production
 * target's boot path is not yet bench-validated on this chip. The bench
 * harness (boards/nexcyber-zbu011k/bench/bringup_main.c) owns the real N32
 * bring-up. See src/hal/oevc_hal_stub.h. */
#include "hal/board_init.h"
#include "oevc_hal_stub.h"

void board_early_init(void) { OEVC_HAL_STUB(); }
```

- [ ] **Step 4: Edit `src/main.c` to call the hook**

Remove the GD32-specific lines (the `clock_real_120m_init();` call at line 69, and the `rcu_periph_clock_enable(RCU_AF);` + `gpio_pin_remap_config(...)` pair at lines 100-101), and the now-unused `#include "gd32f20x.h"` at line 10. Add `#include "hal/board_init.h"`. Insert the hook call right after `uart_init()` + the boot banner. The edited region of `main()`:

```c
    uart_init();
    printk("\n--- OpenEVCharger boot, SystemCoreClock=%u Hz ---\n",
           (unsigned)SystemCoreClock);
#if defined(OPENEVCHARGER_OTA_TEST_MARKER) && OPENEVCHARGER_OTA_TEST_MARKER
    printk("*** OTA-APPLIED v%d ***\n", (int)(OPENEVCHARGER_OTA_TEST_MARKER));
#endif
    clock_log_status();

    /* Chip-specific early init: vendor clock fix-ups, debug-pin remaps,
     * AF clock enables. Implemented per-chip in src/hal/<chip>/board_init.c. */
    board_early_init();

    rtc_init();
    /* ...unchanged from here... */
```
Leave the `rtc_init()` block, the peripheral-init sequence, the persist loads, and task creation exactly as they are — they call the shared HAL interface, which resolves to real impls on GD32 and to real-or-stub on N32.

- [ ] **Step 5: Add `board_init.c` to the rippleon source list**

In `boards/rippleon-roc001/board.cmake` APP_SRCS, add `src/hal/gd32f205/board_init.c`.

- [ ] **Step 6: Replace the Nexcyber production-target placeholder**

In `boards/nexcyber-zbu011k/board.cmake`, replace the "Production target placeholder" block (the `add_executable(${TARGET} ${BRINGUP_SRCS} …)` from Task 7 Step 2) with the real production target — `src/main.c` + the full shared app stack + the complete `src/hal/n32g45x/` set (real impls + stubs):

```cmake
# ---------- Production target ----------
set(PROD_APP_SRCS
    src/main.c
    src/core/fault.c        src/core/j1772.c       src/core/over_temp.c
    src/core/rfid.c         src/core/system_state.c src/core/system_time.c
    src/persist/crc.c       src/persist/boot_count.c src/persist/pingpong.c
    src/persist/boot_config.c src/persist/calibration.c src/persist/crc16.c
    src/persist/event_log.c src/persist/session_log.c src/persist/crash_state.c
    src/persist/rfid_authlist.c src/persist/ota_stage.c
    src/proto/tlv.c
    src/ui/buttons.c        src/ui/buzzer.c        src/ui/led_patterns.c
    src/tasks/safety_task.c src/tasks/io_task.c    src/tasks/comms_task.c
    src/tasks/fc41d_flash_helper.c src/tasks/persist_task.c
    src/diag/stack_watch.c
    src/drivers/w25q.c
    # N32 HAL — real implementations
    src/hal/n32g45x/clock.c    src/hal/n32g45x/uart.c    src/hal/n32g45x/gpio.c
    src/hal/n32g45x/cp_pwm.c   src/hal/n32g45x/bl0939.c  src/hal/n32g45x/spi2.c
    src/hal/n32g45x/board_init.c
    # N32 HAL — stubs for unported modules
    src/hal/n32g45x/rtc.c      src/hal/n32g45x/flash.c   src/hal/n32g45x/spi3.c
    src/hal/n32g45x/uart5.c    src/hal/n32g45x/rfid.c    src/hal/n32g45x/ws2812.c
    src/hal/n32g45x/wdg.c      src/hal/n32g45x/adc_inject.c
    # N32 HAL — shared-interface stubs for divergent-API peripherals
    src/hal/n32g45x/adc_scan_shared_stub.c
    src/hal/n32g45x/gfci_shared_stub.c
    src/hal/n32g45x/relay_shared_stub.c
)
add_executable(${TARGET}
    ${PROD_APP_SRCS}
    ${FREERTOS_SRCS} ${NX_STARTUP_SRC} ${NX_SYSTEM_SRC} ${NX_SPL_SRCS})
target_include_directories(${TARGET} PRIVATE
    src                         # shared headers: hal/*.h, core/*, etc.
    boards/nexcyber-zbu011k     # pin_map.h
    ${NX_CORE} ${NX_VARIANT} ${NX_SPL_INC}
    ${FREERTOS_DIR}/include ${FREERTOS_PORT_DIR})
target_compile_definitions(${TARGET} PRIVATE
    N32G45X=1 HSE_VALUE=8000000 USE_STDPERIPH_DRIVER=1)
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/boards/nexcyber-zbu011k/n32g45x.ld)
```
Note: the production target's include path puts `src` first and **does not** include `src/hal/n32g45x` — so `#include "hal/adc_scan.h"` resolves to the *shared* header (matching the `_shared_stub.c` files), while the bench-harness target keeps `src/hal/n32g45x` first for `adc_scan_nx.h`. The real N32 `adc_scan.c`/`gfci.c`/`relay.c` are bench-harness-only and are **not** in `PROD_APP_SRCS`.

- [ ] **Step 7: Build everything + host tests**

Delete `build/` dirs. Run BUILD-RIPPLEON, BUILD-NEXCYBER, HOST-TESTS.
Expected: all green. `build/nexcyber-zbu011k` now produces both `openevcharger.elf` (production — links via stubs) and `openevcharger-nexcyber-bringup.elf`.

- [ ] **Step 8: GD32 regression gate**

Disassembly diff `build/rippleon-roc001/openevcharger.elf` vs `/tmp/baseline-rippleon.asm` → `/tmp/t10-rippleon.asm`. Expected: `IDENTICAL` — extracting `board_early_init()` is a pure refactor; the GD32 impl runs the same instructions in the same order.

- [ ] **Step 9: Verify the production gate**

```bash
arm-none-eabi-nm build/nexcyber-zbu011k/openevcharger.elf | grep -c ' T main'
arm-none-eabi-size build/nexcyber-zbu011k/openevcharger.elf
```
Expected: `main` present; non-zero `.text`. The production target configures, compiles, and links — the multi-MCU "unblock" deliverable. It is not functional (stub bodies trap); the bench harness is what runs.

- [ ] **Step 10: Commit**

```bash
git add -A
git commit -m "main: extract board_early_init() hook; wire Nexcyber production target"
```

---

## Phase 6 — Cleanup, CI, docs

### Task 11: Repo hygiene — gitignore, stale artifacts, vendored OCPP copy

**Files:**
- Modify: `.gitignore`
- Delete: `fc41d/.esphome/external_components/8ed4226f/` (if tracked), committed `build*/` dirs, `tools/.ntc_peek_baseline.json` (if tracked)

- [ ] **Step 1: Audit what is actually tracked**

```bash
git ls-files | grep -E '(^build|/build_|\.esphome/|__pycache__|\.pyc$|external_components/8ed4226f)' || echo "nothing tracked"
```
This lists tracked files that should be ignored. Note the output for Step 3.

- [ ] **Step 2: Tighten `.gitignore`**

The current `.gitignore` already covers `build/`, `build_*/`, `__pycache__/`, `*.pyc`, `fc41d/.esphome/`, `build_host/`, `tools/.ntc_peek_baseline.json`. Add the one missing pattern — a repo-wide `.esphome/` (currently only `fc41d/.esphome/` is ignored):

```
# ESPHome build cache (any location)
.esphome/
```
Place it under the existing "ESPHome FC41D firmware" comment block and remove the now-redundant `fc41d/.esphome/` line.

- [ ] **Step 3: Remove tracked files that should be ignored**

For each path from Step 1's output:
```bash
git rm -r --cached <path>
```
In particular, if `fc41d/.esphome/external_components/8ed4226f/` is tracked (the stale vendored OCPP copy), remove it from the index — `.gitignore`'s `.esphome/` rule then keeps it out. If `git ls-files` showed nothing, this step is a no-op; record that.

- [ ] **Step 4: Verify the working firmware build still ignores cleanly**

```bash
git status --short | grep -E '(^build|\.esphome|__pycache__)' && echo "STILL LEAKING" || echo "clean"
```
Expected: `clean`.

- [ ] **Step 5: Commit**

```bash
git add .gitignore
git add -u
git commit -m "repo: tighten .gitignore, drop tracked build artifacts + stale vendored OCPP copy"
```

---

### Task 12: Update CI for the new slugs + target names

**Files:**
- Modify: `.github/workflows/firmware.yml` (and `host-tests.yml` / `fc41d-config.yml` if they reference board names or build paths)

- [ ] **Step 1: Read the current workflows**

```bash
cat .github/workflows/firmware.yml .github/workflows/host-tests.yml .github/workflows/fc41d-config.yml
```
Identify every reference to `OPENEVCHARGER_BOARD=rippleon` / `=nexcyber`, the `build_rippleon` / `build_nexcyber` / `build_host` paths, and the matrix definition.

- [ ] **Step 2: Update `firmware.yml`**

In the build matrix, replace the board entries with the new slugs and add the explicit toolchain file + build dir per board. The matrix entries become:
```yaml
matrix:
  include:
    - board: rippleon-roc001
      toolchain: cmake/arm-none-eabi-toolchain.cmake
      continue-on-error: false
    - board: nexcyber-zbu011k
      toolchain: cmake/arm-none-eabi-toolchain-cm4f.cmake
      continue-on-error: true   # production target is a compile-gate until M0-M3 bench-validated
```
The configure/build step:
```yaml
- name: Configure + build ${{ matrix.board }}
  continue-on-error: ${{ matrix.continue-on-error }}
  run: |
    cmake -S . -B build/${{ matrix.board }} -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=${{ matrix.toolchain }} \
      -DOPENEVCHARGER_BOARD=${{ matrix.board }}
    cmake --build build/${{ matrix.board }}
```
This builds all targets for the board — for `nexcyber-zbu011k` that is both `openevcharger.elf` (the compile-gate) and `openevcharger-nexcyber-bringup.elf`.

- [ ] **Step 3: Update `host-tests.yml` / `fc41d-config.yml` if needed**

`host-tests.yml` should already invoke `cmake -S tests -B build/host` — confirm it does and uses `build/host` (matches the convention); fix the path if it uses `build_host`. `fc41d-config.yml` builds the ESPHome companion config — it does not reference MCU board slugs; change nothing unless it hard-codes a `build_*` path.

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/
git commit -m "ci: update firmware matrix for PCBA slugs; nexcyber production target continue-on-error"
```

CI runs on push — the workflow result is verified after the branch is pushed (it cannot be run locally).

---

### Task 13: Rewrite the docs to describe the structure as-built

**Files:**
- Modify: `BOARDS.md`, `README.md`, `boards/nexcyber-zbu011k/README.md`
- Create: `boards/rippleon-roc001/README.md`

- [ ] **Step 1: Update `BOARDS.md`**

The current `BOARDS.md` "board-specific surface" section describes `src/core/pin_map_<board>.h` and `linker/<chip>.ld` — neither of which is how it works now. Rewrite that section to describe the as-built model: `boards/<board>/` holds `board.cmake` + `pin_map.h` + the linker script; `src/hal/*.h` is the shared interface; `src/hal/<chip>/` holds the per-chip implementation; board-specific peripherals get a `*_nx.h`-style header in the chip dir; a board with `OEVC_HAL_STUB`s in its `src/hal/<chip>/` has an incomplete production target. Update the "Porting outline" steps to match. Update the Nexcyber row to note the two targets (production compile-gate + bench harness).

- [ ] **Step 2: Update `README.md`**

The `README.md` "board-specific surface is small: a pin map, a linker script, and a vendor-SDK wiring block in CMake" sentence is now accurate in spirit — confirm it points at `boards/<board>/board.cmake`. Update the build instructions to the new slugs and the `build/<board>/` convention. Add a one-line note that Nexcyber currently ships a bench-harness target plus a compile-gated production target.

- [ ] **Step 3: Update `boards/nexcyber-zbu011k/README.md`**

It currently documents the island layout (`hal/clock.c` etc. under the board dir). Rewrite the "What's here" table to the new layout: `board.cmake`, `pin_map.h`, `n32g45x.ld`, `bench/bringup_main.c`; note that the N32 HAL now lives in `src/hal/n32g45x/`. Update the "Build" block to:
```bash
cmake -S . -B build/nexcyber-zbu011k -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain-cm4f.cmake \
    -DOPENEVCHARGER_BOARD=nexcyber-zbu011k
cmake --build build/nexcyber-zbu011k --target openevcharger-nexcyber-bringup
```
Keep the bench-blocked / roadmap sections — they are still accurate.

- [ ] **Step 4: Create `boards/rippleon-roc001/README.md`**

A short file mirroring the nexcyber one's header: what the dir holds (`board.cmake`, `pin_map.h`, `gd32f205vg.ld`), the build command (BUILD-RIPPLEON with the real slug), and a one-liner that this is the production-validated target (M7 + real-EV session, no bench harness needed).

- [ ] **Step 5: Commit**

```bash
git add BOARDS.md README.md boards/rippleon-roc001/README.md boards/nexcyber-zbu011k/README.md
git commit -m "docs: describe the as-built multi-MCU board structure"
```

---

## Self-Review

**Spec coverage:**
- Spec §1 (layout) → Tasks 2-10 produce every element of the target tree.
- Spec §2 (board-config mechanism) → Tasks 2, 3.
- Spec §3 (HAL split + drivers/) → Tasks 5, 6, 7, 8; the divergent-API finding (adc_scan/gfci/relay) is handled in Tasks 8-10.
- Spec §4 (production main vs bench harness) → Tasks 7 (harness target), 10 (production main + hook).
- Spec §5 (N32 SDK / companion tidy / build dirs) → N32 SDK already vendored (no task needed); companion tidy + build dirs → Task 11.
- Spec §5 (CI) → Task 12.
- Spec §6 (migration sequencing + validation gates) → the Phase/Task order *is* the spec's 6 steps; disassembly/host-test gates are in Tasks 1, 2, 3, 4, 5, 6, 7, 8, 10.
- Spec "open items": shadow-header per-file verdicts → resolved in Task 8's table; `bl0939` one-vs-two → resolved (two; stays chip-coupled, Task 6); `board_early_init()` shape → Task 10 Step 1; `rfid` split → explicitly deferred (Task 6 note); CMake target names → `openevcharger` (production) + `openevcharger-nexcyber-bringup` (bench), used consistently.

**Placeholder scan:** The `[KEEP VERBATIM]` markers in Tasks 3 and 7 point at specific, existing blocks in the current `CMakeLists.txt` — they are lift-and-shift instructions for code already in the repo, not unwritten content. All new glue code (options.cmake, the slim CMakeLists, the stub macro/files, board_init) is shown in full.

**Type/name consistency:** Board slugs `rippleon-roc001` / `nexcyber-zbu011k` used throughout from Task 3 on. Target names `openevcharger` / `openevcharger-nexcyber-bringup` consistent across Tasks 7, 10, 12, 13. `OEVC_HAL_STUB()` defined in Task 9 Step 1, used in Tasks 9-10. `board_early_init()` declared once (Task 10 Step 1), implemented twice, called once. Header names `adc_scan_nx.h` / `gfci_nx.h` / `relay_nx.h` consistent across Task 8 and the Task 7/10 include-path notes.
