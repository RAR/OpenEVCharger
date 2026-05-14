# N32G45x Firmware Library

**Vendor:** Nations Technologies Inc.
**Source firmware version:** V3.1.0 (per `.version` and per-file `\version` headers)
**Vendored from:** https://github.com/NationsTechCoreLib/N32G455xx
**Upstream commit:** `5c43c149bfcde5bc37b90f711071883d3f57b0c5` (tag `V3.1.0`)
**Upstream zip:** `N32G455xx_V3.1.0.zip` (104 MB; the mirror unpacked it as a tree)
**Date vendored:** 2026-05-11

This SDK feeds the in-progress **Nexcyber** board port — Zopoise ZBU011K
PCBA on a Nations N32G45x main MCU (Cortex-M4F, STM32F1-style peripheral
layout). See `boards/nexcyber/` for the board-specific bring-up artefacts
and `BOARDS.md` for the support matrix.

## Upstream layout adaptation

The upstream mirror keeps Nations' full release tree (firmware, USB
middleware, RT-Thread middleware, demo-board projects). We extracted only
the chip-level subset needed to build the safety core, and rehomed it
under a `cmsis/` + `spl/` split that matches
`third_party/GD32F20x_Firmware_Library/`:

| Vendored path | Upstream source path |
|---|---|
| `cmsis/core/` | `firmware/CMSIS/core/` (Cortex-M4F + GCC subset only) |
| `cmsis/variants/n32g45x/` | `firmware/CMSIS/device/` (chip headers + system init) |
| `cmsis/startup_files/startup_n32g45x_gcc.S` | `firmware/CMSIS/device/startup/startup_n32g45x_gcc.s` (extension capitalised so CMake assembles via the C preprocessor) |
| `spl/inc/n32g45x_*.h` | `firmware/n32g45x_std_periph_driver/inc/n32g45x_*.h` |
| `spl/src/n32g45x_*.c` | `firmware/n32g45x_std_periph_driver/src/n32g45x_*.c` |

`startup_n32g45x_gcc.S` is the GCC-syntax startup; the EWARM and ARMCC
variants are intentionally omitted. The MPU header (`mpu_armv7.h`) is
included from upstream `core/` for completeness; the safety core does not
configure the MPU on either target today.

## Excluded from this vendor copy

To keep the third_party tree at ~2 MB instead of ~52 MB:

- `projects/n32g45x_EVAL/` (37 MB of demo-board examples)
- `middlewares/rt-thread/` (RT-Thread RTOS; we use FreeRTOS)
- `firmware/n32g45x_usbfs_driver/` (USB stack, not used)
- `firmware/n32g45x_algo_lib/` (DSP / TSC algorithm blobs, not used)
- All vendor-tooling files (`*.uvproj{,x}`, `*.eww`, `*.dep`, etc.)

To pull any of the above in later, lift them from the upstream commit
above — the mirror is dormant (last activity Nov 2023; the SDK itself was
last revved in 2023), so the pin should remain stable.

## License

Nations Technologies ships these files under a **BSD-style 2-clause
license with a non-endorsement clause** — see the boilerplate header at
the top of any vendored `.c`, `.h`, or `.s`. Compatible with GPL-3.0.
The non-endorsement clause means we cannot use "Nations" to promote
derived products without written permission; that's fine — this project
does not.

## Maintenance

To refresh against a future upstream cut: re-clone the upstream commit /
tag, drop the directory at `third_party/N32G45x_Firmware_Library/`,
re-run the curated copies listed in the layout table above, and update
the metadata header here. Any per-file modifications applied locally
should be called out below this paragraph and patched fresh on each
refresh; none exist today.
