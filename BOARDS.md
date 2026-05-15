# Supported Boards

OpenEVCharger supports two kinds of board:

- **MCU-firmware boards** — clean-room replacement of the safety-MCU
  firmware. Targets ARM Cortex-M3 / M4F EVSE controller MCUs (GD32F2,
  N32G45, and similar STM32F2-class chips). The core (FreeRTOS task
  layout, J1772 state machine, OCPP/TLV protocol, OTA, RTC bridge,
  persistence) is board-independent. Built with
  `cmake -DOPENEVCHARGER_BOARD=<slug>`; valid slugs are
  `rippleon-roc001` and `nexcyber-zbu011k`.
- **Companion-only boards** — for chargers where the OEM firmware is
  intentionally kept (typically an embedded-Linux app processor on the
  OEM side runs the OCPP / web-UI stack). The deliverable is a
  Linux-userland daemon that augments stock firmware via shared memory.
  These boards do **not** participate in the CMake board matrix; they
  build via their own `companion/Makefile` (musl cross-compile).
  Only one today: `eluminocity-ch21130`.

The board-specific surface lives in `boards/<board>/`:

- `board.cmake` — vendor-SDK paths, source lists, compile definitions,
  linker script selection, and any additional CMake targets (e.g. the
  Nexcyber bench-harness target). Included by the top-level
  `CMakeLists.txt` after board selection.
- `pin_map.h` — GPIO pin → function map.
- `<chip>.ld` — flash + RAM layout for that chip
  (`gd32f205vg.ld` for rippleon-roc001; `n32g45x.ld` for nexcyber-zbu011k).

The shared HAL interface is `src/hal/*.h`. Per-chip implementations live
in `src/hal/<chip>/`: `src/hal/gd32f205/` for the GD32F205 (Rippleon) and
`src/hal/n32g45x/` for the Nations N32G45x (Nexcyber). Peripherals whose
API genuinely diverges between boards (e.g. `adc_scan`, `gfci`, `relay`)
carry board-specific `*_nx.h` headers alongside their implementation in
the chip directory. Board-unique peripherals (e.g. Nexcyber's Nextion
display, LED ring, SPI2) also live in `src/hal/n32g45x/` with their own
headers. Chip-independent external-device drivers live in `src/drivers/`
(currently `w25q`).

A chip-implementation directory that uses `OEVC_HAL_STUB()` for some
functions (defined in `src/hal/oevc_hal_stub.h`) has an incomplete
production target: the firmware configures, compiles, and links but is
not yet functional on hardware.

Adding a board means producing a `boards/<board>/` directory with the
three artefacts above, plus `src/hal/<chip>/` implementations;
everything in `src/` above the HAL should compile unchanged.

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
(2026-05-07), N32 HAL ported and building. Two targets:
`openevcharger-nexcyber-bringup` (the M0-M4 bench harness — the image
actually flashed during bring-up) and `openevcharger` (production —
currently a compile/link gate: configures, compiles, and links against
`src/main.c` but is not yet functional, as the N32 HAL uses
`OEVC_HAL_STUB()` for several peripherals).
Board scaffolding: [`boards/nexcyber-zbu011k/`](boards/nexcyber-zbu011k/README.md).
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

## Companion-only (no MCU firmware port)

### Eluminocity CH-21130 — Delta EVMU3015HNSEL OEM

Factory-virgin 2016 Delta AC Mini wallbox rebranded by Eluminocity (a
defunct German operator, ~2018-2020) for BMW Light & Charge / ReachNow
streetlight fleets in Seattle, Munich, Chicago, Oxford, Eindhoven. The
sibling AC Mini Plus (`EVPE3215MUK`, V02.0B.06) ships the same chassis
under various rebrands; we target the older 2016 generation.

Architecture is fundamentally different from the MCU-firmware boards:
an ARM926 Linux app processor runs OCPP 1.5 + a web UI as the
"companion"; a STM32F334 is the safety MCU and stays a trusted black
box. The companion-only deliverable is **`delta-bridge`** — a read-only
C daemon that attaches to the stock firmware's SysV shared-memory
segment and publishes charger state to Home Assistant over MQTT with HA
discovery. The bridge is non-invasive (no shmem writes in v1) and
crash-isolated from the safety path.

| | |
|---|---|
| Packaged SKU | Eluminocity CH-21130 (= Delta `EVMU3015HNSEL`) |
| Region | US 240 V split-phase, 30 A hardwired |
| App processor | ST SPEAr320S (ARM926EJ-S @ 333 MHz, ARMv5TE) |
| Safety MCU | STM32F334C8T6 — **untouched** by this project |
| Wi-Fi | Sparklan WUBA-171GN (Atheros AR9271 USB) |
| Stock cloud stack | OCPP 1.5 SOAP + DeltaOCPP daemon (kept running) |
| Bridge transport | MQTT 3.1.1 over plain TCP, QoS 0, HA discovery, LWT |
| Distribution | USB-flashable `DcoFImage` (legacy `DELTADCOF` magic + byte-sum) — also produces a `DcoFImage-stock-restore` revert image |
| Build | `cd boards/eluminocity-ch21130/companion && make` (musl armv5te) |
| Status | Bridge implemented + 85/85 host tests + clean cross-compile (CI green); bench validation milestones M0–M3 pending |

Board scaffolding: [`boards/eluminocity-ch21130/`](boards/eluminocity-ch21130/README.md).
RE docs (inter-MCU protocol, shmem layout, OCPP / firmware bundle,
decoded shmem snapshot) live in [`boards/eluminocity-ch21130/docs/`](boards/eluminocity-ch21130/docs/).
Spec + plan: [`docs/superpowers/specs/2026-05-14-eluminocity-ch21130-mqtt-bridge-design.md`](docs/superpowers/specs/2026-05-14-eluminocity-ch21130-mqtt-bridge-design.md)
and the matching `plans/` doc.

## Porting outline

For the curious — what porting actually involves:

1. **MCU bring-up** — create `boards/<board>/` with a `board.cmake`,
   then add `src/hal/<chip>/clock.c` + `src/hal/<chip>/uart.c`.
   `printk()` over your debug UART confirms the chip is alive at the
   right frequency.
2. **Pin map** — produce `boards/<board>/pin_map.h`. Required entities:
   relay drive + sense, CP PWM out + sense ADC, CC sense, GFCI sense,
   PE continuity, AC absent, NTCs, button matrix, LED strip, SPI to
   external NOR, UART to comms module, Wi-Fi module power + reset.
3. **HAL implementation** — fill out `src/hal/<chip>/` to implement
   every header in `src/hal/`. Start with stubs (`OEVC_HAL_STUB()`)
   to get a compile-gated production target, then replace stubs with
   real implementations. Peripherals unique to the board (e.g. a
   different display or LED driver) go in `src/hal/<chip>/` with their
   own headers.
4. **Linker script** — `boards/<board>/<chip>.ld`: match the chip's
   flash + RAM size + `.ramfunc` region for the OTA apply path.
5. **`board.cmake`** — wire in vendor-SDK include paths, MCU compiler
   flags, and source lists for `src/hal/<chip>/`. Point `LINKER_SCRIPT`
   at `boards/<board>/<chip>.ld`.
6. **Re-validate the safety detectors** with bench gear: GFCI, relay
   weld, over-temp, J1772 state machine. The detectors are written to
   spec but threshold tuning is per-board (e.g. PE continuity raw band).

The TLV protocol stays bit-for-bit compatible across boards so the
FC41D-side ESPHome integration (`boards/rippleon-roc001/fc41d/openevcharger.yaml`) and HA UX
work unchanged.
