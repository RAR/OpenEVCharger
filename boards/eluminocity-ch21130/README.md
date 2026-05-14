# Eluminocity CH-21130 (Delta EVMU30) — companion-only board

This board is **companion-only**. There is no MCU clean-room firmware port —
the unit's STM32F334 safety MCU is left running its stock Delta firmware and
treated as a trusted black box. Accordingly this directory has **no**
`board.cmake` / `pin_map.h` / linker script, and is not part of the top-level
CMake board matrix.

## Layout

- `companion/` — `delta-bridge`, a read-only Linux-userland daemon that runs on
  the unit's SPEAr320 application processor. It attaches to the stock firmware's
  SysV shared-memory segment and publishes charger state to Home Assistant over
  MQTT. Built with its own `Makefile` (musl armv5te cross-compile), not CMake.
- `docs/` — the reverse-engineering docs the bridge codes against.

## Toolchain

`companion/Makefile` cross-compiles with a musl armv5te toolchain. Provision it
and put its `bin/` on `PATH` (the prefix `armv5l-linux-musleabi-` is the
Makefile default; override with `make CROSS=...`):

    # musl.cc prebuilt toolchain
    curl -LO https://musl.cc/armv5l-linux-musleabi-cross.tgz
    tar xzf armv5l-linux-musleabi-cross.tgz
    export PATH="$PWD/armv5l-linux-musleabi-cross/bin:$PATH"

Host unit tests use the system `cc` and need no cross toolchain.

## Design

See `../../docs/superpowers/specs/2026-05-14-eluminocity-ch21130-mqtt-bridge-design.md`.
