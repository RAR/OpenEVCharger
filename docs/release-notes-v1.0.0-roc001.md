# OpenEVCharger v1.0.0-roc001 — release notes (DRAFT)

**Date:** 2026-05-07 (anticipated)
**Target:** Rippleon ROC001 (GD32F205VG)
**Tag:** `v1.0.0-roc001` (cut after F5 — full charging session against
a real 240 V EV — passes against the bench-validated unit)

> Draft updated 2026-05-06 evening: F1 BL0939 calibration landed,
> WELD / STUCK_OPEN / HARD_OC / SOFT_OC / F3 GFCI-CAL all
> bench-validated, F7 closed as N/A on this hardware revision.
> One bench item between draft and tag: F5.

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

### Safety core — every spec § 4 detector either live or documented N/A

- **GFCI** — live + bench-validated 2026-05-05 (current injection on
  PE → ext module trip → PE2 LOW → fault raised in ~60 ms → contactor
  force-opened).
- **GFCI CAL self-test** — live + bench-validated 2026-05-06 (boot-time
  PE3 pulse + PE2 round-trip; tuned for this PCB's slow GFCI module
  with 500 ms pulse + 1000 ms recover).
- **Relay weld / stuck-open** — live + bench-validated 2026-05-06.
  BL0939 IA-based: 500 mA threshold, 3.2 s post-open settle window
  for load-cap discharge (bench-observed up to 1051 mA for ~1 s
  after a 6 A session ends).
- **Hard over-current** — live + bench-validated 2026-05-06.
  Spec-compliant: advertised × 1.20 sustained > 5 s, latched, halts
  charging. min(spec, hw_ceiling 60 A) preserves the absolute hardware
  backstop for misconfig defense.
- **Soft over-current** — live + bench-validated 2026-05-06.
  Spec-correct: 1.05× advertised for 30 s → ramp duty −10 %, repeat to
  the J1772 6 A floor, raise (self-clearing) only when at floor and
  still over. UI alert (red flash + buzzer) suppressed via
  `FAULT_LATCHED_MASK` since SOFT_OC is informational — log + raise,
  no halt.
- **CP=E sustained, AC absent, over-temp, runtime ADC out-of-range,
  CP regression** — all live.
- **PE continuity** — deferred to v1.1 hardware revision. Install-
  side characterisation 2026-05-07 found PC5 is mains-current-coupled,
  not a clean PE-bonded voltage divider — raw ≈ 1 idle, 500–700 while
  charging, so "raw > 400" cannot distinguish "PE broken" from
  "charging with PE intact." Real PE safety in v1.0.0 is via
  `FAULT_GFCI` (stray earth-current trip, live + bench-validated).
- **Boot self-test** — ADC sanity / relay-open / CP pilot floor /
  GFCI CAL all run on every boot. Failure → FAULT_BOOT_SELF_TEST or
  FAULT_GFCI_SELF_TEST, EVSE → FAULT.
- **Diode check** — deferred to v1.1 hardware revision (bipolar CP
  read-back daughterboard). Stock V1.0.066 firmware also skips diode
  check on this PCB. See `docs/diode-check-investigation.md`.
- **CC out-of-range** — decoder live; raise gated behind
  `OPENEVCHARGER_CC_DETECTOR=1` until F6 bench cal of OEM CC divider
  topology completes.
- **Relay actuate-readback boot test** — removed 2026-05-06 as N/A
  on this hardware revision. PB12 is the UL2231 force-open *latch*
  (driving PB12 HIGH while PE12 HIGH forces the contactor open via
  hardware), not a closed-feedback sense pin. No closed-feedback
  pin exists. Runtime BL0939 WELD/STUCK_OPEN cover the same fault
  modes during real sessions.

### EVSE state machine

BOOT → SELF_TEST → READY → CHARGING / USER_PAUSED / COOLING_DOWN
→ FAULT, bench-validated 2026-05-05 through the full B↔C↔A graph
with the EVSE tester, including session-record persistence and
clean unwind on both C→B and C→A. COOLING_DOWN entered from
over-temp trip, exited to READY on hysteresis-low clear.

### Persistence

W25Q SPI NOR (GigaDevice GD25Q64, 8 MB) hosts boot count, boot
config, calibration, RFID authlist, event log, session log, crash
state — all in ping-pong wear-levelled regions. Crash-loop
safe-fail with fast-restart counting.

### OTA

- **FC41D-mediated TLV chunked-upload OTA**, default ON in production
  builds. Pipeline: HA `/config/www/` → `openevcharger_fetch_and_push_ota`
  service → CMD_OTA_BEGIN/CHUNK/COMMIT (TLV) → MCU stages to W25Q +
  CRC verify → boot_config.pending_ota_flag → MCU reboot →
  pre-scheduler `flash_apply_pending_ota_image()` runs from
  `.ramfunc`, erases bank0 + word-programs + SYSRESETREQ from RAM.
  Self-rollback on CRC mismatch.
- 64 KB image cap (current image ~52 KB; raise via stack-buffer bump
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
- HA diagnostic surface: ~30 sensors / ~12 buttons / ~5 numbers /
  ~5 switches / ~3 binary sensors. "Restart MCU", "Abort MCU OTA",
  "Push BL0939 Calibration", "MCU OTA Self-Test", "Simulate Replug",
  "Run GFCI CAL Self-Test", and the OTA Progress sensor cover the
  bench-bringup loop.

## Hardware required

| Component | Notes |
|---|---|
| Rippleon ROC001 (or rebadge) | Single-phase 6–48 A EVSE PCB |
| ST-Link V2 + 4-pin SWD wiring | One-time MCU flash |
| WCH CH343G USB-UART | FC41D bootloader flash |
| Raspberry Pi or Linux host running HA | OCPP CSMS optional |

See `docs/bring-up.md` for the full bench procedure.

## Known limitations

- Bench-validated on a single PCB. Other GD32F2-class hardware needs a
  port (see `BOARDS.md`).
- Cold-boot wall clock requires an HA push (no battery-backed RTC on this
  PCB). Warm reboots within the same VDD cycle are clean.
- HTTPS OTA not supported (LibreTiny mbedtls limitation); LAN HTTP only.
- BL0939 calibration is per-chassis. The OEM PCB routes a single leg
  through the CT (confirmed 2026-05-06 by board re-inspection); F1 cal
  landed against a single-leg EVSE-tester pull plug, which matches the
  real topology, so the cal is representative. For split-phase EV loads
  (L1 + L2 equal magnitude, 180 ° out of phase) the single-leg current
  IS the per-leg charging current, and P = V<sub>line-to-line</sub> × I
  remains correct.
- Stack high-water sizing right-sized at idle; revisit under real charge
  load (see open `E10`).
- GFCI CAL self-test timing tuned for this unit's GFCI module
  (~370 ms latch latency, ~280 ms hold). Other PCB instances may
  need re-tuning of `GFCI_CAL_PULSE_MS` / `GFCI_CAL_RECOVER_MS`
  via cmake -D overrides; the `gfci_self_test()` per-poll edge logs
  pinpoint where the bench unit's chip lands in time.

## Bench-gated for v1.x

Cal items still TODO before the firmware is "done" rather than "shipped":

- ~~**F1** — BL0939 IA / IB / PA calibration~~ ✅ DONE 2026-05-06.
- **F2** — 5-point CP negative-half fit → unblocks `FAULT_DIODE_CHECK`.
  **Deferred to v1.1**; needs a hardware revision (bipolar CP read-back
  daughterboard). Stock V1.0.066 also skips diode check on this PCB.
- ~~**F3** — GFCI 8-state CAL self-test scope-validation~~ ✅ DONE 2026-05-06.
- ~~**F5** — full charging session with a real EV (M10).~~ ✅ DONE
  2026-05-07 morning, garage. V cal confirmed L-L scaled (no rescale),
  CT polarity correct, PWM duty correct, no faults during charging,
  session_mwh integrating cleanly. STUCK_OPEN settle bumped 3.2 s →
  30 s for real-EV BMS pre-charge ramp.
- **F6** — CC ladder bench characterisation → lift the
  `OPENEVCHARGER_CC_DETECTOR` build flag. Deferred until a tester
  with built-in CC resistor decade is available.
- ~~**F7** — Relay actuate-readback under live AC mains.~~ ✅ CLOSED N/A
  2026-05-06 — no closed-feedback sense pin on this hardware revision.
- **F8** — AC-absent threshold tuning. Now possible in real volts
  (V cal landed via F1).
- ~~**F10** — PE continuity sense topology characterisation.~~
  ✅ CHARACTERISED 2026-05-07 garage; **deferred to v1.1**. PC5 is
  mains-current-coupled (raw 1 idle / 500–700 charging), so we can't
  distinguish "PE broken" from "AC flowing." Needs proper PE-sense
  hardware (active bond test pulse on dedicated pin). GFCI is the
  v1.0.0 PE-related safety.

## License

GPL-3.0. The firmware is a clean-room rewrite of OpenEVSE concepts
adapted to GD32F2 hardware; no upstream OpenEVSE code is incorporated.

## Acknowledgements

- OpenEVSE project for the J1772 / state-machine model.
- New Energy CS (Rippleon ROC OEM) for shipping reasonable hardware
  that's reverse-engineerable.
- evcc, MicroOcpp, ESPHome, LibreTiny upstream maintainers.

---

**Tag command** (run in OpenEVCharger submodule once F5 passes):

```bash
git tag -a v1.0.0-roc001 -m "OpenEVCharger v1.0.0 — Rippleon ROC001"
git push origin v1.0.0-roc001
```
