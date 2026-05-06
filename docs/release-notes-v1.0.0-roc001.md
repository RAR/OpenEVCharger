# OpenEVCharger v1.0.0-roc001 — release notes (DRAFT)

**Date:** 2026-05-06 (anticipated)
**Target:** Rippleon ROC001 (GD32F205VG)
**Tag:** `v1.0.0-roc001` (cut once F1 BL0939 calibration is in)

> Draft prepped 2026-05-05 evening. Tag and ship after the morning's
> bench session validates F1 (BL0939 IA/IB/PA scales) and a smoke-test
> reflash on the renamed firmware. Edit / trim before publishing.

## What this is

A clean-room replacement firmware for the Rippleon ROC001 EVSE
(OEM: New Energy CS; the Nexcyber NECS-ACW family is a separate
product line, not in scope for this release).
GD32F205VG-based safety core in C; FreeRTOS task layout; FC41D
Wi-Fi/BLE/cloud module on UART4 driven by ESPHome + MicroOcpp;
OCPP 1.6-J + Home Assistant integration via TLV.

Stock firmware is V1.0.066. This is a from-scratch rewrite, not a
fork — see `docs/mcu-re/` for the reverse-engineering trail.

## Highlights

### Safety core

- **Spec §4 detectors all wired.** GFCI, PE continuity, CP=E sustained,
  diode check (gated), CC out-of-range (gated), AC absent, over-temp
  (hottest-of-two NTC), runtime ADC out-of-range, hard / soft over-current
  (cal-gated), relay weld / stuck-open (cal-gated), CP regression (event-only).
- **GFCI live-validated** with current injection on PE → external module
  trip → PE2 LOW → FAULT_GFCI in 3 ticks (~60 ms) → contactor force-open.
- **EVSE state machine** (BOOT → SELF_TEST → READY → CHARGING /
  USER_PAUSED / COOLING_DOWN → FAULT) bench-validated through full
  B↔C↔A graph with the EVSE tester, including session-record persistence
  and clean unwind on both C→B and C→A.

### Persistence

- W25Q SPI NOR (GigaDevice GD25Q64, 8 MB) hosts boot count,
  boot config, calibration, RFID authlist, event log, session log,
  crash state — all in ping-pong wear-levelled regions.
- Crash-loop safe-fail with fast-restart counting.

### OTA

- **FC41D-mediated TLV chunked-upload OTA**, default ON in production
  builds. Pipeline: HA `/config/www/` → `openevcharger_fetch_and_push_ota`
  service → CMD_OTA_BEGIN/CHUNK/COMMIT (TLV) → MCU stages to W25Q +
  CRC verify → boot_config.pending_ota_flag → MCU reboot →
  pre-scheduler `flash_apply_pending_ota_image()` runs from
  `.ramfunc`, erases bank0 + word-programs + SYSRESETREQ from RAM.
  Self-rollback on CRC mismatch.
- 64 KB image cap (current image ~51 KB; raise via stack-buffer bump
  in the apply orchestrator).
- HTTP-only — LibreTiny BK7231N's mbedtls is missing
  `mbedtls_net_set_nonblock`. Hit HA over LAN HTTP, not HTTPS.

### Wall-clock

- **On-chip RTC bridge.** LSI-clocked GD32 RTC counter holds unix
  epoch seconds, magic in BKP_DATA0+1 marks "set this VDD cycle".
  Survives any non-power-cycle reset (NRST / SYSRESETREQ / watchdog /
  OTA RAM-side reset / brown-out-recover). Cold boot still requires
  HA to push time once (VBAT pin not wired to a battery on this PCB).
- HA `time:` component pushes seed via TLV on
  `api.on_client_connected`, `time.on_time_sync`, `time.on_time '/30 min'`
  cron, and `binary_sensor.link_up.on_press`. Rate-limited at 2 s gap
  on the FC41D side.

### Logging

- Every printk line carries a fixed-width timestamp prefix:
  `[ssss.mmm]` uptime pre-time-sync, `[hh:mm:ss.mmm]` wall-clock once
  HA's pushed.
- Subsystem tags lowercased and unified to `subsystem: ...` form
  across all 200+ call sites.

### OCPP / HA

- OCPP 1.6-J via MicroOcpp on the FC41D side.
- StartTransaction / StopTransaction round-trip the MCU's TLV surface
  (CSMS Accept relays to MCU `request_start_resume` / `request_stop`).
- RFID authlist on W25Q drives offline auth; OCPP layers idTag
  verification on top via `EVT_RFID_AUTH_RESULT` automation.
- MeterValuesSampledData wires Voltage, Current, Power, Energy plus
  Current.Offered (auto-computes Power.Offered from voltage).
- HA diagnostic surface: ~30 sensors / ~10 buttons / ~5 numbers /
  ~5 switches / ~3 binary sensors. "Restart MCU", "Abort MCU OTA",
  "Push BL0939 Calibration", "MCU OTA Self-Test", and the OTA
  Progress sensor cover the bench-bringup loop.

## Hardware required

| Component | Notes |
|---|---|
| Rippleon ROC001 (or rebadge) | Single-phase 6–48 A EVSE PCB |
| ST-Link V2 + 4-pin SWD wiring | One-time MCU flash |
| WCH CH343G USB-UART | FC41D bootloader flash |
| Raspberry Pi or Linux host running HA | OCPP CSMS optional |

See `docs/bring-up.md` for the full bench procedure.

## Known limitations

- Bench-validated on a single PCB. Other STM32F2-class hardware needs a
  port (see `BOARDS.md`).
- Cold-boot wall clock requires an HA push (no battery-backed RTC on this
  PCB). Warm reboots within the same VDD cycle are clean.
- HTTPS OTA not supported (LibreTiny mbedtls limitation); LAN HTTP only.
- Stack high-water sizing right-sized at idle; revisit under real charge
  load (see open `E10`).

## Bench-gated for v1.x

Cal items still TODO before the firmware is "done" rather than "shipped":

- **F1** — BL0939 IA / IB / PA calibration with a current-pull plug.
  Unblocks RELAY_WELD / RELAY_STUCK_OPEN / HARD_OC / SOFT_OC detectors
  + session_mwh / lifetime_mwh accumulation.
- **F2** — 5-point CP negative-half fit → unblocks `FAULT_DIODE_CHECK`.
- **F3** — GFCI 8-state CAL self-test scope-validation (live GFCI
  already validated).
- **F5** — full charging session with a real EV (M10).
- **F6** — CC ladder bench characterisation → lift the
  `OPENEVCHARGER_CC_DETECTOR` build flag.
- **F7** — Relay actuate-readback under live AC mains.
- **F10** — PE continuity sense topology characterisation.

## License

GPL-3.0. The firmware is a clean-room rewrite of OpenEVSE concepts
adapted to GD32F2 hardware; no upstream OpenEVSE code is incorporated.

## Acknowledgements

- OpenEVSE project for the J1772 / state-machine model.
- New Energy CS (Rippleon ROC OEM) for shipping reasonable hardware
  that's reverse-engineerable.
- evcc, MicroOcpp, ESPHome, LibreTiny upstream maintainers.

---

**Tag command** (run in OpenEVCharger submodule once you're happy):

```bash
git tag -a v1.0.0-roc001 -m "OpenEVCharger v1.0.0 — Rippleon ROC001"
git push origin v1.0.0-roc001
```
