# OpenEVCharger

Replacement firmware for the **Rippleon ROC001** EV charger (FCC model
**ROCHL2US**, FCC ID `2BLBS-ROCHL2US`), targeting the GigaDevice
**GD32F205VG** main MCU paired with a Quectel **FC41D** (BK7231N)
Wi-Fi/BLE module. Restores the unit to local control — Home Assistant,
OCPP 1.6-J, evcc — when the vendor cloud is unreliable or unavailable.

The hardware comes out of **Beizide / NewEnergyCS** (Chinese
parent/factory, sites at `beizide.com` + `newenergycs.com`); **Rippleon
Ltd.** is their US-facing sub-brand and FCC-registered entity. Same
factory firmware also serves the BeiZide-branded SKUs (both cloud URLs
are hardcoded in stock V1.0.066).

**Latest release:** [`2026.19.0`](https://github.com/RAR/OpenEVCharger/releases/tag/2026.19.0)
— first production cut, validated end-to-end against a real 240 V EV
2026-05-07.

This is a clean-room rewrite. The J1772 state machine, fault model, and
self-test sequence are modeled on [OpenEVSE](https://github.com/OpenEVSE/open_evse)
but no source is copied. The MCU runs a small FreeRTOS-based safety core
in C; Wi-Fi/BLE/cloud lives on the FC41D, off the safety MCU, talking
over UART4 with a binary TLV protocol.

## Why

Rippleon ROC001 ships dependent on a vendor cloud
(`api.rippleonenergy.com`) whose OCPP backend frequently 504s. Local
BLE-proximity control via the vendor app still works when the cloud is
down, but **remote** control, scheduling, and any third-party
integration (Home Assistant, evcc, an external OCPP CSMS) all route
through the cloud and break with it. There is no first-party path off
the cloud. OpenEVCharger replaces both halves of the firmware so the
unit operates fully locally — Wi-Fi LAN is enough, no cloud
dependency — and exposes a standards-compliant OCPP 1.6-J Charge Point
+ ESPHome native API for direct HA / evcc / SteVe / any OCPP CSMS
integration.

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
- **MCU OTA** — FC41D-mediated TLV chunked-upload. HA service
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
   │  FC41D (BK7231N, LibreTiny)            │
   │   ESPHome 2026.4.4 + esphome-ocpp-server│
   │   MicroOcpp · OTA proxy · RTC bridge   │
   └─────────────────┬──────────────────────┘
                     │  TLV @ 115200 8N1
                     │  (UART4)
                     ▼
   ┌────────────────────────────────────────┐
   │  GD32F205VG · FreeRTOS · GPL-3.0       │
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
drives PE12 (relay coil), CP PWM, and PB12 (force-open latch). All
other tasks request via inboxes.

## Hardware

| Target | Status | Notes |
|---|---|---|
| Rippleon ROC family (FCC `ROCHL2US`) | ✅ Production-validated on ROC001; siblings in scope per Rippleon's own FCC filing | Single-phase 6–48 A, BL0939 metering, FC41D Wi-Fi/BLE, GD32F205VG. US brand: Rippleon (FCC entity Rippleon Ltd.); ODM: Beizide / NewEnergyCS (CN). Per FCC filing the following 73 SKUs are declared electrically identical aside from spec/appearance/color — **ROC001–ROC010 × W/B/G/L** (40 residential SKUs) plus **NECS-ACW-{7, 9.6, 11.5}-1-US-{3010, 3018, 3020, 3027, 3028, 3110–3115}** (33 commercial-tier SKUs). |
| Other GD32F2-class EVSE | 🔲 needs port | Pin-map + per-chassis cal in `BOARDS.md`. |

The reverse-engineering trail (full SWD dump of stock V1.0.066,
protocol decode, schematic mapping, OCPP cloud capture) is in
[`docs/mcu-re/`](docs/mcu-re/).

## Quickstart (bench)

```sh
# Host deps
sudo apt install gcc-arm-none-eabi cmake ninja-build openocd

# Fetch the GD32F20x vendor library
# (see third_party/GD32F20x_Firmware_Library/README.md)

# Build the MCU image
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake -G Ninja
ninja -C build

# Back up stock firmware (REQUIRED before any flash; round-trip-validated)
./tools/stock_backup.sh

# Flash via SWD (ST-Link V2 + 4-pin SWD wiring to MCU)
./tools/flash.sh
```

For the FC41D Wi-Fi side, set `csms_url` and other secrets in
`fc41d/secrets.yaml` and run:

```sh
cd fc41d && esphome run openevcharger.yaml
```

First flash is via WCH CH343G USB-UART (LibreTiny bootloader); subsequent
updates are OTA over Wi-Fi.

See [`docs/bring-up.md`](docs/bring-up.md) for the full bench procedure
and the milestone-by-milestone validation log.

## Updating a deployed unit

MCU OTA is HA-mediated (no SWD needed once the firmware is on):

1. Drop the new `openevcharger.bin` into HA's `/config/www/`.
2. HA → Developer Tools → Actions → `ESPHome: openevcharger_fetch_and_push_ota`
   with `url: http://homeassistant.local:8123/local/openevcharger.bin`.
3. Watch "MCU OTA Progress" sensor 0→100. The MCU reboots into the new
   image (CRC-verified, self-rollback on mismatch).

For the FC41D half: standard ESPHome OTA over Wi-Fi (`esphome upload
fc41d/openevcharger.yaml --device openevcharger.local`).

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
- **Beizide / NewEnergyCS** (Rippleon ODM) for shipping reasonably
  reverse-engineerable hardware
- **evcc**, **MicroOcpp**, **ESPHome**, **LibreTiny** upstream maintainers
