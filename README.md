# OpenEVCharger

Replacement firmware for vendor-cloud-tethered AC EV chargers (J1772),
giving them fully local control — Home Assistant via the ESPHome
native API, OCPP 1.6-J to any Central System (SteVe, evcc, CitrineOS,
Monta), and no internet or vendor-account dependency.

The safety-critical work runs on the charger's main MCU as a small
FreeRTOS-based C core. The Wi-Fi/BLE companion module hosts ESPHome +
MicroOcpp and talks to the safety MCU over a binary TLV link. Both
sides ship as a complete replacement for the stock firmware on the
user's own hardware.

**Supported hardware** lives in [`BOARDS.md`](BOARDS.md). Today:

- ✅ **Rippleon ROC family** (Beizide / NewEnergyCS ODM, GD32F205VG +
  Quectel FC41D) — production-validated bench + 240 V real-EV charging
  session 2026-05-07.
- 🚧 **Nexcyber** (Zopoise `ZBU011K-C00X` PCBA, Nations N32G45x +
  Tuya WBR2 / RTL8720CF) — port in progress.

The board-specific surface is small: a `boards/<board>/` directory
holding a pin map, a linker script, and a `board.cmake` that wires in
the vendor SDK. The per-chip HAL lives in `src/hal/<chip>/`. The safety
core (J1772 state machine, fault detectors, self-test, OCPP / TLV
protocol, OTA, RTC bridge, persistence) is board-independent.
The Nexcyber port currently ships a bench-harness target
(`openevcharger-nexcyber-bringup`) plus a compile-gated production target.

**Latest release:** [`2026.19.0`](https://github.com/RAR/OpenEVCharger/releases/tag/2026.19.0)
— first production cut on the Rippleon target.

This is a clean-room rewrite. The J1772 state machine, fault model, and
self-test sequence are modeled on [OpenEVSE](https://github.com/OpenEVSE/open_evse)
but no source is copied.

## Why

The supported chargers ship dependent on a vendor cloud for remote
control, scheduling, and any third-party integration (Home Assistant,
evcc, an external OCPP CSMS). When the cloud is unreliable — for
example, Rippleon's `api.rippleonenergy.com` OCPP backend frequently
504s — everything except local BLE-proximity control via the vendor
app goes down with it. There is no first-party path off the cloud.

OpenEVCharger replaces both halves of the firmware so the unit
operates fully locally (Wi-Fi LAN is enough, no cloud dependency) and
exposes a standards-compliant OCPP 1.6-J Charge Point + ESPHome native
API for direct HA / evcc / SteVe / any-OCPP-CSMS integration.

## Features

### Safety core (every spec § 4 detector live or documented N/A)

| Detector | Status | Notes |
|---|---|---|
| **GFCI** (current injection trip) | ✅ live | bench-validated, ~60 ms latch |
| **GFCI CAL self-test** (boot) | ✅ live | per-unit timing tunable |
| **Relay weld** (post-open IA) | ✅ live | BL0939-IA, 3.2 s settle |
| **Relay stuck-open** (pre-charge IA) | ✅ live | 30 s window for EV BMS ramp |
| **Hard over-current** | ✅ live | spec §4 #10: ×1.20 / 5 s |
| **Soft over-current** | ✅ live | spec §4 #11: ×1.05 / 30 s, derate ramp |
| **CP=E sustained / pilot loss** | ✅ live | latched, 60 ms |
| **AC absent** | ✅ live | BL0939 V_RMS threshold |
| **Over-temp** | ✅ live | LUT-driven, hysteresis recovery |
| **Boot self-test** (ADC / relay / CP / GFCI CAL) | ✅ live | every cold boot |
| **IWDG watchdog** | ✅ live | 1 s |
| **Crash-loop safe-fail** | ✅ live | persisted counter |
| **Diode check** | ⏸ deferred to v1.1 | needs bipolar CP read-back daughterboard; stock firmware also skips |
| **CC out-of-range** | ⏸ gated | decoder live, raise gated until F6 cal |
| **PE continuity** | ⏸ deferred to v1.1 | PC5 is mains-current-coupled, can't distinguish from charging-with-PE-intact |

GFCI is the v2026.19.0 PE-related safety net (trips on stray earth current
regardless of PE-wire continuity).

Status above is current for the Rippleon target. The detectors port
across boards untouched; threshold tuning is per-chassis (e.g. PE
continuity raw band, GFCI CAL latency).

### Metering (BL0939, per-chassis cal)

- **Voltage / current / power / energy** — all bench-validated to <0.1 %
  vs external reference at full charge (Active Power 11576.4 W vs truth
  11571 W; Active Amps 47.0 A vs truth 46.97 A on a real EV draw).
- **Sub-µA precision** on IA via cal v2 schema (nA/raw scale in int16_t).
- **Per-chassis frequency reference** via cal v3 (chip's internal RC ref
  drifts ~24 % unit-to-unit).
- **`session_mwh`** integrator persisted in W25Q SPI NOR; sign-aware so
  back-flow doesn't accumulate.
- **OCPP MeterValuesSampledData** wires Voltage, Current.Import, Power,
  Energy, Current.Offered.

### Connectivity

- **Home Assistant** — ~30 sensors, ~12 buttons, ~5 numbers, ~5 switches,
  ~3 binary sensors via the ESPHome native API. RFID auth UI, OTA Push
  flow, BL0939 calibration push button.
- **OCPP 1.6-J** — full Charge Point via [esphome-ocpp-server](https://github.com/RAR/esphome-ocpp-server)
  + MicroOcpp. StartTransaction / StopTransaction / RFID auth /
  SmartCharging (`SetChargingProfile` → live amp-limit derate).
- **evcc** — works with the OCPP charger type. Live amp adjustment
  validated 2026-05-07.
- **MCU OTA** — companion-module-mediated TLV chunked-upload. HA service
  `openevcharger_fetch_and_push_ota` pulls a .bin from `/config/www/`,
  streams it over TLV to the MCU, which stages to W25Q, CRC-verifies,
  and reboots into the new image. Self-rolls-back on CRC mismatch.

### Wall-clock + logging

- **On-chip RTC bridge** — LSI-clocked counter + BKP_DATA magic survives
  any non-power-cycle reset (NRST / SYSRESETREQ / watchdog / OTA RAM
  reset / brown-out). HA pushes time on reconnect / `time.on_time_sync`
  / 30 min cron.
- **Fixed-width log timestamps** — uptime `[ssss.mmm]` pre-time-sync,
  wall-clock `[hh:mm:ss.mmm]` after.

## Architecture

```
                 ┌──────────────────────────────────────────┐
                 │  Home Assistant + evcc + OCPP CSMS       │
                 └────────────┬─────────────────────────────┘
                              │ ESPHome API (Wi-Fi)
                              ▼
   ┌────────────────────────────────────────┐
   │  Companion comms module                │
   │   (FC41D / WBR2 / etc — per board)     │
   │   ESPHome + esphome-ocpp-server        │
   │   MicroOcpp · OTA proxy · RTC bridge   │
   └─────────────────┬──────────────────────┘
                     │  TLV @ 115200 8N1
                     │  (one UART)
                     ▼
   ┌────────────────────────────────────────┐
   │  Safety MCU · FreeRTOS · GPL-3.0       │
   │   (GD32F2 / N32G45 / similar M3/M4F)   │
   │  ┌──────────────┬──────────────┐       │
   │  │ safety_task  │ comms_task   │       │
   │  │ (20 ms tick) │ (TLV I/O)    │       │
   │  └──────────────┴──────────────┘       │
   │  ┌──────────────┬──────────────┐       │
   │  │ persist_task │ io_task      │       │
   │  │ (W25Q NOR)   │ (LED/buzzer) │       │
   │  └──────────────┴──────────────┘       │
   │       │             │           │      │
   │       ▼             ▼           ▼      │
   │   J1772 PWM     BL0939 SPI    GFCI CAL │
   │   relay drive   metering      pulse    │
   └────────────────────────────────────────┘
```

Single writer for all actuation: `safety_task` is the only path that
drives the relay coil, CP PWM, and the force-open latch. All other
tasks request via inboxes.

## Hardware

The full support matrix and porting outline live in
[`BOARDS.md`](BOARDS.md). Two kinds of board:

- **MCU-firmware boards** — clean-room replacement of the safety-MCU
  firmware. Build via `cmake -DOPENEVCHARGER_BOARD=<slug>` with
  `rippleon-roc001` (bench-validated, GD32F205VG) or `nexcyber-zbu011k`
  (in-progress, Nations N32G45x). Bring up a new one by producing
  `boards/<board>/` (`board.cmake`, `pin_map.h`, `<chip>.ld`) and a
  `src/hal/<chip>/` implementation directory.
- **Companion-only boards** — for chargers where the OEM firmware is
  intentionally kept (e.g. an embedded-Linux app processor running the
  OCPP/cloud stack). The deliverable is a Linux-userland daemon that
  augments stock firmware. `eluminocity-ch21130` (Delta EVMU30; bridge
  builds + tests, bench validation pending) is the only one today.
  These don't participate in the CMake board matrix — see
  [`boards/eluminocity-ch21130/companion/`](boards/eluminocity-ch21130/companion/).

The reverse-engineering trail (full SWD dump of stock V1.0.066,
protocol decode, schematic mapping, OCPP cloud capture) is in
[`docs/mcu-re/`](docs/mcu-re/).

## Quickstart (bench)

```sh
# Host deps
sudo apt install gcc-arm-none-eabi cmake ninja-build openocd

# Fetch the GD32F20x vendor library
# (see third_party/GD32F20x_Firmware_Library/README.md)

# Build the MCU image — Rippleon ROC001 (production target)
cmake -S . -B build/rippleon-roc001 -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
    -DOPENEVCHARGER_BOARD=rippleon-roc001
cmake --build build/rippleon-roc001

# For the Nexcyber board (bench-harness target):
cmake -S . -B build/nexcyber-zbu011k -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain-cm4f.cmake \
    -DOPENEVCHARGER_BOARD=nexcyber-zbu011k
cmake --build build/nexcyber-zbu011k --target openevcharger-nexcyber-bringup

# Back up stock firmware (REQUIRED before any flash; round-trip-validated)
./tools/stock_backup.sh

# Flash via SWD (ST-Link V2 + 4-pin SWD wiring to MCU)
./tools/flash.sh
```

For the companion-module Wi-Fi side, set `csms_url` and other secrets in
`boards/rippleon-roc001/fc41d/secrets.yaml` and run:

```sh
cd boards/rippleon-roc001/fc41d && esphome run openevcharger.yaml
```

First flash is via WCH CH343G USB-UART (LibreTiny bootloader); subsequent
updates are OTA over Wi-Fi.

### Companion-only build (Eluminocity CH-21130 / Delta EVMU30)

The `delta-bridge` daemon — Linux-userland, static musl, no MCU port.
Cross-compile with the musl armv5te toolchain (see
[board README](boards/eluminocity-ch21130/README.md)):

```sh
cd boards/eluminocity-ch21130/companion
make test       # host unit tests (~85 checks across 7 suites)
make            # cross-compile delta-bridge for armv5te
```

See [`docs/bring-up.md`](docs/bring-up.md) for the full bench procedure
and the milestone-by-milestone validation log.

## Updating a deployed unit

MCU OTA is HA-mediated (no SWD needed once the firmware is on):

1. Drop the new `openevcharger.bin` into HA's `/config/www/`.
2. HA → Developer Tools → Actions → `ESPHome: openevcharger_fetch_and_push_ota`
   with `url: http://homeassistant.local:8123/local/openevcharger.bin`.
3. Watch "MCU OTA Progress" sensor 0→100. The MCU reboots into the new
   image (CRC-verified, self-rollback on mismatch).

For the Wi-Fi side: standard ESPHome OTA (`esphome upload
boards/rippleon-roc001/fc41d/openevcharger.yaml --device openevcharger.local`).

## Documentation

- **[`docs/release-notes-2026.19.0.md`](docs/release-notes-2026.19.0.md)**
  — current release notes
- **[`docs/safety.md`](docs/safety.md)** — fault inventory, gating
  rationale, hardware-only safeties
- **[`docs/bring-up.md`](docs/bring-up.md)** — bench procedure +
  milestone validation log (M0 → M10)
- **[`docs/mcu-re/`](docs/mcu-re/)** — reverse-engineering trail (stock
  SWD dump, protocol decode, OCPP capture)
- **[`docs/diode-check-investigation.md`](docs/diode-check-investigation.md)**
  — why diode check is deferred to a v1.1 hardware revision
- **[`BOARDS.md`](BOARDS.md)** — hardware support matrix
- **[`docs/superpowers/specs/`](docs/superpowers/specs/)** — design specs
- **[`docs/superpowers/plans/`](docs/superpowers/plans/)** — milestone
  implementation plans

## License

GPL-3.0-only. See [`LICENSE`](LICENSE).

## Acknowledgements

- **OpenEVSE** for the J1772 / state-machine model
- The various ODMs whose hardware turns out to be reasonably
  reverse-engineerable
- **evcc**, **MicroOcpp**, **ESPHome**, **LibreTiny** upstream maintainers

## Disclaimer — independent, unaffiliated project

This is an independent, community-developed project. It is not
affiliated with, sponsored by, endorsed by, or in any way associated
with any of the OEMs or US-side brand owners whose hardware it
interoperates with. Brand and model names are used solely in a
descriptive (nominative) capacity to identify the hardware this
project replaces firmware on; all trademarks, service marks, and
product names remain the property of their respective owners.

No proprietary firmware, source code, signing keys, or other
protected material from any OEM is redistributed by this project.

### Per-board nominative-use statement — Rippleon ROC family

The names "Rippleon" and "ROC001" are used solely to identify the
hardware this project interoperates with. This project is not
affiliated with, sponsored by, or endorsed by RIPPLEON Energy or any
of its subsidiaries.

If you are a representative of RIPPLEON Energy and have a concern
about this repository, please open a GitHub issue and we will engage
in good faith.

### Scope of firmware replacement

OpenEVCharger replaces *both* halves of the stock firmware on the
user's own hardware:

- **Safety MCU** (GD32F205VG on Rippleon; Nations N32G45x on the
  in-progress Nexcyber port) — bare-metal C + FreeRTOS safety core. A
  clean-room rewrite modelled on the OpenEVSE J1772 / fault / self-test
  approach (see [`docs/superpowers/specs/`](docs/superpowers/specs/));
  no upstream OpenEVSE source code is incorporated, and no portion of
  any stock vendor firmware is included or derived.
- **Wi-Fi/BLE companion module** (BK7231N on Rippleon's FC41D;
  RTL8720CF / AmebaZ2 on Nexcyber's WBR2) — ESPHome + LibreTiny,
  original integration code (`boards/rippleon-roc001/fc41d/openevcharger.yaml` + the local
  `openevcharger_tlv` component) talking to the MCU over a custom TLV
  protocol on a single shared UART.

Both images are original work and ship as a complete, voluntary
replacement for the stock vendor firmware on each chip. Stock firmware
is preserved via the documented `tools/stock_backup.sh` workflow before
flashing, so installation is reversible if a stock SWD dump was taken
beforehand.

Protocol documentation and reverse-engineering artefacts in this
repository (UART/Bluetooth captures, SWD dumps, disassembly notes for
stock vendor images) were developed independently from publicly
observable behaviour and from analysis of the user's own purchased
hardware, for the sole purpose of interoperability — a use expressly
permitted by 17 U.S.C. § 1201(f) (US) and Article 6 of EU Directive
2009/24/EC.

Installing this firmware will void the manufacturer's warranty and may
render the device unusable. Use at your own risk.
