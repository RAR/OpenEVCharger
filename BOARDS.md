# Supported Boards

OpenEVCharger v1 targets STM32F2-compatible Cortex-M3 EVSE controllers. The
core (FreeRTOS task layout, J1772 state machine, OCPP/TLV protocol, OTA, RTC
bridge, persistence) is board-independent. The board-specific surface lives
in:

- `src/core/pin_map.h` — GPIO pin → function map.
- `linker/*.ld` — flash + RAM layout.
- `CMakeLists.txt` — vendor SPL paths, MCU flags, clock config.

Adding a board means producing those three artefacts; everything in `src/`
above the HAL should compile unchanged.

## Bench-validated

| Board | MCU | Status | Notes |
|---|---|---|---|
| **Rippleon ROC001** (FCC `ROCHL2US`) | GD32F205VG | ✅ Bench-validated through M7 + GFCI live, OTA, RTC bridge | Single-phase 6–48 A, BL0939 metering, FC41D Wi-Fi/BLE module on UART4. Pin map fully reverse-engineered from stock V1.0.066. US brand: Rippleon (FCC entity Rippleon Ltd., FCC ID `2BLBS-ROCHL2US`); ODM: Beizide / NewEnergyCS (CN — `beizide.com` / `newenergycs.com` are the same parent factory). Cloud-side SKU range ROC002–010 likely close siblings but bench-unverified. |

## Wishlist / open ports

- **Nexcyber NECS-ACW family.** Different brand, different MCU
  (Nations N32G45x), different Wi-Fi module (Tuya WBR2 / RTL8720CF
  AmebaZ2), and a different cloud architecture — Nexcyber is a stock
  Tuya device (Tuya cloud directly via the WBR2), whereas Rippleon
  runs a custom OCPP/REST backend behind the FC41D. Not
  interchangeable with Rippleon ROC. Hardware on hand; port not
  started.

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
