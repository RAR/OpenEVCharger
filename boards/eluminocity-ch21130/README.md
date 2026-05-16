# Eluminocity CH-21130 (Delta EVMU30) — companion-only board

This board is **companion-only**. There is no MCU clean-room firmware port —
the unit's STM32F334 safety MCU is left running its stock Delta firmware and
treated as a trusted black box. Accordingly this directory has **no**
`board.cmake` / `pin_map.h` / linker script, and is not part of the top-level
CMake board matrix.

## Layout

- `companion/` — `delta-bridge`, a Linux-userland daemon that runs on the
  unit's SPEAr320 application processor. Attaches to the stock firmware's
  SysV shared-memory segment, publishes charger state to Home Assistant over
  MQTT, and (when `write_enable=true`) exposes three HA write entities:
  `number.set_current`, `switch.authorize`, `button.clear_faults`. Built
  with its own `Makefile` (musl armv5te cross-compile), not CMake.
- `docs/` — the reverse-engineering docs the bridge codes against. `06-…`
  is the verified shmem layout (single source of truth for offsets); `07-…`
  is the persistence analysis covering when external writes survive.

## Bridge entity set + write semantics

Read-only (always published):

| Entity | Topic | Notes |
|---|---|---|
| `voltage` / `current` / `power` | sensor | Measured RMS V, I, W from MeterIC_new |
| `pilot_state` | sensor enum | J1772 A/B/C/D/transient/F from Adc:PilotState |
| `pilot_duty` | sensor % | Computed pilot PWM duty (`rated × 1.667`) |
| `pri_state`, `user_state`, `red_led` | sensor | Stock state-machine outputs (raw u8) |
| `stm32_link_ok` | binary_sensor | Inter-MCU UART health |
| `active_faults` | sensor | Comma-joined names of bits set in alarm bitmap |

Writable (only when `write_enable = true` in `delta-bridge.conf`):

| Entity | Writes to | Persistence behaviour |
|---|---|---|
| `number.set_current` (6–30 A) | `OFF_RATED_AMPS` (0xa24) | **Sticks at runtime** — no stock-running daemon periodically rewrites this byte. **Lost on reboot** — `main:main`'s boot init unconditionally writes 30 there. evcc / HA dashboards can simply re-set on every connection. |
| `switch.authorize` (ON/OFF) | `OFF_USER_STATE` (0xa00) | **Loses the race** — `Charging_Standard_RFID` rewrites this byte every loop iteration from the pilot-state classifier. The unit's default Authentication Mode is `0` (no-auth), so plug-in alone triggers charging; this switch is effectively a no-op control on stock setups. Kept in v0.3 for diagnostic exploration; persistent control via this byte needs a different RE path (mini OCPP server, or RFID UID injection at 0xa68..0xa6f). |
| `button.clear_faults` | `OFF_ALARM_BITMAP` (0xa74) = 0 | **Diagnostic only** — `Pri_Comm` re-asserts active fault bits on its next poll if the physical fault persists. Useful for confirming whether a fault is latched-stale or genuinely active. |

For evcc / solar-load-balancing the only entity that matters is
`number.set_current`, and that one works correctly for runtime use. Reboot
survival for setpoint values is a v1.1 stretch goal (it would need the
bridge to also rewrite `/dev/mtdblock4` with the right byte-sum checksums
— see `docs/07-persistence-paths.md` for the recipe).

## On-device web UI (v0.4+)

When `web_enable = true` in `delta-bridge.conf`, the bridge also serves a
single-page web UI on `web_port` (default 8080). The same status fields
as the MQTT topics, plus the three writable controls (gated by
`write_enable`), plus a bound-to-`/Storage/delta-bridge.conf` config
editor with a "restart bridge now" button. Auth is HTTP Basic against
`web_user`/`web_pass`; if either is empty, auth is disabled and the
bridge logs a one-shot warning at startup (useful for first-boot setup
on an isolated bench network).

**The web UI is HTTP-only and LAN-bound by design.** No TLS, no auth
cookies, no rate limit. Keep it off any network you don't trust; if you
need remote access, tunnel it (Wireguard / Tailscale) rather than
exposing port 8080.

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
