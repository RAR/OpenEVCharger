# Supported Boards

OpenEVCharger targets ARM Cortex-M3 / M4F EVSE controller MCUs (GD32F2,
N32G45, and similar STM32F2-class chips). The core (FreeRTOS task
layout, J1772 state machine, OCPP/TLV protocol, OTA, RTC bridge,
persistence) is board-independent. The board-specific surface lives in:

- `src/core/pin_map_<board>.h` — GPIO pin → function map.
- `linker/<chip>.ld` — flash + RAM layout.
- `CMakeLists.txt` — vendor SPL paths, MCU flags, clock config
  (selected via `-DOPENEVCHARGER_BOARD=<board>`).

Adding a board means producing those three artefacts; everything in `src/`
above the HAL should compile unchanged.

## Bench-validated

| Board | MCU | Status | Notes |
|---|---|---|---|
| **Rippleon ROC001** (FCC `ROCHL2US`) | GD32F205VG | ✅ Bench-validated through M7 + GFCI live, OTA, RTC bridge | Single-phase 6–48 A, BL0939 metering, FC41D Wi-Fi/BLE module on UART4. Pin map fully reverse-engineered from stock V1.0.066. US brand: Rippleon (FCC entity Rippleon Ltd., FCC ID `2BLBS-ROCHL2US`); ODM: Beizide / NewEnergyCS (CN — `beizide.com` / `newenergycs.com` are the same parent factory). |

## In scope per Rippleon's FCC filing — sibling SKUs

Rippleon's FCC filing for ID `2BLBS-ROCHL2US` declares the following 73 SKUs
electrically identical aside from spec / appearance / color. They have not been
individually bench-validated, but should drop in without code changes:

**ROC residential family (40 SKUs)** — `ROC{001,002,003,004,005,006,007,008,009,010}{W,B,G,L}`

**NECS-ACW commercial family (33 SKUs)** — `NECS-ACW-{7, 9.6, 11.5}-1-US-{3010, 3018, 3020, 3027, 3028, 3110, 3111, 3112, 3113, 3114, 3115}` (single-phase, US, 7 / 9.6 / 11.5 kW tiers).

The "NECS-ACW" prefix here is Rippleon's own commercial SKU naming. It is
**not** related to the unrelated company **Nexcyber** despite the lookalike.

## In progress

### Nexcyber AC EVSE — Zopoise ZBU011K-C00X PCBA

US-market AC wallbox sold under the "Nexcyber" brand on Amazon. The
OEM is **Zhuzhou Zopoise Technology** (Hunan, China — `zopoisetech.cn`
/ `zopoisetech.en.alibaba.com`). The packaged-wallbox SKU is
`ZB04-U011KBH-F017`; Zopoise also sells the bare PCB assembly to
integrators as `ZBU011K-C00X`. Same chassis ships in Europe as
**Blitzwolf** `ZB04-E007/E011/E022 KBC` and as **S-bol** `ZB04-E007K`.

| | Nexcyber (on bench) | siblings (same PCBA family) |
|---|---|---|
| Packaged SKU | `ZB04-U011KBH-F017` | — |
| PCBA SKU | `ZBU011K-C00X` | `ZBU007K-C00X` (32 A) · `ZBU09K6-C00X` (40 A) |
| Region | US 240 V split-phase | US 240 V split-phase |
| Max current | 48 A | 32 A / 40 A |

Hardware differences vs Rippleon — **not interchangeable**:

- **MCU:** Nations N32G45x (Cortex-M4F) — not GD32F2 / Cortex-M3.
- **Wi-Fi module:** Tuya WBR2 (Realtek RTL8720CF / AmebaZ2) — not
  Quectel FC41D (BK7231N).
- **Display:** Nextion HMI — not the OEM LED strip.
- **Cloud architecture:** stock Tuya / Smart Life device (talks
  `tuyacn.com` / `m2.tuyacn.com:8883` over standard TuyaMCU). No
  OCPP, no custom REST API. Rippleon runs a custom OCPP/REST backend
  behind the FC41D; the only thing in common is that both are
  cloud-tethered.

Port status: hardware on the bench, SWD probe + 128 KB flash dumped
(2026-05-07), pin map being reverse-engineered against the live unit.
The 7 kW and 9.6 kW US siblings should drop in with zero code changes
once the 11 kW port lands — same PCBA, same MCU, same Wi-Fi module,
same DP map; only contactor and advertised-A ceiling differ. They
aren't on a bench so they won't have their own `boards/` entries.

Feasibility sketch (≈ 3-4 calendar weeks part-time, ~70 % of codebase
ports unchanged): [`docs/ports/nexcyber-feasibility.md`](docs/ports/nexcyber-feasibility.md).

If you have an EVSE PCB you'd like to add, open an issue with:

- Photographs of both sides, MCU silkscreen visible.
- `lsusb` / chip auto-detect output from `openocd` (so we know the flash
  size + chip variant).
- Schematic if available; otherwise we'll need a scope to trace pinout
  the same way ROC001 was done.

## Porting outline

For the curious — what porting actually involves:

1. **MCU bring-up** — clock tree (`src/hal/clock.c`) + UART logger
   (`src/hal/uart.c`) come up. `printk()` over your debug UART confirms
   the chip is alive at the right frequency.
2. **Pin map** — produce `src/core/pin_map_<board>.h`. Required entities:
   relay drive + sense, CP PWM out + sense ADC, CC sense, GFCI sense,
   PE continuity, AC absent, NTCs, button matrix, LED strip, SPI to
   external NOR, UART to comms module, FC41D / equivalent power + reset.
3. **HAL re-pointing** — adjust the SPL include paths in CMakeLists if
   the chip is a different family (e.g. STM32F207 instead of GD32F205);
   most peripheral code in `src/hal/` uses the legacy F1/F2 SPL idioms.
4. **Linker script** — match the chip's flash + RAM size + `.ramfunc`
   region for the OTA apply path.
5. **Re-validate the safety detectors** with bench gear: GFCI, relay
   weld, over-temp, J1772 state machine. The detectors are written to
   spec but threshold tuning is per-board (e.g. PE continuity raw band).

The TLV protocol stays bit-for-bit compatible across boards so the
FC41D-side ESPHome integration (`fc41d/openevcharger.yaml`) and HA UX
work unchanged.
