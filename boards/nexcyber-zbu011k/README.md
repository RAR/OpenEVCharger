# OpenEVCharger — Nexcyber board port (in progress)

**Status:** N32G45x HAL ported and building. Two targets available
(see Build below). The bench-harness target (`openevcharger-nexcyber-bringup`)
is the image actually flashed during bring-up. The production target
(`openevcharger`) compiles and links against `src/main.c` but is a
compile/link gate — the N32 HAL uses `OEVC_HAL_STUB()` for several
peripherals and is not yet functional on hardware.

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

## What's here

| File | Purpose |
|---|---|
| `board.cmake` | CMake board definition: N32G45x SDK paths, Cortex-M4F flags, N32 HAL source lists, linker script wiring, and both target definitions (`openevcharger` + `openevcharger-nexcyber-bringup`) |
| `pin_map.h` | All confirmed pins from the 2026-05-07 SWD firmware dump + bench wiggle sessions. 27 of ~36 pins confirmed; 7 ULN2003 outputs + 3 candidate digital inputs still need bench resolution (EV-simulator pass through state-B/C) |
| `n32g45x.ld` | Linker: 120 KB FLASH + 8 KB PERSIST + 80 KB RAM. Assumes 2 KB sector geometry (per N32G45x reference manual) — bench-confirm before first flash erase |
| `bench/bringup_main.c` | Entry point for the M0-M4 bench-harness target. Clock + UART + GPIO HAL up, then `vTaskStartScheduler()` with a 1 Hz heartbeat task. The image actually flashed to the bench unit |

The N32G45x HAL implementations live in `src/hal/n32g45x/` (shared
across both targets). Board-unique peripherals (Nextion display, LED
ring, SPI2) have their own headers there (`nextion.h`, `led_ring.h`,
`spi2.h`). Peripherals whose API diverges between boards carry
board-specific `*_nx.h` headers (`adc_scan_nx.h`, `gfci_nx.h`,
`relay_nx.h`).

## Build

```bash
# Bench-harness target (the image flashed during bring-up):
cmake -S . -B build/nexcyber-zbu011k -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain-cm4f.cmake \
    -DOPENEVCHARGER_BOARD=nexcyber-zbu011k
cmake --build build/nexcyber-zbu011k --target openevcharger-nexcyber-bringup

# Production target (compile-gate only — not yet functional on hardware):
cmake --build build/nexcyber-zbu011k --target openevcharger
```

Output in `build/nexcyber-zbu011k/`. The Rippleon target
(`OPENEVCHARGER_BOARD=rippleon-roc001`, default) is unaffected and
continues to build with the Cortex-M3 toolchain file.

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
| GPIO HAL | ✅ M2 — all 16 confirmed pins; 7 TBD OUT_PP pins left at reset default until bench |
| UART (log) | ✅ M0 — USART1 on PA9/PA10 |
| ADC HAL | not started — M3 |
| Timer HAL (CP PWM) | not started — M3 (PA8 pad already AF_PP) |
| Clock tree (RCC) | ✅ M0 — SDK default HSE_PLL 144 MHz |
| FreeRTOS port (`portable/GCC/ARM_CM4F`) | ✅ M1 — scheduler bring-up + heartbeat task |
| Nextion USART2 driver | not started — M3 |
| BL0939 driver (SPI vs UART) | not started — bench-blocked on variant ID |
| Persistence ping-pong | not started — redesign for internal-flash (this file's PERSIST region) |
| OTA staging path | not started — redesign single-bank, CRC-pre-verify, atomic from RAM, no self-rollback |
| Safety core port | not started — pulls in src/core/, src/persist/, src/tasks/. Becomes possible once GPIO + ADC + timer + persist HALs are up |
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
