# M0: Toolchain Bootstrap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Get a CMake + arm-none-eabi-gcc + GD32F20x vendor library project building, flashing via SWD, and blinking the heartbeat LED on PD4 at 1 Hz with the GD32F205V running at 120 MHz.

**Architecture:** Bare-metal main loop (no FreeRTOS yet — that's M1). Vendor's `startup_gd32f20x_cl.s` and `system_gd32f20x.c` provide reset vector, vector table, and clock-tree init at 120 MHz from an 8 MHz HSE crystal. CMake drives an out-of-source build producing `openevcharger.elf` and `openevcharger.bin`. Stock MCU firmware is dumped via SWD before any flash so the bench unit is always recoverable.

**Tech Stack:** CMake 3.20+, arm-none-eabi-gcc 12+, OpenOCD 0.12+, ST-Link v2/v3 (or compatible CMSIS-DAP), GigaDevice GD32F20x_Firmware_Library V2.5.x, GPL-3.0 license.

**Hardware preconditions:**
- Bench unit powered via 5 V (USB or supply, not AC — AC isn't needed for M0).
- ST-Link or CMSIS-DAP probe wired to SWD pads (SWCLK, SWDIO, GND, optional NRST). SWD pads are accessible; `pin_probe.py` already verified the SWD link works.
- Host has `arm-none-eabi-gcc`, `cmake`, `openocd` installed. On Ubuntu: `sudo apt install gcc-arm-none-eabi cmake openocd`.

**Bench unit recovery contract:** Before any new firmware is flashed, this plan dumps the stock 256 KB MCU flash to `recovery/stock-mcu-V1.0.066.bin`. If anything goes wrong in M0 or later milestones, that backup + `tools/flash_stock.sh` (added at end of M0) restores the unit to factory state.

---

## File Structure

This plan creates exactly these files in `OpenEVCharger/`:

```
OpenEVCharger/
├── README.md                     # short project description + build/flash quickstart
├── LICENSE                       # full GPL-3.0 text (fetched verbatim from gnu.org)
├── .gitignore                    # build/, recovery/*.bin, third_party/GD32F20x_*
├── CMakeLists.txt                # top-level build, names openevcharger target
├── cmake/
│   └── arm-none-eabi-toolchain.cmake   # toolchain selection + CFLAGS
├── linker/
│   └── gd32f205vc.ld             # 256 KB flash + 128 KB RAM, vector table at 0x08000000
├── src/
│   └── main.c                    # PD4 blink loop using vendor GPIO + SysTick
├── tools/
│   ├── openocd-gd32f205.cfg      # ST-Link target = STM32F2x (GD is register-compat)
│   ├── flash.sh                  # build + flash openevcharger.elf
│   ├── flash_stock.sh            # restore recovery/stock-mcu-V1.0.066.bin
│   └── stock_backup.sh           # SWD dump main flash → recovery/stock-mcu-*.bin
├── recovery/
│   └── README.md                 # explains what's in here, gitignored .bin files
├── docs/
│   └── bring-up.md               # milestone log; M0 entry added at end
└── third_party/
    └── GD32F20x_Firmware_Library/   # vendored from GigaDevice, see fetch instructions
        ├── README.md             # how this was obtained + version
        ├── Firmware/CMSIS/...    # CMSIS headers + GD startup + system_gd32f20x.c
        └── Firmware/GD32F20x_standard_peripheral/...   # peripheral drivers
```

`build/` (out-of-tree) and `recovery/*.bin` are gitignored.

**No source files inside `third_party/GD32F20x_Firmware_Library/` are modified by us.** When the vendor's `system_gd32f20x.c` needs a different HSE value, we override the macro at compile time via `-DHXTAL_VALUE=...` in CMakeLists.txt rather than editing vendor source.

---

## TDD posture for embedded bring-up

Pure-logic TDD does not apply to M0 — there is no logic, only hardware initialization. Each task's "test" is a measurable physical outcome: `arm-none-eabi-gcc --version` returns ≥ 12, `cmake --build build` produces an ELF of expected size, `openocd flash write_image` reports success, a multimeter on PD4 reads a 1 Hz square wave. Future milestones (M2+ with `j1772.c`, `tlv.c`, etc.) introduce host-built unit tests under `tests/`.

---

## Tasks

### Task 1: Project skeleton — README, LICENSE, .gitignore

**Files:**
- Create: `OpenEVCharger/README.md`
- Create: `OpenEVCharger/LICENSE`
- Create: `OpenEVCharger/.gitignore`

- [ ] **Step 1: Write `README.md`**

```markdown
# OpenEVCharger

Replacement firmware for the **Rippleon ROC001** / **NewEnergyCS ROC-family**
J1772 EV charger, targeting the GigaDevice **GD32F205V** main MCU.

OpenEVCharger is a clean-room reimplementation of OpenEVSE-style EVSE firmware.
The behavior of the J1772 state machine, fault model, and self-test sequence
is modeled on [OpenEVSE](https://github.com/OpenEVSE/open_evse) but no source
is copied. Wi-Fi/BLE/cloud features run on the FC41D Wi-Fi module, off the
safety MCU, controlled via a binary TLV protocol over UART5.

**Status:** v1 in development. See [`docs/superpowers/specs/`](docs/superpowers/specs/)
for the design spec and [`docs/superpowers/plans/`](docs/superpowers/plans/)
for the milestone-by-milestone implementation plan.

## License

GPL-3.0-only. See `LICENSE`.

## Quickstart (bench)

```sh
# 1. Install host deps
sudo apt install gcc-arm-none-eabi cmake openocd

# 2. Fetch the GD32F20x vendor library (see third_party/GD32F20x_Firmware_Library/README.md)

# 3. Configure and build
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
cmake --build build

# 4. Back up stock firmware (REQUIRED before any flash)
./tools/stock_backup.sh

# 5. Flash
./tools/flash.sh
```
```

- [ ] **Step 2: Write `LICENSE`**

Run:
```sh
curl -fsSL https://www.gnu.org/licenses/gpl-3.0.txt -o /home/rar/device-configs/esphome/rippleon/OpenEVCharger/LICENSE
```

Expected: `LICENSE` is the full GPL-3.0 text, ~35 KB.

If `curl` fails (network), fall back to:
```sh
cp /usr/share/common-licenses/GPL-3 /home/rar/device-configs/esphome/rippleon/OpenEVCharger/LICENSE
```

Verify:
```sh
head -1 /home/rar/device-configs/esphome/rippleon/OpenEVCharger/LICENSE
```
Expected output starts with: `                    GNU GENERAL PUBLIC LICENSE`

- [ ] **Step 3: Write `.gitignore`**

```gitignore
# Build artifacts
build/
*.elf
*.bin
*.hex
*.map
*.lst

# But keep our gitkeeps
!recovery/.gitkeep
!third_party/**/.gitkeep

# Recovery backups (large binaries, regenerated locally)
recovery/*.bin
recovery/*.hex

# Editor
.vscode/
.clangd/
.cache/
compile_commands.json
*.swp

# OS
.DS_Store
Thumbs.db
```

- [ ] **Step 4: Verify and commit**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
ls -la README.md LICENSE .gitignore
wc -c LICENSE
```
Expected: `README.md`, `LICENSE`, `.gitignore` exist; `LICENSE` is ≥ 30 000 bytes.

```sh
git add README.md LICENSE .gitignore
git commit -m "M0.1: project skeleton (README, GPL-3.0, gitignore)"
```

---

### Task 2: Stock-firmware backup tooling

**Why first:** The bench unit is unique. We back up before installing anything that could brick. If the OpenOCD config is wrong, this is where we'll discover it — safely, without writing.

**Files:**
- Create: `OpenEVCharger/tools/openocd-gd32f205.cfg`
- Create: `OpenEVCharger/tools/stock_backup.sh`
- Create: `OpenEVCharger/recovery/README.md`
- Create: `OpenEVCharger/recovery/.gitkeep`

- [ ] **Step 1: Write `tools/openocd-gd32f205.cfg`**

```tcl
# OpenOCD config for GD32F205VC (256 KB flash, 128 KB RAM).
# GD32F2 is register-compatible enough with STM32F2 that the stm32f2x driver
# works for connect/halt/dump. For erase+program we use a generic flash bank
# pointing at GD32's flash controller (same registers as STM32F1's FPEC).

source [find interface/stlink.cfg]
transport select hla_swd

# CPU TAP — Cortex-M3 default
source [find target/stm32f2x.cfg]

# Slow clock for first connect; pin_probe.py confirmed reliable at 1.8 MHz
adapter speed 1000

# GD32F205VC: 256 KB flash starting at 0x08000000
# Override the flash bank size that stm32f2x.cfg may set wrong for GD32
flash bank gd32f205_flash stm32f1x 0x08000000 0x40000 0 0 $_TARGETNAME

reset_config srst_only srst_nogate
```

- [ ] **Step 2: Write `tools/stock_backup.sh`**

```sh
#!/usr/bin/env bash
# Dump the GD32F205VC main flash (256 KB) to recovery/stock-mcu-V1.0.066.bin
# Run BEFORE flashing any new firmware. Always runs read-only.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$REPO_ROOT/recovery/stock-mcu-V1.0.066.bin"
CFG="$REPO_ROOT/tools/openocd-gd32f205.cfg"

mkdir -p "$REPO_ROOT/recovery"

if [[ -e "$OUT" ]]; then
    echo "Backup already exists at $OUT"
    echo "Refusing to overwrite. Move/rename it if you really want a fresh dump."
    exit 1
fi

echo "Dumping 256 KB main flash to $OUT ..."
openocd -f "$CFG" \
        -c "init" \
        -c "reset halt" \
        -c "dump_image $OUT 0x08000000 0x40000" \
        -c "reset run" \
        -c "shutdown"

SIZE=$(stat -c '%s' "$OUT")
if [[ "$SIZE" -ne 262144 ]]; then
    echo "ERROR: dump size $SIZE != 262144 expected. Backup is suspect; investigate before proceeding."
    exit 2
fi

echo "OK: $OUT ($SIZE bytes)"
sha256sum "$OUT"
```

- [ ] **Step 3: Make script executable**

```sh
chmod +x /home/rar/device-configs/esphome/rippleon/OpenEVCharger/tools/stock_backup.sh
```

- [ ] **Step 4: Write `recovery/README.md`**

```markdown
# Recovery directory

Stock-firmware backups live here. Files are gitignored (large binaries, locally
reproducible by SWD-dumping the bench unit before any flash).

## Files

| File | Source | Restore script |
|---|---|---|
| `stock-mcu-V1.0.066.bin` | SWD dump of GD32F205VC main flash, 256 KB | `tools/flash_stock.sh` (added in Task 12) |

## Recovering the bench unit

If a new flash bricks the unit, restore stock with:

```sh
./tools/flash_stock.sh
```

This writes `recovery/stock-mcu-V1.0.066.bin` back to flash starting at
`0x08000000`. The unit will boot to stock V1.0.066 firmware afterwards.
```

- [ ] **Step 5: Add gitkeep**

Create empty file:
```sh
touch /home/rar/device-configs/esphome/rippleon/OpenEVCharger/recovery/.gitkeep
```

- [ ] **Step 6: RUN THE BACKUP**

This is the single most important command in M0. Connect the SWD probe to the bench unit (probe powered, target powered via USB-5V). Then:

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
./tools/stock_backup.sh
```

Expected output ends with:
```
OK: /home/rar/device-configs/esphome/rippleon/OpenEVCharger/recovery/stock-mcu-V1.0.066.bin (262144 bytes)
<64-hex-char sha256>  /home/rar/device-configs/esphome/rippleon/OpenEVCharger/recovery/stock-mcu-V1.0.066.bin
```

If the dump fails (target not detected, wrong CPU ID), STOP. Do not continue M0 until backup succeeds. Diagnostic command:
```sh
openocd -f tools/openocd-gd32f205.cfg -c "init; targets; shutdown"
```
Expected output includes a Cortex-M3 entry with state `halted`.

- [ ] **Step 7: Verify backup matches a known-good reference**

If a previous SWD dump of the same firmware version exists in `~/device-configs/esphome/rippleon/` from prior reverse-engineering work, compare:
```sh
sha256sum recovery/stock-mcu-V1.0.066.bin
ls -la /home/rar/device-configs/esphome/rippleon/rippleon-mcu-firmware.bin 2>/dev/null && \
    sha256sum /home/rar/device-configs/esphome/rippleon/rippleon-mcu-firmware.bin
```
Expected: hashes match. (If a file at `rippleon-mcu-firmware.bin` already exists, that was the original RE dump — same physical chip, same firmware, hashes should agree.)

If hashes differ, investigate before proceeding — the backup may have captured a different state.

- [ ] **Step 8: Commit tooling (NOT the .bin)**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
git add tools/openocd-gd32f205.cfg tools/stock_backup.sh recovery/README.md recovery/.gitkeep
git commit -m "M0.2: stock-firmware SWD backup tooling + run initial backup"
```

Verify the .bin was excluded:
```sh
git status
```
Expected: `recovery/stock-mcu-V1.0.066.bin` listed under "Untracked files" (it's gitignored — that's intentional).

---

### Task 3: Fetch and vendor the GD32F20x firmware library

**Files:**
- Create: `OpenEVCharger/third_party/GD32F20x_Firmware_Library/README.md`
- Create: `OpenEVCharger/third_party/GD32F20x_Firmware_Library/Firmware/...` (vendored from upstream zip)

**Background.** GigaDevice ships the firmware library as a zip from gd32mcu.com. As of writing, V2.5.1 is current. The license bundled with the zip permits redistribution alongside firmware that targets a GD32 chip. We vendor the relevant subdirectories into `third_party/` (gitignored body, README explaining provenance).

- [ ] **Step 1: Download the vendor library zip**

```sh
mkdir -p /tmp/gd32fwlib
cd /tmp/gd32fwlib

# Primary: GigaDevice's mirror (URL changes occasionally — check gd32mcu.com if 404)
curl -fL -o GD32F20x_Firmware_Library_V2.5.1.zip \
  'https://www.gd32mcu.com/data/documents/yingyongbiji/GD32F20x_Firmware_Library_V2.5.1.zip' || \
echo "Primary fetch failed — see fallback below"
```

If primary fails: download manually from https://www.gd32mcu.com → "F2-Series" → "GD32F20x" → "Resources" → "Firmware Library V2.5.1". Save the zip to `/tmp/gd32fwlib/GD32F20x_Firmware_Library_V2.5.1.zip`.

Verify presence:
```sh
ls -la /tmp/gd32fwlib/GD32F20x_Firmware_Library_V2.5.1.zip
```
Expected: file ≥ 5 MB.

- [ ] **Step 2: Extract and vendor the needed subset**

```sh
cd /tmp/gd32fwlib
unzip -q GD32F20x_Firmware_Library_V2.5.1.zip
ls
```
Expected: directory `GD32F20x_Firmware_Library_V2.5.1/` containing `Firmware/` and `Examples/`.

Vendor only the `Firmware/` subtree (we don't need their Keil/IAR examples):
```sh
DEST=/home/rar/device-configs/esphome/rippleon/OpenEVCharger/third_party/GD32F20x_Firmware_Library
mkdir -p "$DEST"
cp -r GD32F20x_Firmware_Library_V2.5.1/Firmware "$DEST/"
ls "$DEST/Firmware"
```
Expected: subdirs `CMSIS/` and `GD32F20x_standard_peripheral/`.

- [ ] **Step 3: Verify the GCC startup file is present**

```sh
DEST=/home/rar/device-configs/esphome/rippleon/OpenEVCharger/third_party/GD32F20x_Firmware_Library
ls "$DEST/Firmware/CMSIS/GD/GD32F20x/Source/GCC/"
```
Expected: `startup_gd32f20x_cl.s` (cl = "connectivity line", which is the family GD32F205 belongs to).

If only ARM/IAR variants exist and no GCC dir, stop and adapt the ARM startup to GAS syntax — that's a separate sub-task we can do then. Most V2.5.x ships GCC.

- [ ] **Step 4: Write provenance README**

Create `OpenEVCharger/third_party/GD32F20x_Firmware_Library/README.md`:

```markdown
# GD32F20x Firmware Library

**Vendor:** GigaDevice Semiconductor Inc.
**Version:** V2.5.1
**Source:** https://www.gd32mcu.com/ → F2-Series → GD32F20x → Resources
**SHA-256 of source zip:** (record after first successful fetch)
**Date vendored:** 2026-05-02

## What's vendored

- `Firmware/CMSIS/` — ARM CMSIS Core + GigaDevice's GD32F20x device headers
- `Firmware/GD32F20x_standard_peripheral/` — GD's standard peripheral library
  (gpio, adc, timer, spi, usart, dma, exti, fmc, rcu, etc.)

The `Examples/` directory was discarded — we don't need vendor samples.

## License

Per the LICENSE.txt bundled in the upstream zip: free for use with GigaDevice
MCUs; redistribution permitted as part of derivative firmware. See
`Firmware/COPYRIGHT.HTML` (or equivalent) inside the vendored tree.

## Updating

To upgrade to a newer vendor release:
1. Download the new zip from gd32mcu.com.
2. Replace `Firmware/` with the new release's `Firmware/`.
3. Update `Version` and `Date vendored` in this README.
4. Build + flash + smoke-test M0 (LED blinks at 1 Hz).
```

Add the SHA-256 of the downloaded zip:
```sh
sha256sum /tmp/gd32fwlib/GD32F20x_Firmware_Library_V2.5.1.zip
```
Edit the README to replace `(record after first successful fetch)` with the actual hash.

- [ ] **Step 5: Decide on tracking strategy**

The vendor library is large (~10 MB unpacked). Two options:

**Option A — track in git.** Pros: one-step clone, reproducible builds. Cons: bloats the repo, makes upstream updates a noisy diff.

**Option B — gitignore + fetch script.** Pros: small repo. Cons: every clone needs the fetch.

We pick **Option A** — track in git. The library is small relative to a GitHub repo limit (50 MB soft cap), updates are infrequent (vendor releases once a year), and "clone-and-build" is a strong UX. Update `.gitignore` to NOT ignore the third_party tree:

Edit `OpenEVCharger/.gitignore`. Locate the line:
```
!recovery/.gitkeep
!third_party/**/.gitkeep
```

The current `.gitignore` doesn't actually ignore the vendor tree (we only blocked `recovery/*.bin`). Verify:
```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
git check-ignore -v third_party/GD32F20x_Firmware_Library/README.md
```
Expected output: empty (file is NOT ignored). If it IS ignored, fix `.gitignore`.

- [ ] **Step 6: Commit**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
git add third_party/
git commit -m "M0.3: vendor GD32F20x_Firmware_Library V2.5.1"
```

Expected commit size: ~10 MB (~1500 files). This is a one-time hit.

---

### Task 4: arm-none-eabi toolchain CMake config

**Files:**
- Create: `OpenEVCharger/cmake/arm-none-eabi-toolchain.cmake`

- [ ] **Step 1: Verify toolchain on host**

```sh
arm-none-eabi-gcc --version | head -1
arm-none-eabi-objcopy --version | head -1
cmake --version | head -1
```
Expected: gcc ≥ 10, cmake ≥ 3.20. If missing:
```sh
sudo apt install gcc-arm-none-eabi cmake
```

- [ ] **Step 2: Write the toolchain file**

`OpenEVCharger/cmake/arm-none-eabi-toolchain.cmake`:

```cmake
# arm-none-eabi cross-compile toolchain for GD32F205VC (Cortex-M3).

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Toolchain binaries
set(TOOLCHAIN_PREFIX arm-none-eabi-)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_OBJCOPY ${TOOLCHAIN_PREFIX}objcopy CACHE INTERNAL "objcopy")
set(CMAKE_OBJDUMP ${TOOLCHAIN_PREFIX}objdump CACHE INTERNAL "objdump")
set(CMAKE_SIZE ${TOOLCHAIN_PREFIX}size CACHE INTERNAL "size")

# Cortex-M3 target flags (no FPU)
set(CPU_FLAGS "-mcpu=cortex-m3 -mthumb -mfloat-abi=soft")

set(CMAKE_C_FLAGS_INIT
    "${CPU_FLAGS} -ffunction-sections -fdata-sections -fno-common -fmessage-length=0 \
    -Wall -Wextra -Wshadow -Wundef -Werror=implicit-function-declaration \
    -Werror=incompatible-pointer-types -Werror=return-type \
    -fstack-usage -specs=nano.specs -specs=nosys.specs")

set(CMAKE_CXX_FLAGS_INIT
    "${CMAKE_C_FLAGS_INIT} -fno-exceptions -fno-rtti -fno-use-cxa-atexit")

set(CMAKE_ASM_FLAGS_INIT "${CPU_FLAGS} -x assembler-with-cpp")

set(CMAKE_EXE_LINKER_FLAGS_INIT
    "${CPU_FLAGS} -Wl,--gc-sections -Wl,--print-memory-usage -nostartfiles \
    -specs=nano.specs -specs=nosys.specs")

# Optimisation per build type
set(CMAKE_C_FLAGS_DEBUG_INIT "-Og -g3 -DDEBUG")
set(CMAKE_C_FLAGS_RELWITHDEBINFO_INIT "-Os -g3")
set(CMAKE_C_FLAGS_RELEASE_INIT "-Os -g")
```

- [ ] **Step 3: Smoke-test the toolchain file with an empty CMake project**

Create a temp CMakeLists.txt to verify the toolchain loads:
```sh
cd /tmp
mkdir tc_smoke && cd tc_smoke
cat > CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(tc_smoke C)
add_library(empty STATIC empty.c)
EOF
echo "void noop(void) {}" > empty.c
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=/home/rar/device-configs/esphome/rippleon/OpenEVCharger/cmake/arm-none-eabi-toolchain.cmake
cmake --build build
```
Expected: builds `libempty.a` for cortex-m3. Last lines look like:
```
[100%] Built target empty
```

Clean up:
```sh
rm -rf /tmp/tc_smoke
```

- [ ] **Step 4: Commit**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
git add cmake/arm-none-eabi-toolchain.cmake
git commit -m "M0.4: arm-none-eabi cmake toolchain for cortex-m3"
```

---

### Task 5: Linker script for GD32F205VC

**Files:**
- Create: `OpenEVCharger/linker/gd32f205vc.ld`

GD32F205VC: 256 KB flash at `0x08000000`, 128 KB main RAM at `0x20000000`. Stack at top of RAM, heap minimal (FreeRTOS will own its own heap in M1; for M0 we just need ≥ 256 B for printf-style ops if used).

- [ ] **Step 1: Write the linker script**

`OpenEVCharger/linker/gd32f205vc.ld`:

```ld
/* GD32F205VC linker script
 * Flash: 256 KB @ 0x08000000
 * RAM:   128 KB @ 0x20000000
 *
 * Stack at top of RAM, growing down.
 * Heap from end of .bss upward (small for M0; FreeRTOS-managed in M1+).
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH  (rx) : ORIGIN = 0x08000000, LENGTH = 256K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

_estack       = ORIGIN(RAM) + LENGTH(RAM);
_min_heap     = 0x200;     /* 512 B minimum heap */
_min_stack    = 0x800;     /* 2 KB minimum stack */

SECTIONS
{
    .isr_vector : {
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    .text : {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)

        KEEP (*(.init))
        KEEP (*(.fini))

        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .rodata : {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
    .ARM       : { __exidx_start = .; *(.ARM.exidx*); __exidx_end = .; } > FLASH

    .preinit_array : {
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP (*(.preinit_array*))
        PROVIDE_HIDDEN (__preinit_array_end = .);
    } > FLASH

    .init_array : {
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP (*(SORT(.init_array.*)))
        KEEP (*(.init_array*))
        PROVIDE_HIDDEN (__init_array_end = .);
    } > FLASH

    .fini_array : {
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP (*(SORT(.fini_array.*)))
        KEEP (*(.fini_array*))
        PROVIDE_HIDDEN (__fini_array_end = .);
    } > FLASH

    /* RAM-resident initialised data; loaded from flash at startup */
    _sidata = LOADADDR(.data);
    .data : {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss : {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } > RAM

    /* Heap + stack room check */
    ._user_heap_stack : {
        . = ALIGN(8);
        PROVIDE ( end = . );
        PROVIDE ( _end = . );
        . = . + _min_heap;
        . = . + _min_stack;
        . = ALIGN(8);
    } > RAM

    /DISCARD/ : {
        libc.a ( * )
        libm.a ( * )
        libgcc.a ( * )
    }

    .ARM.attributes 0 : { *(.ARM.attributes) }
}
```

- [ ] **Step 2: Commit**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
git add linker/gd32f205vc.ld
git commit -m "M0.5: linker script for GD32F205VC (256K flash, 128K RAM)"
```

---

### Task 6: Minimal `src/main.c` — PD4 blink at 1 Hz

**Files:**
- Create: `OpenEVCharger/src/main.c`

Uses GD32 standard peripheral library calls — no register-bashing in M0 (we'll layer our own HAL on top in later milestones). PD4 is the heartbeat LED per the canonical pin map.

- [ ] **Step 1: Write `src/main.c`**

```c
/* OpenEVCharger M0 — heartbeat LED on PD4.
 *
 * Sets up the system clock at 120 MHz (handled by the vendor's SystemInit
 * called from startup_gd32f20x_cl.s) and toggles PD4 once per second using
 * SysTick as the time base.
 *
 * Once M1 introduces FreeRTOS, this file becomes the kernel-bringup
 * entry point and the blink moves into io_task.
 */

#include "gd32f20x.h"

#define HEARTBEAT_PORT  GPIOD
#define HEARTBEAT_PIN   GPIO_PIN_4
#define HEARTBEAT_RCU   RCU_GPIOD

static volatile uint32_t systick_ms;

void SysTick_Handler(void)
{
    systick_ms++;
}

static void systick_init(void)
{
    /* SystemCoreClock is set by SystemInit() in system_gd32f20x.c */
    SysTick_Config(SystemCoreClock / 1000U);   /* 1 ms tick */
}

static void delay_ms(uint32_t ms)
{
    uint32_t start = systick_ms;
    while ((systick_ms - start) < ms) {
        __WFI();
    }
}

static void heartbeat_init(void)
{
    rcu_periph_clock_enable(HEARTBEAT_RCU);
    gpio_init(HEARTBEAT_PORT,
              GPIO_MODE_OUT_PP,
              GPIO_OSPEED_2MHZ,
              HEARTBEAT_PIN);
    gpio_bit_reset(HEARTBEAT_PORT, HEARTBEAT_PIN);
}

int main(void)
{
    systick_init();
    heartbeat_init();

    for (;;) {
        gpio_bit_set(HEARTBEAT_PORT, HEARTBEAT_PIN);
        delay_ms(500);
        gpio_bit_reset(HEARTBEAT_PORT, HEARTBEAT_PIN);
        delay_ms(500);
    }
}
```

- [ ] **Step 2: Commit**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
git add src/main.c
git commit -m "M0.6: minimal main.c — 1 Hz heartbeat blink on PD4"
```

---

### Task 7: Top-level `CMakeLists.txt`

**Files:**
- Create: `OpenEVCharger/CMakeLists.txt`

This wires the vendor library, our linker script, our `main.c`, and the toolchain file into a single `openevcharger.elf` target.

- [ ] **Step 1: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.20)

# The toolchain file MUST be set on the cmake command line via
# -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
project(openevcharger C ASM)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(VENDOR_LIB ${CMAKE_SOURCE_DIR}/third_party/GD32F20x_Firmware_Library/Firmware)
set(CMSIS_DIR  ${VENDOR_LIB}/CMSIS)
set(GD_DIR     ${VENDOR_LIB}/CMSIS/GD/GD32F20x)
set(STDP_DIR   ${VENDOR_LIB}/GD32F20x_standard_peripheral)

# ---------- Vendor sources we always want ----------

set(GD_STARTUP_SRC ${GD_DIR}/Source/GCC/startup_gd32f20x_cl.s)
set(GD_SYSTEM_SRC  ${GD_DIR}/Source/system_gd32f20x.c)

# Standard peripheral library — only the modules we use in M0 (GPIO, RCU, MISC).
# Add more here as later milestones consume them.
set(STDP_SRCS
    ${STDP_DIR}/Source/gd32f20x_gpio.c
    ${STDP_DIR}/Source/gd32f20x_rcu.c
    ${STDP_DIR}/Source/gd32f20x_misc.c
)

# ---------- Application sources ----------

set(APP_SRCS
    src/main.c
)

# ---------- Target ----------

set(TARGET ${PROJECT_NAME})

add_executable(${TARGET}
    ${APP_SRCS}
    ${GD_STARTUP_SRC}
    ${GD_SYSTEM_SRC}
    ${STDP_SRCS}
)

target_include_directories(${TARGET} PRIVATE
    src
    ${CMSIS_DIR}/Include
    ${GD_DIR}/Include
    ${STDP_DIR}/Include
)

# Vendor expects this define to select the chip family
target_compile_definitions(${TARGET} PRIVATE
    GD32F20X_CL=1                # connectivity-line family (F205/F207)
    HXTAL_VALUE=8000000          # 8 MHz HSE crystal on the Rippleon board
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

# Generate .bin and .hex alongside .elf
add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${TARGET}> ${TARGET}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${TARGET}> ${TARGET}.hex
    COMMAND ${CMAKE_SIZE}   $<TARGET_FILE:${TARGET}>
    BYPRODUCTS ${TARGET}.bin ${TARGET}.hex
    COMMENT "Generating ${TARGET}.bin/.hex and printing size"
)
```

- [ ] **Step 2: Configure the build**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
```
Expected: ends with `-- Generating done` and `-- Build files have been written to: .../build`. No errors.

If you see "no such file or directory" pointing at `startup_gd32f20x_cl.s`, the vendor lib path/Layout differs — locate the actual file:
```sh
find third_party/GD32F20x_Firmware_Library -name 'startup_gd32f20x*'
```
and update `GD_STARTUP_SRC` in `CMakeLists.txt` accordingly.

- [ ] **Step 3: Build**

```sh
cmake --build build
```
Expected: ends with a `arm-none-eabi-size` line showing something like:
```
   text    data     bss     dec     hex filename
   2548      12     288    2848     b20 .../openevcharger.elf
Memory region         Used Size  Region Size  %age Used
           FLASH:        2560 B       256 KB      0.98%
             RAM:         300 B       128 KB      0.23%
```

(Exact byte counts vary with toolchain version. The key indicator is "no link errors, percentages plausible".)

If link errors mention undefined `_close`, `_read`, `_write` etc., they're satisfied by `nosys.specs` already in the toolchain file — recheck `CMakeLists.txt` linker flags.

- [ ] **Step 4: Verify outputs**

```sh
ls -la build/openevcharger.elf build/openevcharger.bin build/openevcharger.hex build/openevcharger.map
arm-none-eabi-size build/openevcharger.elf
```
Expected: all four files exist. ELF is non-empty.

- [ ] **Step 5: Commit**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
git add CMakeLists.txt
git commit -m "M0.7: top-level CMake build — produces openevcharger.elf/.bin/.hex"
```

---

### Task 8: Flashing scripts

**Files:**
- Create: `OpenEVCharger/tools/flash.sh`
- Create: `OpenEVCharger/tools/flash_stock.sh`

- [ ] **Step 1: Write `tools/flash.sh`**

```sh
#!/usr/bin/env bash
# Build (if needed) and flash openevcharger.elf to the connected GD32F205 via SWD.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CFG="$REPO_ROOT/tools/openocd-gd32f205.cfg"
ELF="$REPO_ROOT/build/openevcharger.elf"

# Refuse to flash without a stock backup on disk
if [[ ! -e "$REPO_ROOT/recovery/stock-mcu-V1.0.066.bin" ]]; then
    echo "ERROR: no stock backup at recovery/stock-mcu-V1.0.066.bin"
    echo "Run ./tools/stock_backup.sh first. Aborting."
    exit 1
fi

# Build if .elf is missing or older than any source
if [[ ! -e "$ELF" ]] || \
   find "$REPO_ROOT/src" -newer "$ELF" -type f | grep -q .; then
    echo "Build needed; running cmake --build build"
    cmake --build "$REPO_ROOT/build"
fi

echo "Flashing $ELF ..."
openocd -f "$CFG" \
        -c "init" \
        -c "reset halt" \
        -c "program $ELF verify reset" \
        -c "shutdown"

echo "Done. Heartbeat LED on PD4 should be blinking at 1 Hz."
```

- [ ] **Step 2: Write `tools/flash_stock.sh`**

```sh
#!/usr/bin/env bash
# Restore the stock GD32F205 firmware from recovery/stock-mcu-V1.0.066.bin.
# Use this if a new flash bricks the unit or you want to revert to factory.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CFG="$REPO_ROOT/tools/openocd-gd32f205.cfg"
BIN="$REPO_ROOT/recovery/stock-mcu-V1.0.066.bin"

if [[ ! -e "$BIN" ]]; then
    echo "ERROR: $BIN not found. Cannot restore stock."
    exit 1
fi

SIZE=$(stat -c '%s' "$BIN")
if [[ "$SIZE" -ne 262144 ]]; then
    echo "ERROR: $BIN is $SIZE bytes (expected 262144). Refusing."
    exit 1
fi

echo "Restoring stock firmware from $BIN ..."
openocd -f "$CFG" \
        -c "init" \
        -c "reset halt" \
        -c "flash write_image erase $BIN 0x08000000 bin" \
        -c "verify_image $BIN 0x08000000 bin" \
        -c "reset run" \
        -c "shutdown"

echo "Stock V1.0.066 restored."
```

- [ ] **Step 3: Make executable**

```sh
chmod +x /home/rar/device-configs/esphome/rippleon/OpenEVCharger/tools/flash.sh \
         /home/rar/device-configs/esphome/rippleon/OpenEVCharger/tools/flash_stock.sh
```

- [ ] **Step 4: Commit**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
git add tools/flash.sh tools/flash_stock.sh
git commit -m "M0.8: flash.sh + flash_stock.sh wrapper scripts"
```

---

### Task 9: Flash the bench unit and verify the blink

**This is the hardware validation gate for M0.** All previous tasks are scaffolding; this is where we find out if it works.

- [ ] **Step 1: Confirm SWD connection**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
openocd -f tools/openocd-gd32f205.cfg -c "init; targets; shutdown"
```
Expected: `targets` prints a Cortex-M3 line with state `halted` or `running`. If "Error: open failed", check the probe USB connection / udev rules.

- [ ] **Step 2: Flash**

```sh
./tools/flash.sh
```
Expected output ends with:
```
** Programming Started **
** Programming Finished **
** Verify Started **
** Verified OK **
** Resetting Target **
Done. Heartbeat LED on PD4 should be blinking at 1 Hz.
```

- [ ] **Step 3: Visual + multimeter verification**

Look at PD4 (or the LED tied to PD4 on the board, per pinout.md). It should blink at exactly 1 Hz (500 ms on, 500 ms off).

If you have a multimeter on the pad in DC volts mode you should see it oscillate between near-0 V and near-3.3 V at 1 Hz.

If you have a logic analyzer or scope, capture a few seconds of PD4 and confirm the period is 1.000 s ± 0.5%. Tolerance comes from the HSE crystal accuracy + MCU clock-tree. If you see wildly different period (say 1.2 s or 0.8 s), the clock tree is wrong — check `HXTAL_VALUE` in `CMakeLists.txt` matches the actual crystal on the board.

- [ ] **Step 4: Post-validation diagnostics**

After confirming blink, attach openocd + telnet for live state:
```sh
openocd -f tools/openocd-gd32f205.cfg &
sleep 1
telnet localhost 4444 <<'EOF'
mdw 0x40021000 4
shutdown
EOF
```
That dumps four words at the RCU base. Useful as a sanity-baseline; record the values in `docs/bring-up.md` for future comparison if M1+ regresses.

Alternative diagnostic if the blink fails:
```sh
# Halt and inspect PC
openocd -f tools/openocd-gd32f205.cfg -c "init; halt; reg pc; reg lr; shutdown"
```
- If `pc` is in `0x080000xx`, the chip is running our code.
- If `pc` is in `0x1FFFxxxx`, it's stuck in vendor bootloader (BOOT0 strap wrong — but we never touched BOOT0).
- If `pc` is `0x00000000`, the vector table didn't load correctly — re-check linker script `_estack` and the `.isr_vector` placement.

- [ ] **Step 5: If blink works — DOCUMENT**

Edit `docs/bring-up.md` (created in Task 10):
This step documents the M0 milestone result. Defer to Task 10 for content.

- [ ] **Step 6: If blink fails — DEBUG, do not proceed**

Common failure modes and fixes:
1. **No blink, chip running.** Wrong PD4 mapping. Verify with multimeter on the actual pin (LQFP100 pin 81) and on the silkscreen LED. If pin 81 toggles but the LED doesn't, the LED is wired to a different pin than we think — recheck pinout.md.
2. **No blink, chip in fault.** `pc` stuck in vendor ROM or hard-fault handler. Check linker script and startup file are linked in.
3. **Blink at wrong rate.** HSE_VALUE wrong, or PLL config wrong. Verify the crystal frequency (should be 8 MHz — visible on the board near the MCU). If different, update `HXTAL_VALUE` in CMakeLists.txt.

If stuck after 30 minutes of debugging, restore stock with `./tools/flash_stock.sh` and review the failure mode before reflashing OpenEVCharger.

---

### Task 10: Document M0 milestone in `docs/bring-up.md`

**Files:**
- Create: `OpenEVCharger/docs/bring-up.md`

- [ ] **Step 1: Write the bring-up log**

```markdown
# OpenEVCharger bring-up log

Per-milestone hardware validation notes. Every milestone gets an entry with
date, success criterion, observed result, and any deviations from spec.

## M0 — Toolchain Bootstrap

**Date completed:** YYYY-MM-DD (fill in when done)
**Spec section:** § 9 M0
**Plan:** docs/superpowers/plans/2026-05-02-m0-toolchain-bootstrap.md

### Success criterion (from spec)
PD4 heartbeat LED blinks at 1 Hz with the GD32F205V running at 120 MHz, after
SWD-flashing `openevcharger.elf` produced by CMake + arm-none-eabi-gcc + GD32F20x
vendor library.

### Observed result
- [ ] LED blink visible at 1 Hz: YES / NO
- [ ] Multimeter on PD4 reads 0–3.3 V swing: YES / NO
- [ ] Period measured by scope/LA: ___ ms (target: 1000 ms ± 5 ms)
- [ ] OpenOCD connect/halt/program/verify all succeed: YES / NO
- [ ] Build size: text=___, data=___, bss=___ (record from arm-none-eabi-size)

### Stock backup
- [ ] `recovery/stock-mcu-V1.0.066.bin` exists, 262 144 bytes
- SHA-256: ____________
- Optional: matches `~/device-configs/esphome/rippleon/rippleon-mcu-firmware.bin`: YES / NO

### Deviations from spec or plan
(record any: HSE value, vendor lib version, openocd config tweaks)

### Next milestone
M1: FreeRTOS + idle/safety tasks. Plan to be written after this milestone validates.
```

- [ ] **Step 2: Fill in the actual measurements**

After Task 9 step 5 confirmed blink, edit this file and replace `YYYY-MM-DD` with today's date and populate the checkboxes with actual results.

```sh
sha256sum /home/rar/device-configs/esphome/rippleon/OpenEVCharger/recovery/stock-mcu-V1.0.066.bin
arm-none-eabi-size /home/rar/device-configs/esphome/rippleon/OpenEVCharger/build/openevcharger.elf
```

Record those numbers in the log.

- [ ] **Step 3: Commit**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
git add docs/bring-up.md
git commit -m "M0.9: bring-up log — M0 validated on bench"
```

---

### Task 11: Tag M0 and push

- [ ] **Step 1: Tag the milestone commit**

```sh
cd /home/rar/device-configs/esphome/rippleon/OpenEVCharger
git tag -a m0-toolchain-bootstrap -m "M0 complete: toolchain + 1 Hz PD4 blink validated on bench"
```

- [ ] **Step 2: (Optional) push to origin**

User confirms before push. The repo has a remote at `git@github.com:RAR/OpenEVCharger.git`. To push:
```sh
git push origin main
git push origin m0-toolchain-bootstrap
```

If the repo doesn't exist on GitHub yet, the push will fail with a useful error. Create the empty repo on GitHub first (https://github.com/new) then retry. Do NOT push without user confirmation.

---

## After M0

Once Task 9 has validated the blink and Task 10 has the measurements logged:

1. **Update `MEMORY.md`** in `~/.claude/projects/-home-rar-device-configs/memory/` with a new project memory entry summarizing what we learned during M0 (HSE value, vendor lib version, openocd quirks, any hardware surprises).

2. **Write the M1 plan.** M1 = FreeRTOS + idle task + safety_task scaffold + IWDG armed. The plan structure mirrors M0 but adds:
   - FreeRTOS submodule (third_party/FreeRTOS-Kernel)
   - FreeRTOS port for Cortex-M3 (`portable/GCC/ARM_CM3`)
   - `FreeRTOSConfig.h` tuned for our heap+stack sizes
   - `safety_task.c` with the 50 Hz tick stub (no logic yet, just structure)
   - `io_task.c` taking over the heartbeat blink from `main.c`
   - IWDG init + kick from safety_task
   - `vTaskList` sanity output via UART (need M2 UART scaffolding here? Or add a tiny semihosting log just for M1? — decide when writing M1 plan).

3. **Do NOT start M2/M3/etc. without M1 validating.** Each milestone gates the next. If M1 reveals (say) that FreeRTOS port has a Cortex-M3 quirk we didn't anticipate, M0 may need touch-up, but later milestones cannot proceed.

---

## Self-review

**Spec coverage check (M0 only — § 9 of the spec, M0 row).**

| Spec requirement (M0) | Plan task |
|---|---|
| CMake + arm-none-eabi + vendor lib + linker + startup | Tasks 3, 4, 5, 7 |
| SWD flash via openocd works | Task 8 (flash.sh), Task 9 (verified) |
| Vendor systick + clock tree at 120 MHz | Task 6 (main.c uses SystemCoreClock from vendor SystemInit) |
| **SUCCESS = LED blinks** | Task 9 step 3 |
| Stock backup before flash (spec § 1, recovery contract) | Task 2 |

All M0 spec requirements have corresponding tasks. Tasks 1, 10, 11 are infrastructure (skeleton, logging, tagging) — necessary plan hygiene, not new spec requirements.

**Placeholder scan.** No "TODO", "TBD", or "implement later" tokens. All file contents written out in full. All commands have expected outputs.

**Type/name consistency.** `HEARTBEAT_PORT/PIN/RCU` macro names consistent in `main.c`. `TARGET = openevcharger` consistent across CMakeLists.txt and tools/flash.sh (both reference `openevcharger.elf`). `recovery/stock-mcu-V1.0.066.bin` filename consistent across stock_backup.sh, flash_stock.sh, and recovery/README.md. `HXTAL_VALUE=8000000` matches the vendor's expected `HXTAL_VALUE` macro (defined in `system_gd32f20x.c`).

**Gaps.** None for M0. M1+ deferred to subsequent plans by design.
