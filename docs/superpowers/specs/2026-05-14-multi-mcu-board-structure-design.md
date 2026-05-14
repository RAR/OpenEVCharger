# Multi-MCU Board Structure — Design

**Date:** 2026-05-14
**Status:** Approved (design); implementation plan pending
**Scope:** OpenEVCharger repository structure only

## Problem

OpenEVCharger's `BOARDS.md` / `README.md` describe a board-portability
model — per-board pin maps, per-chip linker scripts, CMake board
selection — that `main` does not implement. And a substantial port
branch, `nexcyber-port-skeleton` (33 commits, M0–M4 of the Nexcyber /
N32G45x bring-up), has grown an **island-model** structure that is *not*
the model anyone intends to keep:

- `main`'s `CMakeLists.txt` hardcodes the GD32F20x SPL, `ARM_CM3`
  FreeRTOS port, and `linker/gd32f205vc.ld` — no board selection.
- `nexcyber-port-skeleton` adds board selection, but as a 476-line
  `if(rippleon)…elseif(nexcyber)…endif()` — two copy-pasted target
  definitions in one file.
- That branch keeps `src/` (Rippleon/GD32) untouched and puts the
  entire Nexcyber port in `boards/nexcyber/` as a parallel island: its
  own `main.c` (766 lines), its own `hal/` with its own `.c` and `.h`
  files. Board HAL headers "win" over `src/hal/*.h` via include-path
  shadowing (`-I boards/nexcyber` before `-I src`).

The MCU board axis is heading to at least three chips — GD32F205VG
(Rippleon), N32G45x (Nexcyber), STM32F334 (Delta EVMU30, MCU not yet
touched) — across different Cortex-M cores and vendor SDKs. The island
model duplicates `main.c` and HAL per board and does not scale.

## What `nexcyber-port-skeleton` already has

This restructure is a **reconciliation of that branch**, not a
greenfield build. The branch's substance is bench-validated and must be
fully preserved:

- Nations N32G45x SDK vendored (`third_party/N32G45x_Firmware_Library/`,
  ~30k lines).
- `cmake/arm-none-eabi-toolchain-cm4f.cmake` — the Cortex-M4F toolchain.
- `OPENEVCHARGER_BOARD` CMake selector (`rippleon` / `nexcyber`).
- `boards/nexcyber/pin_map.h` — pins locked via real bench fault
  injection; `boards/nexcyber/n32g457.ld`.
- M0–M4 Nexcyber HAL: `clock`, `uart`, `gpio`, `adc_scan`, `cp_pwm`,
  `relay`, `gfci`, `bl0939`, `spi2`, `nextion`, `led_ring`.
- `boards/nexcyber/main.c` — a 766-line bring-up harness.
- ~14 bench tools, N32 flash tooling, CI matrix build.

Two facts make the reconciliation tractable:

1. **The porter kept HAL function surfaces deliberately aligned.**
   `boards/nexcyber/hal/cp_pwm.h` says outright it "mirrors the surface
   of `src/hal/cp_pwm.h`." `clock`/`gpio`/`uart` have *no* shadow
   header and already include the shared `src/hal/*.h`.
2. **The core is already board-independent.** `src/core/j1772.c` is
   compiled into the Nexcyber target unchanged.

## Requirements

| Decision | Choice |
|---|---|
| Scope | OpenEVCharger repo only |
| Driver | Unblock multi-MCU support — replace the island model with a real portability boundary |
| HAL approach | A — portable `src/hal/*.h` interface + per-chip `src/hal/<chip>/` implementations |
| Entry point | Shared production `src/main.c`; Nexcyber's 766-line bring-up harness preserved as a **separate bench target**, not the firmware main |
| Board-specific peripherals | Allowed — boards have genuinely different peripheral sets (`nextion`/`led_ring` vs `ws2812`/`w25q`); they live in `src/hal/<chip>/` with their own headers, no shared-interface counterpart required |
| Companion (`fc41d/`) | Minimal tidy only — no restructure |
| Hard constraint | The GD32/Rippleon target is bench-validated through M7 + a real EV session; it must not regress. All `nexcyber-port-skeleton` bench-validated work must be preserved. |

## Approach

**Approach A — interface/impl HAL split + a `boards/` config tree.**
`src/hal/*.h` becomes the canonical portable interface contract;
implementations move to per-chip dirs (`src/hal/gd32f205/`,
`src/hal/n32g45x/`). `boards/<board>/` holds config only — `board.cmake`,
`pin_map.h`, linker script. CMake selects via `-DOPENEVCHARGER_BOARD=`.

Rejected alternatives: **B** (shared HAL + per-chip vendor shim — leaks
into `#ifdef` sprawl because GD32 SPL and the N32 SDK differ at the
peripheral-model level) and **C** (CMake selection only, HAL stays flat
— this is essentially what `nexcyber-port-skeleton` became; the island
model is the problem being solved).

## Section 1 — Target directory layout

```
OpenEVCharger/
  CMakeLists.txt              # slimmed, board-agnostic; includes the selected board.cmake + cmake/options.cmake
  cmake/
    arm-none-eabi-toolchain.cmake        # kept — base arm-none-eabi setup
    arm-none-eabi-toolchain-cm4f.cmake   # kept — from nexcyber-port-skeleton
    options.cmake             # NEW — the ~15 bench OPENEVCHARGER_* feature flags moved out of CMakeLists.txt
  boards/                     # config only — one dir per bench-validated PCBA
    rippleon-roc001/
      board.cmake             # the rippleon block of the current if/elseif, extracted
      pin_map.h               # moved from src/core/pin_map.h
      gd32f205vg.ld           # moved from linker/gd32f205vc.ld
    nexcyber-zbu011k/         # renamed from boards/nexcyber/
      board.cmake             # the nexcyber block of the current if/elseif, extracted
      pin_map.h               # kept (bench-locked pins)
      n32g45x.ld              # kept (was n32g457.ld)
      bench/
        bringup_main.c        # moved from boards/nexcyber/main.c — the 766-line harness
    # delta-evmu30/           # documented as the next slot (STM32F334) — NOT created now
  src/
    main.c  FreeRTOSConfig.h  # shared production entry point + shared RTOS config
    core/    diag/  persist/  proto/  tasks/  ui/    # board-independent — unchanged
    hal/
      *.h                     # the canonical portable HAL interface contract
      gd32f205/*.c            # existing src/hal/*.c impls move here
      n32g45x/                # boards/nexcyber/hal/* moves here
        *.c                   # ported impls (clock, uart, gpio, adc_scan, cp_pwm, relay, gfci)
        nextion.{c,h}  led_ring.{c,h}  spi2.{c,h}   # board-specific peripherals (own headers)
        <stub>.c              # stubs for not-yet-ported HAL (rtc, flash, spi3, uart5, w25q, ws2812, wdg, adc_inject)
    drivers/                  # NEW — chip-independent external-device drivers (see Section 3)
  tests/                      # unchanged — host build
  third_party/
    FreeRTOS-Kernel/  GD32F20x_Firmware_Library/  N32G45x_Firmware_Library/   # all kept
  fc41d/                      # companion — minimal tidy only (Section 5)
  recovery/  tools/  docs/
  build/<board>/              # out-of-tree, gitignored
```

`boards/<board>/` is **config only** plus an optional `bench/` harness.
`linker/` and `src/core/pin_map.h` go away. `src/hal/` becomes interface
+ per-chip impl. The island `boards/nexcyber/hal/` and
`boards/nexcyber/main.c` are relocated, not deleted.

Board-dir naming: PCBA-ish slugs (`rippleon-roc001`,
`nexcyber-zbu011k`). A `boards/` entry equals one bench-validated PCBA.

## Section 2 — Board-config mechanism

CMake keeps the existing `-DOPENEVCHARGER_BOARD=<board>` cache variable
(default `rippleon-roc001`). The 476-line `if/elseif` collapses:
`CMakeLists.txt` becomes board-agnostic and does
`include(boards/${OPENEVCHARGER_BOARD}/board.cmake)`.

`boards/<board>/board.cmake` is the single source of truth for "what
this PCBA is" — the per-board block lifted out of the current
`if/elseif`:

- Vendor SDK paths + the SPL source list (`STDP_SRCS` / `NX_SPL_SRCS`).
- Startup `.S` + system `.c` file.
- FreeRTOS port dir — `ARM_CM3` (GD32) / `ARM_CM4F` (N32, STM32F334).
- Toolchain selection note — which `cmake/*-toolchain*.cmake` to pass.
- MCU compile definitions (`GD32F20X_CL=1` / `N32G45X=1`, `HXTAL`/`HSE`).
- Which `src/hal/<chip>/` impl dir to compile.
- The linker script path.
- Board-specific feature defaults that are hardware facts, not bench
  toggles (`OPENEVCHARGER_PE_CONTINUITY_DETECTOR`, GFCI CAL
  timing/self-test, `REAL_120M_PLL`).

`cmake/options.cmake` holds the ~15 genuine build-time/bench knobs
(semihosting, WS2812 overrides, bench crash reset, OTA test marker,
stack-watch, BL0939 smoke, etc.). Included *after* board.cmake so a
bench run can override a board default.

The shared tail of the current CMakeLists (git build-info block, link
options, post-build objcopy) stays in `CMakeLists.txt`, board-agnostic.

The host test build (`tests/CMakeLists.txt`) is untouched.

## Section 3 — HAL split + `src/drivers/`

**Interface contract:** `src/hal/*.h` are promoted to *the* canonical
interface. The include-path shadowing trick goes away — there is one
header per HAL module, board-independent.

**Per-chip implementations:**

- `src/hal/gd32f205/` — the current `src/hal/*.c` (chip-coupled ones)
  move here.
- `src/hal/n32g45x/` — `boards/nexcyber/hal/*.c` move here. For modules
  with a shadow header (`cp_pwm`, `adc_scan`, `bl0939`, `gfci`,
  `relay`): if the shadow's surface matches `src/hal/*.h` it is
  deleted in favour of the shared header; if it genuinely diverges,
  that divergence is reconciled (the shared interface flexes, or the
  impl adapts) — flagged per-file in the implementation plan.
- Not-yet-ported Nexcyber HAL (`rtc`, `flash`, `spi3`, `uart5`, `w25q`,
  `ws2812`, `wdg`, `adc_inject`) get **stubs** in `src/hal/n32g45x/` —
  every function present, signature matching the header, body a
  greppable `OEVC_HAL_STUB()`. Enough for the production target to
  compile + link.

**Board-specific peripherals** (`nextion`, `led_ring`, `spi2` — N32
only) live in `src/hal/n32g45x/` with their *own* headers and no shared
`src/hal/*.h` counterpart. This is expected: boards have different
peripheral sets. The shared interface covers only the common
peripherals.

**External-device drivers → `src/drivers/`:** chip-independent drivers
that ride the HAL interface (`w25q` over the SPI HAL, `bl0939` over the
UART/SPI HAL). Moved out of `src/hal/`; compiled once for all boards;
no per-chip stub needed. Provisional, validated per-file in the plan —
`w25q` is the safe bet; `ws2812` (precise bit-bang timing) stays
chip-coupled. `nexcyber-port-skeleton` already has *two* `bl0939.c`
implementations (`src/hal/` and `boards/nexcyber/hal/`) — the plan
reconciles whether bl0939 is one portable driver or genuinely two.

`FreeRTOSConfig.h` stays shared in `src/` — already confirmed
core-independent by the branch (`configPRIO_BITS=4` works for both M3
and M4F NVIC layouts); `board.cmake` selects the port dir.

## Section 4 — Entry point: production main vs. bench harness

`src/main.c` (216 lines, Rippleon M7) stays the **shared production
entry point**. It becomes board-agnostic where it must — board-specific
init goes behind a small board hook (e.g. `board_early_init()`) rather
than `#ifdef`s in `main.c`.

`boards/nexcyber/main.c` (766 lines) is a **bring-up harness**, not a
production main. It moves to `boards/nexcyber-zbu011k/bench/bringup_main.c`
and is built as its own CMake target (e.g. `openevcharger-nexcyber-bringup`)
— the actively-developed image flashed during M-milestone bench work.

So each board can produce up to two artifacts:

- **Production target** (`openevcharger`) — `src/main.c` + full
  `src/hal/<chip>/` (real impls where ported, stubs elsewhere) + the
  shared core/persist/tasks/ui stack. For Nexcyber today this
  *configures, compiles, and links* but is not functional — that is
  the multi-MCU "unblock" deliverable.
- **Bench-harness target** (Nexcyber only, for now) — `bringup_main.c`
  + just the ported HAL. What actually runs on the bench during
  bring-up.

When Nexcyber reaches feature parity the bench harness is retired and
the production target becomes its real firmware. Rippleon has no bench
harness — it is already production.

## Section 5 — N32 SDK, companion tidy, build dirs

**N32G45x SDK** is already vendored at
`third_party/N32G45x_Firmware_Library/` — kept as-is. (A future cleanup
could convert it to a submodule for consistency with FreeRTOS-Kernel /
GD32 lib, but that is out of scope here.)

**Companion minimal tidy (`fc41d/`):**

- Delete `fc41d/.esphome/external_components/8ed4226f/` — a committed
  build-cache artifact of a git-sourced external component.
- Fix `.gitignore` repo-wide: `.esphome/`, `__pycache__/`, `*.pyc`,
  `build/`, `build_*/`.
- No restructure, no rename of the companion.

**Build directories:** out-of-tree `build/<board>/` convention; `build/`
fully gitignored; committed `build*/` dirs removed.

**CI:** the branch already has a rippleon+nexcyber matrix build
(`d7936fd`). It is updated for the new board slugs and the new target
names (production vs bench-harness), nexcyber's production target gated
as a "configures + compiles" check with `continue-on-error: true` until
bench-validated through M0–M3.

## Section 6 — Migration sequencing & validation

The work happens on the **`multi-mcu-board-structure` branch off
`nexcyber-port-skeleton`** (that branch holds all the substance being
reconciled). The Rippleon/GD32 target must not regress; all Nexcyber
bench-validated work must be preserved. Sequencing keeps both the
`rippleon` build and the `nexcyber` build green at every step.

1. **CMake split.** Extract the `if/elseif` blocks into
   `boards/rippleon-roc001/board.cmake` and
   `boards/nexcyber-zbu011k/board.cmake`; extract bench flags into
   `cmake/options.cmake`; slim `CMakeLists.txt` to the board-agnostic
   core. No file moves yet — board.cmake references files in their
   current spots. → *both boards build; host tests pass.*
2. **Relocate board config.** Move pin maps and linkers into
   `boards/<board>/`; rename `boards/nexcyber/` → `boards/nexcyber-zbu011k/`;
   delete the empty `linker/`. → *both boards build; host tests pass.*
3. **GD32 HAL into per-chip dir.** `src/hal/*.c` → `src/hal/gd32f205/`;
   create `src/drivers/` and move the files that qualify. Headers stay
   in `src/hal/`. → *rippleon builds (disassembly-diffed vs pre-move);
   host tests pass.*
4. **N32 HAL into per-chip dir.** `boards/nexcyber/hal/*` →
   `src/hal/n32g45x/`; reconcile or delete shadow headers against the
   shared `src/hal/*.h`; move `boards/nexcyber/main.c` →
   `boards/nexcyber-zbu011k/bench/bringup_main.c`; wire it as the
   bench-harness CMake target. → *nexcyber bench-harness target builds
   identically; host tests pass.*
5. **Nexcyber production target.** Add `src/hal/n32g45x/` stubs for the
   unported HAL; point the production `openevcharger` target at
   `src/main.c` + the full N32 HAL; add the `board_early_init()` hook
   to `src/main.c`. → *nexcyber production target configures + compiles
   + links; rippleon still builds; host tests pass.*
6. **Cleanup + CI + docs.** Remove committed `build*/` dirs, fix
   `.gitignore`, delete the stale vendored OCPP copy, update CI for the
   new slugs/targets, rewrite `BOARDS.md` / `README.md` / the
   `boards/*/README.md` to describe the structure as-built. → *CI
   green; nexcyber production job `continue-on-error`.*

**Validation gates:**

- **GD32 regression gate:** after the move steps, the `rippleon` build
  is diffed at the disassembly level against the pre-restructure ELF,
  minus the embedded build-info string.
- **Nexcyber preservation gate:** after step 4, the nexcyber
  bench-harness ELF is disassembly-diffed against the pre-restructure
  `nexcyber` build — the bench-validated bring-up code must be
  byte-equivalent.
- **Host tests:** the `tests/` suite is green at every step.
- **Production-target gate (end state):** `nexcyber-zbu011k`'s
  production target configures, compiles, and links. Not flashed, not
  functional.
- **Bench re-validation is out of scope** — no bench unit needed. The
  disassembly diffs are the safety argument; smoke-flashes of both the
  Rippleon target and the Nexcyber bench harness are *recommended* when
  the benches are next free, but not merge blockers.

## Out of scope

- Actual N32G45x functional bring-up beyond what
  `nexcyber-port-skeleton` already has (M5+).
- Any STM32F334 / Delta EVMU30 MCU work — `boards/delta-evmu30/` is a
  documented future slot only.
- Companion (`fc41d/`) restructure or rename — minimal tidy only.
- Converting the N32 SDK to a submodule.
- Bench re-validation of either target.

## Open items to resolve during implementation

- Per-file confirmation of which `boards/nexcyber/hal/` shadow headers
  match the shared `src/hal/*.h` surface vs. genuinely diverge.
- Whether `bl0939` is one portable `src/drivers/` driver or two
  genuinely different chip-coupled implementations.
- Exact shape of the `board_early_init()` hook in `src/main.c`.
- The `src/hal/rfid.c` vs. `src/core/rfid.c` transport-vs-logic split.
- Final CMake target names for production vs. bench-harness builds.
