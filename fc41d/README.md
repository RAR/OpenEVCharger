# OpenEVCharger FC41D firmware

ESPHome firmware for the FC41D Wi-Fi/BLE module on a Rippleon ROC001 (or
rebadge running the same hardware family). Pairs with the safety-core
firmware in [`../src/`](../src) running on the GD32F205VG MCU.

The MCU owns everything safety-critical (J1772, contactor, faults,
persistence, LED + buzzer + buttons) and exports its full state and a
small command surface over the binary TLV protocol from
[spec § 5](../docs/protocol.md). The FC41D's only job is to be a thin
network bridge: speak TLV downward to the MCU and Home Assistant /
OCPP / Wi-Fi / BLE upward. It can crash, reboot, or lose the LAN; the
MCU keeps charging or faults safely on its own.

## Layout

```
fc41d/
├── README.md                         # this file
├── openevcharger.yaml                     # ESPHome config (BK7231N target)
├── secrets.yaml.example              # template for wifi creds
├── boards/
│   ├── generic-bk7231n-qfn32-quectel.json   # partition-shifted variant
│   └── README.md                            # rationale + symlink step
└── components/
    └── openevcharger_tlv/                 # ESPHome external component
        ├── __init__.py               # main schema (poll_interval, uart)
        ├── sensor.py / binary_sensor.py / text_sensor.py
        ├── number.py                 # advertised_amps writable
        ├── button.py                 # stop / start / clear-fault / diagnostics
        ├── openevcharger_tlv.h            # transport + state cache
        └── openevcharger_tlv.cpp          # TLV parser/builder + entity publish
```

## Wiring

| BK7231N pin | MCU pin (GD32F205) | Direction        |
|-------------|---------------------|------------------|
| P11 (UART1 TX)   | PD2 (UART4 RX)  | BK → MCU         |
| P10 (UART1 RX)   | PC12 (UART4 TX) | MCU → BK         |
| GND              | GND             | bonded           |

The same UART that previously carried Quectel AT traffic between the
stock Rippleon firmware and FC41D now carries OpenEVCharger TLV. Same wires,
same baud (115200 8N1), different protocol.

## Build + flash

One-time setup of the partition-shifted board variant:

```sh
ln -sfn "$(pwd)/boards/generic-bk7231n-qfn32-quectel.json" \
        ~/.platformio/platforms/libretiny/boards/generic-bk7231n-qfn32-quectel.json
cp secrets.yaml.example secrets.yaml && $EDITOR secrets.yaml
```

### First serial flash — silence the MCU first

The MCU's UART4 (PC12 TX / PD2 RX) shares wires with the BK7231N's UART1
(P10 / P11), the same UART that ltchiptool drives during a serial flash.
With OpenEVCharger running, the MCU pushes a STATE_REPORT every 5 s plus
event traffic — that collides with the BK's bootloader handshake and
flashes hang or corrupt.

**DIP4 = "FC41D flash mode" strap.** Slide DIP4 to the GND ("ON")
position before powering up. At boot the MCU prints

```
MODE: FC41D flash (DIP4 held) — comms_task suppressed
fc41d-flash: VEN=1 CEN=1 — module released; press PC9 to pulse CEN for ltchiptool handshake
```

PC12/PD2 stay tri-stated, comms_task never starts, and a small helper
task takes over: it powers up the FC41D (VEN+CEN high) and watches
the internal **PC9** button. With the bus quiet, run the flash and
then tap PC9 to drop CEN — that lands the FC41D in bootloader-
handshake territory while ltchiptool is already listening:

```sh
esphome run openevcharger.yaml          # ltchiptool starts and waits for the chip
# in another window or after the prompt: tap PC9 once
```

The helper logs `fc41d-flash: PC9 pressed → CEN pulse` on each tap;
press again if the first handshake misses. After the flash completes,
slide DIP4 back to OPEN and power-cycle the unit. The MCU brings
comms_task up normally and the FC41D attaches to it on the next boot.

Subsequent updates go OTA over Wi-Fi from the same `esphome run`
command — DIP4 doesn't need to move again unless you're re-doing the
serial flash.

## Surfaced HA entities

| Domain          | Names                                                                              |
|-----------------|-------------------------------------------------------------------------------------|
| `sensor`        | CP High, Advertised Amps, Active Amps, Lifetime / Session Energy, fault/state codes |
| `binary_sensor` | MCU Link, Vehicle Connected, Charging, AC Present, Fault Active, Contactor Cmd      |
| `text_sensor`   | EVSE State, J1772 State, First Fault, MCU Build                                     |
| `number`        | Set Advertised Amps (6–48 A)                                                        |
| `button`        | Stop / Start / Clear All Faults / Ping / Refresh* / Buzzer Beep                     |

`Set Advertised Amps` writes `SET_ADVERTISED_AMPS`; the MCU clamps to its
DIP1 + hardware cap and persists to W25Q. The displayed value is
optimistic and refreshed by the next `STATE_REPORT`.

## Component design

`openevcharger_tlv` is a single ESPHome component with sub-platforms. The
parent class owns the UART, TLV stream parser, transmit framer, state
cache, and a 5-second `GET_STATE` poll. STATE_CHANGED, FAULT_RAISED,
SESSION_BEGAN/ENDED, BOOT_COMPLETE arrive unsolicited and trigger
re-publish or follow-up requests as appropriate. Sub-platform classes
register themselves with the parent and either receive setter calls
(sensors) or call back into the parent (number/button).

Frame layout matches `../src/proto/tlv.h` and the `openevcharger_state`
struct layout matches `../src/core/system_state.h` — keep the two in
sync if either changes.
