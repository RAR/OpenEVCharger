# M12 — stripped DcoFImage with delta-bridge personalities

**Date:** 2026-05-16 (autonomous overnight build) — updated 2026-05-19 with bench-validation results.
**Status:** Pipeline validated end-to-end on the bench via `DcoFImage-stock-restore` (2026-05-19); M12 flash itself pending operator. See §"Bench-validation results" below + docs/24 for the recovery procedure used after the first attempt bricked the bench (eraseblock-size bug, fixed).

## Goal

Ship a single USB-flashable rootfs image that:

1. **Owns the leaf I/O daemons** we've decoded and replaced with safer/auditable C
   (RFID, MeterIC_new, Adc, LED_control → delta-bridge personalities).
2. **Keeps stock** for everything safety-critical or under-decoded
   (`main`, `Pri_Comm`, `Charging_Standard_RFID`, `FlashLog`, `RTC`, `ErrorHandle`).
3. **Trims dead weight** (DeltaOCPP, factory build tools, unused variants) — frees ~800 KB.
4. **Boots into a working web UI on first run** so the operator can configure the
   bridge without serial console access.

## Rootfs contents — keep / replace / remove

Source: `/home/rar/device-configs/esphome/testcharger/delta/dump/rootfs-unpacked/`
(extract of `mtd5-rootfs.bin` from the bench unit, JFFS2, 16 MiB).

Per-binary verdict in `/root`:

| Binary | Size | Verdict | Reason |
|---|---:|---|---|
| `main` | 87 KB | **keep stock** | Orchestrator; forks all the others |
| `Pri_Comm` | 35 KB | **keep stock** | Safety supervisor → STM32 (docs/18) |
| `Charging_Standard_RFID` | 47 KB | **keep stock** | EVSE state machine (docs/20) |
| `FlashLog` | 11 KB | **keep stock** | Energy/PassTime persistence (docs/19) |
| `RTC` | 13 KB | **keep stock** | I2C 0x51 clock (docs/20) |
| `ErrorHandle` | 22 KB | **keep stock** | SNMP error reporter |
| `snmpd` | 46 KB | keep stock | SNMP daemon |
| `snmptrap` | 24 KB | keep stock | SNMP trap sender |
| `mini_httpd` | 47 KB | keep stock | Stock CGI web — leave alone in case stock paths still bind |
| `wpa_supplicant`(+.conf) | 2.3 MB | keep stock | Wi-Fi |
| `iwconfig`, `iwlist` | ~32 KB | keep stock | Wi-Fi diagnostics |
| `htpasswd`, `simple.script`, `ppp` | — | keep stock | CGI/network helpers |
| `www/` | — | keep stock | Stock CGI files |
| `RFID` | 17 KB | **wrapper** | docs/12, RFID → delta-bridge (-c default) |
| `Adc` | 24 KB | **wrapper** | docs/17, `--personality=adc` |
| `MeterIC_new` | 34 KB | **wrapper** | docs/16, `--personality=meter` |
| `LED_control` | 13 KB | **wrapper** | docs/19+21, `--personality=led` |
| `DeltaOCPP` | **734 KB** | **REMOVE** | Replaced by delta-bridge MQTT/evcc path (docs/20) |
| `ACFWMaker`, `FWMaker` | 20 KB | REMOVE | Build-time bundlers, not runtime |
| `ScenarioMaker` | 14 KB | REMOVE | Factory test |
| `Pri_Comm_cqc` | 23 KB | REMOVE | CQC variant, not referenced |
| `NTC_tmp` | 33 KB | REMOVE | Standalone NTC test tool, no references |
| `PowerCard-UltraLight` | — | REMOVE | No references, factory tool |
| `Charging_Standard` (non-RFID) | 40 KB | REMOVE | `main` only forks the RFID variant |
| `Energy`, `PassTime`, `LogCount` | <1 KB | keep | Persistence files (FlashLog recreates if missing, but keeping seed values is harmless) |

**Stock backup of each replaced binary** lands in `/Storage/stk/` at first
boot via a one-shot script — rollback is `cp /Storage/stk/<name> /root/<name>; sync; reboot`.

## /root layout in the image

```
/root/
├── delta-bridge              ← M12 binary (one binary, multiple personalities)
├── RFID                      ← wrapper: exec delta-bridge -c /Storage/delta-bridge.conf
├── Adc                       ← wrapper: exec delta-bridge --personality=adc
├── MeterIC_new               ← wrapper: exec delta-bridge --personality=meter
├── LED_control               ← wrapper: exec delta-bridge --personality=led
├── main                      ← stock, unchanged
├── Pri_Comm                  ← stock, unchanged
├── Charging_Standard_RFID    ← stock, unchanged
├── FlashLog, RTC, ErrorHandle, snmpd, snmptrap, …  ← stock, unchanged
└── (DeltaOCPP, ACFWMaker, FWMaker, ScenarioMaker, Pri_Comm_cqc,
     NTC_tmp, PowerCard-UltraLight, Charging_Standard, RFID-stock copy
     all REMOVED — see /etc/delta-bridge/first-boot.sh for stock backups)
```

## /etc and /Storage layout

```
/etc/
├── funs                      ← stock unchanged (still starts main/RTC/Adc/MeterIC_new/snmpd/ErrorHandle)
├── inittab                   ← stock unchanged
├── delta-bridge.conf.default ← shipped default (web_enable=true, web auth admin/changeme)
└── delta-bridge/
    ├── first-boot.sh         ← runs once via /etc/funs: ensures /Storage/delta-bridge.conf
    │                            exists (copies from default) and /Storage/stk/ backups exist
    └── stk-manifest.txt      ← list of binaries our wrappers replaced (used by first-boot.sh
                                 to back up each replaced binary if /Storage/stk/<name> is missing)
```

```
/Storage/                     ← /dev/mtdblock3 (persistent across rootfs reflash)
├── delta-bridge.conf         ← created by first-boot.sh from default; web POST persists here
├── delta-bridge/
│   └── bridge-boot.log       ← stderr from main bridge (via wrapper redirect)
└── stk/                      ← stock binaries backed up by first-boot.sh
    ├── RFID
    ├── Adc
    ├── MeterIC_new
    └── LED_control
```

The first-boot script is idempotent — runs on every boot, no-ops on
the "seed default conf" and "pre-stage stk binaries" paths once their
targets exist. The USB-config-import path (docs/23 §5) runs whenever
`/UsbFlash/delta-bridge.conf` is present *and* byte-different from the
on-disk copy, so a stick left in won't re-apply; a stick with an edited
file will.

## Wrapper template

```sh
#!/bin/ash
# M12 wrapper — exec the delta-bridge personality. Rollback:
#   cp /Storage/stk/<name> /root/<name>; sync; reboot
exec /root/delta-bridge --personality=<role>
```

The main-bridge wrapper (`/root/RFID`) is special — passes `-c`:

```sh
#!/bin/ash
# M12 main bridge — MQTT + web + RFID reader (docs/12, docs/14, docs/21).
exec /root/delta-bridge -c /Storage/delta-bridge.conf \
     >> /Storage/delta-bridge/bridge-boot.log 2>&1
```

Matches the bench's runtime layout that the new `log_path` default in
PR #25 picks up — `/api/log` "just works".

## DcoFImage bundle format

Unchanged from docs/03:

```
<jffs2-rootfs-payload> DELTADCOF <BE-uint32 byte-sum of payload>
```

Wrapping handled by `companion/image/wrap_dco.py` (already host-tested).

## Build invocation

```sh
# 1. Cross-compile delta-bridge with the build identifiers exposed at /api/build
cd boards/eluminocity-ch21130/companion
SHA=$(git rev-parse --short HEAD)
DATE=$(date -u +%Y-%m-%dT%H:%MZ)
docker run --rm -v "$(pwd)":/work -w /work -u "$(id -u):$(id -g)" \
    muslcc/x86_64:armv5l-linux-musleabi cc -Wall -Wextra -std=c11 -O2 \
    -Isrc -static \
    -DDELTA_BRIDGE_VERSION='"m12"' \
    -DDELTA_BRIDGE_BUILD_SHA="\"$SHA\"" \
    -DDELTA_BRIDGE_BUILD_DATE="\"$DATE\"" \
    -o delta-bridge.m12 \
    src/shmem.c src/charger_state.c src/mqtt_codec.c src/mqtt_client.c \
    src/mqtt_adapter.c src/commands.c src/config.c src/web.c src/rfid.c \
    src/meter.c src/adc.c src/led.c src/main.c

# 2. Build the image (uses the existing extracted stock rootfs)
make dcofimage
# or, explicit:
#   image/build-dcofimage.sh <stock-rootfs-dir> delta-bridge.m12 <out-dir>

# 3. Verify (also runs automatically as the second half of `make dcofimage`)
image/verify_dcofimage.py --expected-sha256 \
    "$(sha256sum delta-bridge.m12 | cut -d' ' -f1)" \
    <out-dir>/DcoFImage
```

## Builder

`companion/image/build-dcofimage.sh` (rewrite of the previous version
which assumed delta-bridge was a side-process running alongside stock).

Inputs:
- Stock rootfs directory (default: extracted from `dump/rootfs-unpacked/`)
- delta-bridge binary (cross-compiled armv5 static-pie)
- Output directory

Output:
- `DcoFImage` — our trimmed + replaced image
- `DcoFImage-stock-restore` — untouched stock for rollback

Steps:
1. Stage stock rootfs into a temp dir
2. Apply the M12 changes:
   - Install `delta-bridge` at `/root/delta-bridge`
   - Write the four wrapper scripts
   - Write `/etc/delta-bridge.conf.default`
   - Write `/etc/delta-bridge/first-boot.sh` and `stk-manifest.txt`
   - Patch `/etc/funs` to run first-boot.sh before the daemons
   - Remove the binaries in the REMOVE list
3. `mkfs.jffs2` (same flags as the old builder)
4. `wrap_dco.py` to add DELTADCOF magic + byte-sum
5. Same flow for the stock-restore variant

## Install on the bench

**Recommended: flash the stock-restore image FIRST to prove the
end-to-end pipeline before risking it with the M12 changes.** The
stock-restore image extracts identically to the bench's existing
rootfs (verified offline) — if it doesn't boot, the issue is in our
mkfs.jffs2 invocation or the DELTADCOF wrapping, NOT in our M12
modifications.

```sh
# Pipeline validation (do this first):
# 1. cp build/m12/DcoFImage-stock-restore /UsbFlash/DcoFImage
# 2. Insert USB stick; reboot.
# 3. /root/main polls /UsbFlash/ at boot, finds DcoFImage,
#    UpdateCSU() verifies DELTADCOF + byte-sum, dd's the payload
#    to /dev/mtdblock5, reboots. Bench should come back identically
#    to its current state. If it doesn't — STOP, debug the builder
#    before trying the M12 image.

# M12 install (after pipeline validation):
# 1. cp build/m12/DcoFImage /UsbFlash/DcoFImage
#    (optionally: also drop /UsbFlash/delta-bridge.conf with your broker
#     creds + web_pass — first-boot.sh imports it; see docs/23 §5)
# 2. Insert USB stick; reboot.
# 3. New rootfs boots; /etc/funs runs first-boot.sh which:
#      - mkdir /Storage/delta-bridge (for the bridge log)
#      - mkdir /Storage/stk          (for stock-binary backups)
#      - cp default conf -> /Storage/delta-bridge.conf if missing
#      - if /UsbFlash/delta-bridge.conf present + byte-different:
#          atomically copy it over /Storage/delta-bridge.conf
#      - if /UsbFlash/stk/<name> present for any wrapper-replaced binary:
#          copy into /Storage/stk/ for hot rollback
# 4. /root/main forks the daemons; the four wrappers exec delta-bridge
#    with their respective personalities.
# 5. Web UI at http://<bench-ip>:8080/ — admin / changeme (or whatever
#    you set in /UsbFlash/delta-bridge.conf). Change password via Config
#    tab on first login if you haven't.
```

Rollback (anytime):

```sh
# Hot rollback of individual binaries (no reboot for most):
cp /Storage/stk/<name> /root/<name>; sync; reboot

# Full revert: flash DcoFImage-stock-restore the same way.
```

## Known risks / things to watch for on first M12 boot

- **`/root/main` forks `/root/DeltaOCPP`** (per `strings`: `killall DeltaOCPP; /root/DeltaOCPP &`). After M12 install, that fork's child-side `execve` fails with ENOENT — main itself doesn't crash (the fork already returned), but expect a stderr line per call. If main re-forks on a loop, install a no-op shim at `/root/DeltaOCPP` instead of removing it:
  ```sh
  #!/bin/ash
  # no-op shim — keeps main's fork-and-forget happy
  while :; do sleep 3600; done
  ```
  Out of caution we may want to ship this shim from the start; deferred until first boot tells us whether log-spam is a real problem.

- **First-boot `/Storage/stk/` backups are empty by default.** Operator-supplied stock binaries at `/UsbFlash/stk/<name>` get copied in by `first-boot.sh` (mounts USB itself before `main` starts). Without those, individual-binary rollback (`cp /Storage/stk/X /root/X`) won't work — the supported rollback is `cp DcoFImage-stock-restore /UsbFlash/DcoFImage; reboot`.

- **`/etc/funs` patches** are line-based — if a future stock-rootfs extract has different line endings or a CRLF in line 1, the `head -n 1; echo; tail -n +2` injection point shifts. We re-extract from the bench's actual mtdblock5, so this is currently fine; flag it if we ever pull rootfs from a different source.

## What this M12 doesn't ship (deferred)

- Kernel/uImage update — out of scope; we only touch the rootfs (mtdblock5).
- HMI firmware update — separate code path (`ACmini_Primary.bin` to `Pri_Comm`).
- Dropbear / SSH — the old builder had it as an option; defer to a follow-up so
  the M12 baseline is minimal. Add via `--with-dropbear` flag when needed.
- Per-device unique web_pass — first-boot ships `admin/changeme` and the operator
  changes via UI. Later: provision a unique pass from the unit's SerialNumber.

## Bench-validation results (2026-05-19)

End-to-end pipeline validation by flashing `DcoFImage-stock-restore` (the untouched-stock variant — proves the build pipeline without changing what's deployed).

**First attempt: brick.** Built with `--eraseblock=0x20000` (128 KiB). Stock's flasher in `/root/main` honored the JFFS2 image's declared erase block but the hardware sectors on this device are 64 KiB. Result: every-other 64 KiB sector in the upper half of mtd5 was erased, the alternates kept old data. JFFS2 mounted (kernel doesn't care about declared eraseblock during mount, just scans nodes) but inode versions came from two different filesystem images. Kernel panicked at `/sbin/init: symbol lookup error: /lib/libc.so.6: undefined symbol: loc1` — classic ABI mismatch from init and libc loaded from two different versions of the same rootfs.

U-Boot `flinfo` after the brick was the smoking gun — alternating `E` (erased) and non-E sectors from offset 0x99B0000 onward in Bank 2:

```
F99B0000 E    F99C0000      F99D0000 E    F99E0000      F99F0000 E
```

**Recovery: U-Boot serial console + USB load + raw `cp.b` to flash.** Full procedure in docs/24. Took ~30 min including stick-reformat (stock-time flash needs partition-less FAT; U-Boot's `fatload` needs a real MBR — they can't be the same stick).

**Second attempt: clean.** Rebuilt with `--eraseblock=0x10000` (64 KiB) to match `/proc/mtd`. Stock-restore image flashed cleanly via U-Boot recovery path, bench booted through `/sbin/init` to the full stock daemon stack (`main`, `Pri_Comm`, `Charging_Standard_RFID`, `Adc`, `MeterIC_new`, `LED_control`, `RTC`, `ErrorHandle`, `snmpd`, `wpa_supplicant`, etc. — no delta-bridge processes, exactly as stock would).

**What this validates:**
- ✓ DELTADCOF wrap + checksum format is correct
- ✓ `mkfs.jffs2 --eraseblock=0x10000 --pad=0x1000000 --little-endian --squash-uids` produces a bootable image on this device
- ✓ Stock's USB-flash auto-detect path works (`/UsbFlash/DcoFImage` → "Update CSU File system by USB" log → erase+write+reboot)
- ✓ The 16 MiB image fits and boots cleanly from mtd5

**What it does NOT validate** (still pending operator):
- The M12 image itself (replacements + trims) — only stock-restore tested so far. Both images use the same builder so the eraseblock fix carries over, but you'll want to verify M12 actually boots with the wrappers in place.

## USB-stick prep — two different layouts needed

This bench has a subtle compatibility split:

| Scenario | USB format | Why |
|---|---|---|
| Stock-time DcoFImage flash (via `/root/main`) | **Partition-less** whole-device FAT (`mkfs.vfat -I /dev/sdX`) | Stock's flasher only does `mount /dev/sda /UsbFlash`, no `sda1` fallback when `sda` exists |
| U-Boot recovery (via `fatload usb 0:1`) | **Partitioned** FAT32 (MBR + one primary partition) | U-Boot's `fatload usb 0` is hardcoded to look at partition 1; refuses partition-less sticks |

Plan ahead: have **two USB sticks** ready (or one stick that you reformat between phases). The recovery procedure in docs/24 covers both formats.

## Verification checklist (operator runs after USB-flash)

- [ ] Web UI loads at http://<bench-ip>:8080/, default creds work
- [ ] Status tab shows live V/I/P, pilot state, faults
- [ ] Config tab shows all 16 keys, current values populated
- [ ] /api/log shows real content from `/Storage/delta-bridge/bridge-boot.log`
- [ ] /api/gain reads `/Storage/Gain` correctly (Vgain/Igain/Wgain present)
- [ ] /api/build reports version/sha/build_date
- [ ] LEDs light correctly (middle green idle, etc. per docs/19+21)
- [ ] Free space saved vs stock ≥ 700 KB
