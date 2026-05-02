# OpenBHZD

Replacement firmware for the **Rippleon ROC001** / **NewEnergyCS ROC-family**
J1772 EV charger, targeting the GigaDevice **GD32F205V** main MCU.

OpenBHZD is a clean-room reimplementation of OpenEVSE-style EVSE firmware.
The behavior of the J1772 state machine, fault model, and self-test sequence
is modeled on [OpenEVSE](https://github.com/OpenEVSE/open_evse) but no source
is copied. Wi-Fi/BLE/cloud features run on the FC41D Wi-Fi module, off the
safety MCU, controlled via a binary TLV protocol over UART5.

**Status:** v1 in development. See [`docs/superpowers/specs/`](docs/superpowers/specs/)
for the design spec and [`docs/superpowers/plans/`](docs/superpowers/plans/)
for the milestone-by-milestone implementation plan.

## License

GPL-3.0-only. See `LICENSE`.

## Quickstart (bench)

```sh
# 1. Install host deps
sudo apt install gcc-arm-none-eabi cmake openocd

# 2. Fetch the GD32F20x vendor library (see third_party/GD32F20x_Firmware_Library/README.md)

# 3. Configure and build
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake
cmake --build build

# 4. Back up stock firmware (REQUIRED before any flash)
./tools/stock_backup.sh

# 5. Flash
./tools/flash.sh
```
