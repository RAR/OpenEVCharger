# Eluminocity CH-21130 (Delta EVMU30) — Companion MQTT Bridge

**Date:** 2026-05-14
**Board slug:** `eluminocity-ch21130`
**Status:** design approved, pending spec review

## Summary

A read-only Linux-userland bridge daemon for the Delta EVMU30 / Eluminocity
CH-21130 AC EV charger. It attaches to the stock firmware's SysV shared-memory
segment, normalizes charger state, and publishes it to Home Assistant over MQTT
(with HA discovery). The stock Delta firmware — including the STM32F334 safety
MCU — is left running and untouched; the bridge is a pure observer added
alongside it.

This is the **companion-only** deliverable for the `eluminocity-ch21130` board.
There is no MCU clean-room port — the STM32F334 stays a trusted black box. A
clean-room MCU port may come later and is explicitly out of scope here.

## Context & motivation

The unit is a factory-virgin Delta AC Mini (model `EVMU3015`, Eluminocity SKU
`CH-21130`), never tethered to a CSMS. Unlike the sibling Rippleon/Nexcyber
projects there is no vendor cloud to escape — the goal is positive: make the
charger a first-class Home Assistant citizen without replacing the (working,
safety-critical) Delta firmware.

The SPEAr320 Linux side is fully reverse-engineered: root shell on the serial
console, daemon binaries with debug symbols, decoded `Pri_Comm` ⇄ STM32
protocol, and a decoded shared-memory offset map. The RE artifacts live in
`esphome/testcharger/delta/` (`dump/`, `docs/`, `bench/`, `decode_sharemem.py`).

### Decisions locked during brainstorming

| Question | Decision |
|---|---|
| End goal | De-cloud / HA-native; keep stock firmware |
| MCU (STM32F334) | Trusted black box — untouched, revisit only on real need |
| Invasiveness | Add a bridge process; do not replace Delta daemons |
| Transport | MQTT + HA discovery now; OCPP 1.6-J designed-for but **deferred** to a later spec |
| Language/runtime | C, static musl (rootfs is pure busybox+ash — no interpreters) |
| v1 scope | **Read-only** — publish state, no shmem writes |
| Internal architecture | Layered modules with a northbound adapter seam |
| Location | OpenEVCharger repo, `boards/eluminocity-ch21130/companion/` |

## Architecture

### Modules

Strictly layered, no cycles:
`shmem` ← `charger_state` ← `northbound` ← `mqtt_adapter` → `mqtt_client`.
Every layer is host-testable in isolation.

| Module | Responsibility | Depends on |
|---|---|---|
| `shmem` | Attach to Delta's SysV segment (`shmget(0x153E, 0x40000, 0)` + `shmat`, **read-only**). Typed read accessors for the offsets we consume. No write accessors exist in v1. | — |
| `charger_state` | `struct charger_state` — normalized, scaled values. `charger_state_read()` pulls from `shmem` and applies scaling. Change-detection so only deltas are published. | `shmem` |
| `northbound` | Thin adapter interface (`init` / `publish_state` / `tick` / `shutdown`). This is the seam where OCPP 1.6-J slots in later as a second implementation. | `charger_state` |
| `mqtt_adapter` | Implements `northbound`: HA discovery configs, per-field `PUBLISH`, Last-Will. | `northbound`, `mqtt_client` |
| `mqtt_client` | Minimal MQTT 3.1.1 over plain TCP: `CONNECT` / `PUBLISH` / `PINGREQ` / `DISCONNECT` (+ a `SUBSCRIBE` stub for v1.1). Zero dependencies. | — |

`main.c` ties it together: config-file parse, init, poll loop, signal handling.
Config parsing is small enough to live in `main.c` for v1.

### Repo layout

```
OpenEVCharger/boards/eluminocity-ch21130/
├── README.md          # "companion-only board — MCU clean-room port deferred"
├── companion/
│   ├── Makefile        # musl-cross build — standalone, NOT the CMake board matrix
│   ├── src/            # shmem, charger_state, northbound, mqtt_adapter, mqtt_client, main
│   ├── test/           # host unit tests + fixtures/ (committed shmem snapshot)
│   └── image/          # DcoFImage builder + dropbear integration
└── docs/              # the Delta RE docs the bridge depends on (migrated in)
```

- The companion build is **independent** of OpenEVCharger's top-level CMake board
  matrix (that machinery is for MCU firmware). `boards/eluminocity-ch21130/` has
  no `board.cmake` / `pin_map.h` / linker — there is no MCU target.
- RE **docs** (`01-Pri_Comm-protocol.md` and friends) migrate into
  `boards/eluminocity-ch21130/docs/` so the board is self-documenting. The raw
  32 MB firmware **dumps** and bench RE scripts stay in `esphome/testcharger/delta/`
  as RE evidence; the bridge's test suite carries a small *committed* shmem
  snapshot fixture under `companion/test/fixtures/`.

### On-device paths

| Path | Notes |
|---|---|
| `/root/delta-bridge` | The binary — in the rootfs, so a `DcoFImage` carries it; `/root` is jffs2-rw, so the dev loop writes the same path directly. |
| `/Storage/delta-bridge.conf` | Config — on mtd3, **survives a rootfs re-flash**, user-editable. |
| `/Storage/delta-bridge.log` | Size-capped log. |
| `/etc/funs` | One appended line for autostart. |

## Data flow

### Poll loop

`main` runs a single-threaded loop at a configurable interval (default 1 Hz):

```
shmem_refresh() → charger_state_read() → diff vs previous → northbound->publish_state(dirty fields)
```

MQTT keepalive (`PINGREQ`) and reconnect are time-sliced on the same loop — no
threads, no locking.

### Read-only v1 entity set

Published as individual HA entities under one device, "Delta EVMU30 (`<id>`)":

| Entity | Source (shmem) | HA type |
|---|---|---|
| Voltage (Vrms) | `+0x00` | sensor, V |
| Current (Irms) | `+0x04` | sensor, A |
| Power | metering `0xa69–0xa79` | sensor, W |
| Session energy | metering region | sensor, Wh |
| EVSE state | `+0xa08` (decoded enum) | sensor (idle/connected/charging/fault) |
| Pilot voltage | `+0x1d0/+0x1d4` | sensor, mV |
| STM32 link | `+0xa0b` (comm health) | binary_sensor (connectivity) |
| Active faults | trap-alarm bitmap `0x138–0x157` | text sensor — decoded 31-alarm catalog, lists active |

**Scaling caveat:** exact scale factors — and whether the per-unit `Gain`
calibration (Vgain/Igain/Wgain) must be applied — are best-effort from static
RE. All scaling is isolated in `charger_state` so it is a one-file fix once
bench-validated against a multimeter / known load (milestone M1). v1 ships the
RE values plus an explicit "verify on bench" task.

### MQTT topic layout

- State: `delta-bridge/<id>/<field>` (retained), per-field — pairs naturally
  with delta-publishing.
- Availability: `delta-bridge/<id>/availability` — set as the MQTT **Last Will**,
  so an unexpected bridge death marks every entity unavailable in HA.
- Discovery: retained configs to `homeassistant/<component>/delta_<id>_<field>/config`,
  re-published on each (re)connect.

### Config — `/Storage/delta-bridge.conf`

Plain `key = value`, parsed in `main.c`:

```
broker_host, broker_port, broker_user, broker_pass
topic_prefix   (default "delta-bridge")
device_id      (default: unit serial from shmem)
poll_hz        (default 1)
log_level
```

## Safety properties

This is an EVSE; the bridge must be incapable of affecting the charging path.

- **Read-only attach.** v1 does `shmat` and only ever reads. It cannot corrupt
  Delta's shmem or perturb `Pri_Comm` / the STM32 handshake / the safety loop.
  This is structural: `shmem` exposes no write accessors in v1.
- **Never creates the segment.** `shmget` is called without `IPC_CREAT`. If the
  segment is not present yet, the bridge waits — it never risks creating a
  wrong-sized empty segment that Delta's daemons would then attach.
- **Crash isolation.** If the bridge hangs or crashes, Delta firmware is wholly
  unaffected — it is a pure observer, in nobody's critical path. Worst-case
  failure mode: HA entities go unavailable (via LWT); the charger keeps charging.
- **No watchdog interaction.** The bridge never touches `/dev/watchdog`.
- **Flat footprint.** Single-threaded, fixed-size buffers, no unbounded
  allocation — it shares a 128 MB box with Delta's daemons.

## Error handling

| Condition | Response |
|---|---|
| shmem segment not present (raced `main` at boot) | Retry with backoff, do not exit. |
| MQTT broker unreachable / mid-session disconnect | Retry with backoff; keep polling shmem meanwhile (just do not publish). On reconnect, re-publish discovery + a *full* state snapshot. |
| MQTT socket I/O | Bounded timeouts (short `SO_RCVTIMEO`/`SO_SNDTIMEO`) so a dead broker cannot stall the poll loop. |
| Unexpected shmem values (enum out of range, etc.) | Defensive accessors — publish `"unknown"`, never crash. |
| `SIGTERM` / `SIGINT` | Graceful: publish `availability=offline`, `shmdt`, exit 0. |

## Logging

To `/Storage/delta-bridge.log`, size-capped (truncate at a configurable limit —
`/Storage` is a 13 MB partition). Level set by `log_level`. No syslog dependency.

## Deployment

### Two-tier

- **Dev loop:** cross-compiled binary onto `/root/delta-bridge` via the serial
  `printf_transfer.py` path (or USB `cp`); autostart via `/etc/funs`. Fast iterate.
- **Distribution:** a `DcoFImage` builder produces a USB-flashable full-rootfs
  image. End-user experience: drop `DcoFImage` on a FAT32 stick, plug in,
  power-cycle.

### `DcoFImage` builder — `companion/image/`

1. Take stock `dump/rootfs-unpacked/` as base.
2. Inject `/root/delta-bridge`, the dropbear binary, and autostart lines in
   `/etc/funs`.
3. Repack JFFS2 at **mtd5 geometry** (erase-block size / endianness must match;
   `dump/mtd5-rootfs.bin` is the reference).
4. Append `DELTADCOF` magic + BE-u32 bytesum → `DcoFImage`.
5. Also emit **`DcoFImage-stock-restore`** — untouched stock rootfs, same
   wrapper — so users can revert to pure Delta firmware.

The builder is trusted only once `DcoFImage-stock-restore` flashes and boots
identically — i.e. the JFFS2-geometry and USB-`UpdateCSU` risks are retired with
our code *out* of the picture (milestone M2).

> Note: our unit's `UpdateCSU()` expects the legacy `DELTADCOF` 9-byte ASCII
> magic trailer. The newer AC Mini Plus (V02.0B.06) uses a different,
> model-string-keyed trailer — we mimic *our* unit's format, not the Plus's.

### dropbear

Lift the binary from the V02.0B.06 rootfs (same SoC / kernel / STLinux-toolchain
era — bench-test it runs; cross-compile only if it does not). Key-only auth.
Host keys generated at first boot onto `/Storage` (survive reflash).
`authorized_keys` supplied to the image-builder via a `--authorized-key` arg.
dropbear is a component of the `DcoFImage` builder only — the bridge core does
not know it exists; the dev loop does not use it.

## Testing

### Host (CI + local)

- `shmem` — against the committed `test/fixtures/` shmem snapshot.
- `charger_state` — known bytes in, assert scaled values + change-detection.
- `mqtt_client` — assert MQTT 3.1.1 wire bytes; optional integration test vs a
  local mosquitto.
- `mqtt_adapter` — assert discovery payloads + topic layout with a mock client.
- Cross-compile gate — musl armv5te `make` builds + links static-clean.

### Bench milestones (hardware, manual)

| Milestone | Goal |
|---|---|
| M0 | Serial-deploy binary to `/root`, run, confirm shmem attach + entities in HA. |
| M1 | Validate scaling vs multimeter / known load; tune `charger_state`. |
| M2 | Flash `DcoFImage-stock-restore` via USB; confirm identical boot (validates builder + `UpdateCSU` path). |
| M3 | Flash full `DcoFImage` via USB; confirm cold-flash autostart + SSH + HA. |

### CI

A new `eluminocity-companion` GitHub workflow (host tests + cross-compile),
alongside the existing `fc41d-config.yml`.

## Out of scope (YAGNI)

- Shmem writes / charger control — read-write v1.1, needs a real EV on the bench.
- OCPP 1.6-J adapter — the `northbound` interface leaves the seam; the adapter
  itself is a later spec. (The V02.0B.06 AC Mini Plus ships `OCPP16J` /
  `OCPP16JCtrl` binaries built for this exact SoC — a useful reference impl when
  that work starts.)
- MQTT TLS — local broker assumed for v1.
- `SUBSCRIBE` handling beyond a stub.
- STM32F334 firmware — untouched.

## Related follow-up (separate task, not this spec)

During the multi-MCU repo reorg, the Rippleon `fc41d/` tree was left at the repo
root; it is board-specific and should move to `boards/rippleon-roc001/fc41d/`.
Tracked separately from this spec.
