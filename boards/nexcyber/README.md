# OpenEVCharger — Nexcyber board port (in progress)

**Status:** Bring-up skeleton, **not yet buildable**.

This directory holds the board-specific scaffolding for porting
OpenEVCharger to the Nexcyber AC EVSE (Tuya-based, Nations N32G45x main
MCU + Tuya WBR2 / RTL8720CF Wi-Fi/BLE module). It also covers the
broader family of Tuya-MCU EVSEs that share the same DP map (see
`esphome/testcharger/NOTES.md` in the parent device-configs repo).

For overall feasibility, effort estimate, and architectural decisions
see [`docs/ports/nexcyber-feasibility.md`](../../docs/ports/nexcyber-feasibility.md)
in the OpenEVCharger root.

## What's here

| File | Purpose | Status |
|---|---|---|
| `pin_map.h` | All confirmed pins from the 2026-05-07 SWD firmware dump | 27 of ~36 pins confirmed; 9 ULN2003 outputs + 3 candidate digital inputs need bench mains-on resolution |
| `n32g457.ld` | Linker: 120 KB FLASH + 8 KB PERSIST + 80 KB RAM | Drafted; assumes 2 KB sector geometry (per N32G45x reference manual) — bench-confirm before first flash erase |
| `docs/` | Per-port engineering notes (gotchas, deviations from upstream Nations SDK, etc.) | Will fill as port progresses |
| `hal/` | Board-specific HAL implementation (CM4F / N32G45x drivers) | **Empty** — Phase 2 work |

## What's NOT here yet

1. **Nations N32G45x vendor SDK.** Source: V3.1.0 mirror at
   <https://github.com/NationsTechCoreLib/N32G455xx>. License is
   3-clause-BSD-style (Nations 2019, see `firmware/CMSIS/device/n32g45x.h`).
   When ready to make this buildable:

   ```bash
   cd OpenEVCharger
   git submodule add https://github.com/NationsTechCoreLib/N32G455xx \
       third_party/N32G45x_Firmware_Library
   ```

   Layout (mirror's convention):
   - `firmware/CMSIS/{core,device}/` — CMSIS + device files
     (`n32g45x.h`, `n32g45x_conf.h`, `system_n32g45x.c/.h`, `startup/`)
   - `firmware/n32g45x_std_periph_driver/{inc,src}` — SPL drivers
     (parallel to `third_party/GD32F20x_Firmware_Library/spl/`)

2. **CMakeLists.txt board-arm.** A board-selection switch has been added
   (top of `CMakeLists.txt`) and currently `FATAL_ERROR`s when
   `-DOPENEVCHARGER_BOARD=nexcyber`. Phase 1 will replace that with
   real wiring: vendor-lib paths, startup file, FreeRTOS port
   (`portable/GCC/ARM_CM4F`), linker script.

3. **HAL implementations.** None of the GD32F20x HAL .c files in
   `src/hal/` are reusable — the F1-style register layout means
   GPIO_CRL/CRH (4-bit nibble per pin) instead of MODER/OTYPER/OSPEEDR/
   PUPDR. The full rewrite list per
   `docs/ports/nexcyber-feasibility.md`:

   | Layer | Status |
   |---|---|
   | GPIO HAL | not started |
   | UART HAL | not started |
   | ADC HAL | not started |
   | Timer HAL (CP PWM) | not started |
   | Clock tree (RCC) | not started |
   | OTA staging path | redesign — single-bank, no self-rollback |
   | Persistence ping-pong | redesign — internal-flash (this file's PERSIST region) |

4. **Bench validations not yet run** (deferred until mains-on bench):
   - Mains-on wiggle to identify the 9 silent OUT_PP pins (ULN2003 inputs).
   - Scope each USART TX during stock boot to ID protocols.
   - Confirm SPI2 vs USART3 is the BL0939 link.
   - Confirm the 3 digital-input candidates (PC3 / PC7 / PC9) for
     L1+L2 mains-presence (TLP293-2 photocoupler outputs at 60 Hz
     pulse rate).
   - Pinpoint which ADC channel = CP by correlating the SRAM cache at
     `0x2000075c` against scope readings at idle (+12 V on CP).

## How to (eventually) build

Once the SDK is vendored and `CMakeLists.txt` is wired up:

```bash
cmake -S . -B build_nexcyber \
    -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain-cm4f.cmake \
    -DOPENEVCHARGER_BOARD=nexcyber
cmake --build build_nexcyber
```

The rippleon target (`OPENEVCHARGER_BOARD=rippleon`, default) is
unaffected by these changes and continues to build with the existing
M3 toolchain file.

## Dependent repos

- ESPHome side: this firmware will pair with a future nexcyber YAML in
  `esphome/testcharger/openevcharger_nexcyber.yaml` (not yet authored)
  targeting the WBR2's RTL8720CF via `rtl87xx`. The TLV component
  (`fc41d/components/openevcharger_tlv/`) reuses verbatim — only the
  YAML platform / pin assignments differ.

## References

- Pinout source-of-truth: `esphome/testcharger/NOTES.md` § "Nations
  N32G45x main MCU pinout" (and the `stock-mcu-2026-05-07.bin` SWD dump
  beside it; gitignored, sha256 in NOTES.md).
- Feasibility / effort estimate: `../../docs/ports/nexcyber-feasibility.md`
- BOARDS.md (lists this as the second supported port).
