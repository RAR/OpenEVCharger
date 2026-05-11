# OpenEVCharger — Nexcyber board port (in progress)

**Status:** M0 bring-up — clock + log UART + heartbeat printk only.
Buildable, not yet flashable to a real EVSE (no safety core).

This directory holds the board-specific scaffolding for porting
OpenEVCharger to the Nexcyber AC EVSE — actually a Zopoise `ZBU011K-C00X`
PCBA (Nations N32G45x main MCU + Tuya WBR2 / RTL8720CF Wi-Fi/BLE
module). It also covers the broader family of Zopoise ZB04-series
chassis that share the same PCBA (Nexcyber US 32/40/48 A, Blitzwolf
EU, S-bol). DP map + protocol notes in
`esphome/testcharger/NOTES.md` in the parent device-configs repo.

For overall feasibility, effort estimate, and architectural decisions
see [`docs/ports/nexcyber-feasibility.md`](../../docs/ports/nexcyber-feasibility.md)
in the OpenEVCharger root.

## What's here (M0)

| File | Purpose | Status |
|---|---|---|
| `pin_map.h` | All confirmed pins from the 2026-05-07 SWD firmware dump + 2026-05-09 mains-on wiggle | 27 of ~36 pins confirmed; 7 ULN2003 outputs + 3 candidate digital inputs still need bench resolution (EV-simulator pass through state-B/C) |
| `n32g457.ld` | Linker: 120 KB FLASH + 8 KB PERSIST + 80 KB RAM | Drafted; assumes 2 KB sector geometry (per N32G45x reference manual) — bench-confirm before first flash erase |
| `hal/clock.c` | Clock bring-up | M0 — trusts SDK SystemInit (HSE_PLL → 144 MHz default); publishes the rate via printk |
| `hal/uart.c` | USART1 (PA9/PA10) at 115200 8N1 + printk | M0 — bare-metal, no FreeRTOS or timestamp prefix; same `uart_init` / `uart_write` / `printk` surface as rippleon's `src/hal/uart.c` |
| `main.c` | M0 entry point | Clock + UART up + heartbeat printk loop. No FreeRTOS, no tasks, no peripherals beyond the log UART |

## Build

```bash
cmake -S . -B build_nexcyber -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain-cm4f.cmake \
    -DOPENEVCHARGER_BOARD=nexcyber
cmake --build build_nexcyber
```

Output: `build_nexcyber/openevcharger.{elf,bin,hex,map}`. Current
sizes are tiny — ~2.8 KB FLASH / ~2.6 KB RAM — because the image only
covers M0. They grow as M1+ HAL files port.

The rippleon target (`OPENEVCHARGER_BOARD=rippleon`, default) is
unaffected and continues to build with the existing M3 toolchain file.

## Roadmap — what's still TODO

### Bench-blocked (need hardware in hand)

- **First flash** — `openevcharger.bin` onto the bench unit via SWD,
  scope PA9 for the heartbeat printk. Confirms toolchain + linker +
  startup + SDK all land coherently on real silicon. ST-LINK V2
  wiring is already documented in the SWD-probe memory.
- **Mains-on wiggle** to identify the remaining 7 silent OUT_PP pins
  via J1772 state walk (need EV simulator or hardware mod for fake
  state-B/C).
- **BL0939 variant** — scope PB11 (USART3 RX) vs PB14 (SPI2 MISO)
  during stock boot: whichever carries continuous data is the
  metering link.
- **ADC channel assignment** — CP / CC / AC / NTC. Walk J1772 state
  with a tester, scope each ADC pin (PB1/PB2/PC0/PC4/PC5).
- **Capacitive touch button sweep** — IDR readback on PA11/PA12
  during taps.
- **GFCI sense polarity** — wiggle wire near CT, watch PC13.

### Code (can do without bench)

| Layer | Status |
|---|---|
| GPIO HAL | not started |
| UART (log) | ✅ M0 — USART1 on PA9/PA10 |
| ADC HAL | not started |
| Timer HAL (CP PWM) | not started |
| Clock tree (RCC) | ✅ M0 — SDK default HSE_PLL 144 MHz |
| FreeRTOS port (`portable/GCC/ARM_CM4F`) | not started — M1 |
| Persistence ping-pong | not started — redesign for internal-flash (this file's PERSIST region) |
| OTA staging path | not started — redesign single-bank, CRC-pre-verify, atomic from RAM, no self-rollback |
| Safety core port | not started — pulls in src/core/, src/persist/, src/tasks/ |
| pin_map `PIN_LOG_UART_*` aliases | TBD — currently the HAL uart.c hard-codes USART1; promote to aliased macros once we decide whether to keep that or move log onto a free UART (USART3 PB10/PB11 candidate, depends on what BL0939 turns out to use) |

## Companion ESPHome side

Once the MCU side reaches M2+ (TLV server up + a handful of state
entities exporting), the Wi-Fi-side YAML follows. Starting scaffold:
[`esphome/testcharger/testcharger.yaml`](../../../testcharger/testcharger.yaml)
in the parent device-configs repo — the bench unit's current ESPHome
config that talks **stock TuyaMCU** to the **stock Nations firmware**
via the WBR2 module. The scaffold (rtl87xx platform, board variant,
PIN_SERIAL0_RX/TX build flags, USART pins, api/wifi/logger/time
blocks) carries over verbatim.

The adaptation to talk our protocol is a swap, not a rewrite:

| Strip out (TuyaMCU world) | Add (OpenEVCharger world) |
|---|---|
| `tuya:` component block | `external_components:` pointing at `../OpenEVCharger/fc41d/components/openevcharger_tlv/` (local) + `RAR/esphome-ocpp-server` (git) |
| Every `on_datapoint_update` lambda + `platform: tuya` sensor / switch / number | `openevcharger_tlv:` block + the same ~30 sensors / 12 buttons / 5 numbers / 5 switches / 3 binary_sensors as `OpenEVCharger/fc41d/openevcharger.yaml` |
| 30 s `uart.write` cron pulling the DATAPOINT_QUERY frame | nothing — MCU push-publishes via TLV STATE_REPORT |
| `tuya_time` sync block | The TLV protocol's existing time-set command (already wired in the rippleon fc41d YAML) |

End shape mirrors `OpenEVCharger/fc41d/openevcharger.yaml` — same
HA UX, same OCPP behavior, same TLV component verbatim. The only
real differences are the LibreTiny platform (`rtl87xx` vs `bk72xx`)
and the UART pin remap (PA13/PA14 in the WBR2's UART0 pinmux table,
not the BK7231N's P10/P11). Targets the bench unit at `testcharger`
on the network; eventual home in the repo will be
`OpenEVCharger/wbr2/openevcharger.yaml` parallel to `fc41d/`.

Out of scope until then — `testcharger.yaml` stays as the bench-unit
config against stock firmware so the device remains usable through
the rest of the MCU bring-up.

## References

- Pinout source-of-truth: `esphome/testcharger/NOTES.md` § "Nations
  N32G45x main MCU pinout" (and the `stock-mcu-2026-05-07.bin` SWD dump
  beside it; gitignored, sha256 in NOTES.md).
- Feasibility / effort estimate: `../../docs/ports/nexcyber-feasibility.md`
- OEM / SKU disambiguation: `../../BOARDS.md` § Nexcyber AC EVSE.
