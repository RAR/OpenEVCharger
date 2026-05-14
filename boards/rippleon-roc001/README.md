# OpenEVCharger — Rippleon ROC001 board

**Status:** Production-validated. Bench-validated through M7 + a real
240 V EV charging session (2026-05-07). All safety detectors live
(see [`README.md`](../../README.md) safety table). No bench-harness
target needed — the production `openevcharger` target is the image
that runs on deployed hardware.

This is the GD32F205VG target for the Rippleon ROC family of AC EVSE
wallboxes (FCC ID `2BLBS-ROCHL2US`; ODM: Beizide / NewEnergyCS). Full
hardware details and sibling-SKU coverage in
[`BOARDS.md`](../../BOARDS.md).

## What's here

| File | Purpose |
|---|---|
| `board.cmake` | CMake board definition: GD32F20x SDK paths, Cortex-M3 flags, GD32 HAL source list, linker script wiring, single `openevcharger` production target |
| `pin_map.h` | Full GPIO pin → function map, reverse-engineered from stock V1.0.066 SWD dump |
| `gd32f205vg.ld` | Linker: 1 MB FLASH (lower 512 KB used; upper 512 KB OTA staging) + 128 KB RAM |

The GD32F205 HAL implementations live in `src/hal/gd32f205/`.

## Build

```bash
cmake -S . -B build/rippleon-roc001 -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
    -DOPENEVCHARGER_BOARD=rippleon-roc001
cmake --build build/rippleon-roc001
```

Output: `build/rippleon-roc001/openevcharger.{elf,bin,hex,map}`.

## Flashing

Back up stock firmware before first flash (round-trip-validated):

```bash
./tools/stock_backup.sh
```

Then flash via SWD (ST-Link V2, 4-pin SWD wiring to the GD32F205VG):

```bash
./tools/flash.sh
```

Subsequent updates are HA-mediated OTA (no SWD needed) — see
[Updating a deployed unit](../../README.md#updating-a-deployed-unit).
