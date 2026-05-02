# GD32F20x Firmware Library

**Vendor:** GigaDevice Semiconductor Inc.
**Source firmware version:** V2.2.0 (2020-09-30) per per-file `\version` headers
**Vendored from:** https://github.com/CommunityGD32Cores/gd32-pio-spl-package
**Upstream commit:** 431ebdea002f24af68b61d18c2111d451b4d4dc7
**Date vendored:** 2026-05-02

## Upstream layout adaptation

CommunityGD32Cores' SPL package keeps the GigaDevice firmware library files
verbatim but reorganises them under a `gd32/` namespace shared across many
chip families. We extracted only the GD32F20x-relevant subset:

| Vendored path | Upstream source path |
|---|---|
| `cmsis/cores/gd32/` | `gd32/cmsis/cores/gd32/` (cortex-m3 subset only) |
| `cmsis/variants/gd32f20x/` | `gd32/cmsis/variants/gd32f20x/` |
| `cmsis/startup_files/startup_gd32f20x_cl.S` | `gd32/cmsis/startup_files/startup_gd32f20x_cl.S` |
| `spl/inc/gd32f20x_*.h` | `gd32/spl/variants/gd32f20x/inc/gd32f20x_*.h` |
| `spl/src/gd32f20x_*.c` | `gd32/spl/variants/gd32f20x/src/gd32f20x_*.c` |

`startup_gd32f20x_cl.S` is the GCC-syntax startup for the connectivity-line
family (GD32F205/F207). The `_cl` suffix matches the `GD32F20X_CL` define
that gates connectivity-line code paths in `gd32f20x.h`.

## License

GigaDevice ships these files under a **3-clause BSD license** (see the
copyright block at the top of any vendored `.c` or `.h`). Compatible with
the OpenBHZD GPL-3.0 license: BSD attribution must be preserved (we don't
modify the headers). When distributing OpenBHZD binaries we acknowledge
GigaDevice in `docs/attribution.md` and ship this README.

## What's NOT vendored

- USB FS device library (`GD32F20x_usbfs_library/`) — not needed for safety core
- LCD eval, NAND eval, SDRAM eval samples — not relevant
- FAT filesystem — not used
- Other GD32 chip families (E10x, F1x0, F30x, etc.) — different MCU

## Updating

To upgrade to a newer SPL release:
1. `git pull` the CommunityGD32Cores repo.
2. Re-run the same `cp` commands used to vendor (preserved in the M0 plan task 3 history).
3. Update version + commit hash in this README.
4. Build + flash + smoke-test M0 (LED blinks at 1 Hz).

If GigaDevice publishes a new vendor zip on gd32mcu.com (the original
distribution channel), it can be substituted manually:
1. Download the zip from gd32mcu.com → F2-Series → Resources.
2. Replace the `spl/` and `cmsis/variants/gd32f20x/` content from the new
   zip's `Firmware/GD32F20x_standard_peripheral/` and
   `Firmware/CMSIS/GD/GD32F20x/` respectively.
3. Update version metadata.
