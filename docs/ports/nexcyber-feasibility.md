# OpenEVCharger → Nexcyber AC EVSE — Port Feasibility

**Status:** Sketch (2026-05-07). Hardware on hand, fully reverse-engineered.
Port not started. Numbers below are estimates from one engineer's perspective.

## TL;DR

- **Verdict:** Feasible, ~70 % of the codebase ports unchanged.
- **Effort:** **12-17 person-days serial**, ≈ 3-4 calendar weeks part-time.
- **Biggest risk:** Internal flash is only 128 KB and there's no confirmed
  external SPI NOR on the PCB — persistence model needs redesign.
- **Biggest unlock:** The Nexcyber DP map (per `esphome/testcharger/NOTES.md`)
  appears to be shared across many Tuya-based EVSEs, so a successful port
  generalises to a broader product family rather than just one bench unit.

## Target hardware (already RE'd)

| Component | Part | Notes |
|---|---|---|
| Main MCU | Nations N32G45x | Cortex-M4F, **STM32F1-style** peripheral layout, 128 KB flash, ≈144 KB SRAM (TBC) |
| Wi-Fi/BLE module | Tuya WBR2 (RTL8720CF / AmebaZ2 internally) | Already supported by ESPHome via LibreTiny |
| Display | Nextion HMI | Pages: `setting`, `nogun`, `chargeing`, `waittime`; well-known protocol |
| Metering | BL0939 | Same chip as Rippleon, likely on hardware SPI2 (PB12-15) — *confirm bench tomorrow* |
| Driver array | ULN2003 | 7-channel Darlington, drives both 12 V relay coils |
| AC voltage sense | 2× ZMPT107-1 | One per leg → split-phase L1 + L2 |
| Main contactors | 2× 75 A / 1000 VAC, 12 V coil | Double-pole break |
| GFCI test relay | 1× small relay near NTC/CP connector | Test-pulse injection |
| GFCI module | 4-wire variety | CT around one line — *confirm whether metering or GFCI* |
| Front panel | Cap-touch with 2 buttons | TTP223-style ICs → PA11 / PA12 |
| Analog conditioning | LM2904 dual op-amp | ZMPT107 amplification (× 2 legs) before ADC |
| Mains isolation | TLP293-2 dual photocoupler | L1 + L2 mains-presence detection (60 Hz pulse stream when live → MCU IDR) |

Pinout: see `esphome/testcharger/NOTES.md` "Nations N32G45x main MCU pinout"
section. 27 of ~36 usable pins are mapped from firmware static analysis;
remaining 9 will be confirmed by mains-on bench wiggle tomorrow.

## What ports cleanly vs needs rewriting

| Layer | Compatibility | Notes |
|---|---|---|
| Safety supervisor (19-fault catalog) | ✅ portable | logic only, no HAL dep |
| EVSE state machine | ✅ portable | logic only |
| J1772 CP decoder + duty calc | ✅ portable | math only; PWM-driver HAL changes |
| OCPP 1.6-J + MicroOcpp + SmartCharging | ✅ portable | runs on the Wi-Fi module, not MCU |
| TLV protocol (MCU↔Wi-Fi) | ✅ portable | byte-level binary protocol |
| Calibration record handling (v1→v2→v3) | ✅ portable | logic only |
| Persistence ping-pong + crash log | ✅ portable | depends on flash availability — see risks |
| Session log | ✅ portable | logic only |
| ESPHome integration (`fc41d/openevcharger.yaml`) | ⚠️ ~80 % | new YAML targeting `rtl87xx` instead of `bk72xx`; reuse `openevcharger_tlv` component verbatim |
| **GPIO HAL** | ❌ rewrite | F1 CRL/CRH (4-bit nibble) ≠ F2 MODER/OTYPER/OSPEEDR/PUPDR |
| **UART HAL** | ❌ rewrite | Similar conceptually; register offsets and clock-enable bits differ |
| **ADC HAL** | ❌ rewrite | F1 single-mode + scan sequence; channel 8/9 on PB0/PB1 etc. |
| **Timer HAL (CP PWM)** | ❌ rewrite | TIM1 register layout + breaking-input behaviour differs |
| **Clock tree (RCU)** | ❌ rewrite | Nations-specific PLL chain |
| **Linker script** | ❌ rewrite | 128 KB flash + ~144 KB RAM |
| **OTA staging path** | ⚠️ redesign | Smaller flash + likely no external NOR — may have to drop self-rollback |

## Phased plan (rough)

### Phase 1: bring-up (3-4 days)

- Create `boards/nexcyber/` with `pin_map.h`, `linker.ld`, `clock.c`, `CMakeLists.txt`
- Get `printk()` over USART1 (the WBR2 link can dual-use during bring-up
  before the WBR2 firmware is reflashed)
- Validate clock tree comes up at the advertised frequency
  (N32G45x typically runs 144 MHz from HSE × PLL)
- Sanity test: blink an OUT_PP pin at 1 Hz, confirm with scope

### Phase 2: HAL port (4-5 days)

- **GPIO HAL** in CRL/CRH idiom (this is the most pervasive change — touched
  by every peripheral driver)
- **UART HAL** for USART1 (WBR2 link), USART2 (Nextion), USART3 (TBD — BL0939
  if UART variant, debug log otherwise)
- **ADC HAL** with single-mode + scan sequence; need DMA setup for continuous
  sampling on the 5 channels (2× ZMPT107, CP, CC, NTC)
- **Timer HAL** for TIM1 CH1 (CP PWM) — most complex single piece because of
  J1772 timing constraints (1 kHz, sub-1% duty granularity)
- Each peripheral validated with a dedicated bring-up unit

### Phase 3: integration (2-3 days)

- Wire safety supervisor + state machine to new HAL
- Re-tune ADC calibration constants — split-phase 2× ZMPT107 means two
  voltage readings (we already have a v2 cal schema field for this; might
  need v4 with 6 fields instead of 4)
- Get J1772 idle state passing: CP at 12 V, no faults raised, contactor
  open, ULN2003 drives confirmed off

### Phase 4: peripheral integration (2-3 days)

- BL0939 driver — port from Rippleon's bit-banged version to use the
  hardware SPI2 peripheral. Faster + simpler.
- Nextion display protocol — different command set from Rippleon's DGUS
  but well-documented (`page X`, `t0.txt="..."`, `t0.pic=N`, terminator
  `\xFF\xFF\xFF`). Rough lift, not a port.
- Cap-touch button handling on PA11 / PA12 (simple GPIO IRQ on falling edge)
- ULN2003 relay control with the new pin map

### Phase 5: validation (1-2 days)

- Bench safety walk (B↔C↔A J1772 transitions, all 19 faults)
- Real EV F5 charging session
- OCPP smoke test against an evcc CSMS

### Total: 12-17 days serial, ≈ 3-4 calendar weeks part-time

## Risks

- 🟢 **Algorithm portability** — proven approach
- 🟢 **ESPHome side** — RTL8720CF is a well-trodden LibreTiny target
- 🟡 **Vendor SDK quality** — Nations SPL may have bugs comparable to the
  GD32F20x SDK 120m_hxtal PLL bug we ran into. Budget 1-2 days for
  vendor-bug-spelunking surprises.
- 🟡 **Persistence redesign** — if there's no external SPI NOR, we either
  dedicate an internal flash sector for ping-pong (write-cycle limited but
  workable for boot count + cal record) or drop persistence to RAM-only
  with HA-side mirror. **Decide before starting Phase 4.**
- 🟡 **Flash size** — current OpenEVCharger image is ~52 KB. With
  Nations SPL bloat + redundant safety headers it could grow to ~100 KB,
  leaving ~28 KB margin in 128 KB. Tight. May need image compression
  for OTA staging (probably impossible without external NOR anyway).
- 🟢 **Hardware availability** — already on bench, no procurement risk.

## Prerequisites before starting

1. **OpenEVCharger v1.0.0-roc001 stable for ≥ 2 weeks** in the garage —
   we want to know v1.x is fault-free before splitting attention.
2. **Confirm N32G45x SRAM size** by SWD memory test (currently estimated
   144 KB from heuristic — needs verification before linker script).
3. **Bench-confirm presence/absence of external SPI NOR** — visual
   inspection or scope SCK during boot. Affects persistence design choice.
4. **Tomorrow's mains-on wiggle** to nail the last ~9 pins.

## Why it's worth doing

- The testcharger DP map and "Tuya MCU + WBR2 + Nextion + BL0939"
  topology is shared across multiple OEMs. A working port proves
  OpenEVCharger generalises beyond the Rippleon family and unlocks an
  effective "rescue firmware" for Tuya-cloud-locked EVSEs.
- Reverse-engineering investment is largely paid: pinout 85% done, all
  external chips identified, GFCI architecture mapped, touch panel
  understood.
- Forces the OpenEVCharger HAL boundary to actually exist (rather than
  being implicit). Cleaner architecture for future ports.
- Unlike Rippleon's bit-banged BL0939 + custom DGUS protocol, Nexcyber
  uses standard parts (Nextion, hw SPI). Cleaner reference port.
- Validates the BOARDS.md "Porting outline" is actually followable.

## Recommended sequencing

1. Finish 2026-05-08+ bench session (mains on, wiggle game continuation,
   ADC scope). Pin map → ≥ 95 %.
2. Confirm or add external SPI NOR. Settle persistence design.
3. Wait for OpenEVCharger v1.1 to land (cal v3 polish, mains-freq entity
   drop, anything else from the v1.1 followups list). Don't split focus.
4. Spin up `boards/nexcyber/` skeleton on a feature branch; pure
   bring-up only — don't touch logic layer yet.
5. Iterate Phase 2 HAL piece-by-piece. Keep the Rippleon target buildable
   throughout (CI on both boards).
6. Document each Nations-SPL gotcha in `docs/ports/nexcyber-gotchas.md`
   for the next porter (likely STM32F1-clone vendors more broadly).

---

**Open questions for the engineer who picks this up:**

- Does Nations have an OpenOCD target file we missed, or does
  `target/stm32f4x.cfg` continue to work end-to-end (including flash
  erase/write — we've only done read so far)?
- Does the WBR2 silicon's RTL8720CF have enough spare flash for both
  the ESPHome image *and* MicroOcpp staging? (FC41D was 2 MB usable
  on a 4 MB part. AmebaZ2 is similar.)
- Is the WBR2 boot strap (PA0 to 3.3 V + CEN pulse) the same procedure
  used here as on `testcharger.yaml`?
