# Safety system status

Canonical inventory of OpenBHZD safety detectors: what is active, what
is gated, and what each gated detector needs before it can be safely
enabled. Source of truth — `projectstate.txt` summarises this; code
comments cross-reference it.

Last updated: 2026-05-03.

---

## Active detectors

| Detector | Trigger | Trip → | Notes |
| -- | -- | -- | -- |
| IWDG watchdog | Task lockup ≥ 1 s | Hardware reset | `hal/wdg.c` |
| `FAULT_CRASH_LOOP_SAFE_FAIL` | ≥ N watchdog resets in window | EVSE_FAULT, latched | `persist/crash_state.c`, `safety_task::check_safe_fail` |
| `FAULT_CP_NO_PILOT` | J1772 state E sustained 60 ms | EVSE_FAULT, latched | `safety_task::check_cp_e` |
| `FAULT_BOOT_SELF_TEST` | ADC sanity / relay-open / CP-A floor at boot | EVSE_FAULT, latched | `safety_task::run_boot_self_test` |
| `FAULT_OVER_TEMP` | Per-channel NTC raw ≤ trip threshold sustained 100 ms (5 ticks) | EVSE_FAULT, self-clearing with 10 °C hyst | `safety_task::check_over_temp`. NTC1 on by default; NTC2 off (gun thermistor). Build-time toggle: `OPENBHZD_NTC{1,2}_PRESENT`. |
| Hardware force-open latch | EVSE_FAULT entry | PB12 HIGH → contactor latched open | UL2231-style redundancy against PE12 driver failure. `relay_force_open_latch()`. |
| User pause/resume | Top button or FC41D TLV `REQUEST_STOP/_RESUME` | EVSE_USER_PAUSED | Spec § 4.2; FC41D and physical button share the inbox path. |
| CP idle-high in USER_PAUSED, state-F in FAULT | EVSE state machine | CP output | `safety_task::apply_cp_for_state` |
| DIP1 + HW amp clamp | `effective_advertised_amps()` | min(FC41D, DIP1, 48 A) | Spec § 3 |
| `session_log` + `event_log` | Every fault edge / session end | W25Q persistence | `persist/event_log.c`, `persist/session_log.c` |

---

## Gated detectors

Each row lists the fault, the missing input that gates it, the
build/runtime flag that enables it, and the bench investigation needed
to lift the gate. **Order roughly tracks production-blocker priority.**

### `FAULT_GFCI` — gated

- **Why gated:** No GFCI sense pin identified in the pin map yet. PE3
  drives the GFCI CAL pulse output; no return EXTI input has been
  found. UL2231 mandates this — production blocker.
- **Bench needed:** Probe the GFCI module's output pin while
  injecting a CT differential current; identify which MCU input
  toggles. Likely an EXTI line on a pin we currently classify as
  unused.
- **Enable:** Wire EXTI handler in `hal/gfci.c`; add detector to
  `safety_task` tick. Latched, power-cycle-only clear (spec § 4.2).

### `FAULT_RELAY_WELD` / `FAULT_RELAY_STUCK_OPEN` — gated

- **Why gated:** No relay closed-feedback signal identified. PB12
  was the original guess but it's a force-open OUTPUT (UL2231
  latch). PB0/NTC2 was the second guess but reads 565–686 raw across
  all relay states (more likely AC-mains-presence sense). Without a
  real sense input, weld and stuck-open detection are blind.
- **Bench needed:** Scope every otherwise-unassigned MCU input
  while toggling PE12 with the AC contactor energised. The stock
  firmware's safety supervisor probably reads it — disassembly trace
  could short-circuit the search.
- **Enable:** Identify pin → wire `relay_main_sense_closed()` in
  `hal/relay.c` → flip `OPENBHZD_RELAY_FEEDBACK_KNOWN=1` to enable
  `check_relay_weld` + `check_relay_stuck_open`. Per-boot
  actuate-and-readback test in `safety_task::self_test_relay_actuate`
  is gated separately by `OPENBHZD_RELAY_ACTUATE_SELF_TEST`.

### `FAULT_DIODE_CHECK` — gated

- **Why gated:** CP read-back divider is one-sided on this PCB —
  swings only ~0..1.18 V at +12 V CP, so the negative half of the
  pilot waveform isn't observable. Diode check (state F detection)
  needs the EV's diode to clamp the negative half to ~−1.5 V; with
  no negative half visible, we can't distinguish good diode from bad.
- **Bench needed:** Add an external scaling/biasing stage on PA4 (or
  use a different ADC pin) that can resolve −12 V .. +12 V into the
  ADC's 0..3.3 V range. Calibrate the negative half via the M3
  3-point fit extension.
- **Enable:** 5-point CP fit covering the negative half-range, then
  add detector that checks state F sustained ≥ 60 ms while CP duty
  cycle is in the negative half → raise `FAULT_DIODE_CHECK`.

### `FAULT_HARD_OVER_CURRENT` / `FAULT_SOFT_OVER_CURRENT` — gated

- **Why gated:** CT secondary on PC0/PC1 is unconverted to amps. No
  base offset, no scale, no linearity check. Enabling at any
  threshold without calibration false-trips immediately or, worse,
  silently misses real overcurrent.
- **Bench needed:** Inject known currents (e.g. 0 A, 16 A, 32 A,
  48 A) through the primary at the bench, capture PC0/PC1 raw. Fit
  scale + offset; verify linearity. Decide soft-trip threshold
  (= advertised amps × 1.10, say) and hard-trip (≥ 60 A).
- **Enable:** Land calibration (similar to mains-voltage path).
  Update `system_state.active_amps_x10`; add detector that compares
  active vs `effective_advertised_amps()` × tolerance.

### `FAULT_AC_ABSENT` — gated

- **Why gated:** PB0 (NTC2 channel) is the strongest candidate for
  AC-mains-presence sense — reads 565..686 raw with mains live, ?
  without. Threshold + hysteresis are unknown until the bench is
  re-probed with mains explicitly cycled.
- **Bench needed:** Toggle bench AC supply on/off; capture PB0 raw
  in both states. Fit a hysteresis band. Verify the signal is
  rectified L1 upstream of the contactor (so it survives contactor
  open).
- **Enable:** Add `check_ac_absent()` reading `ADC_RANK_NTC2` raw,
  trip if below threshold sustained ≥ 200 ms while EVSE is in
  CHARGING. Self-clearing.

### `FAULT_CC_OUT_OF_RANGE` — gated

- **Why gated:** CC sense (PA7) is wired and scanned but the
  raw → max-amps mapping isn't validated. Spec § 3 J1772 CC ladder is
  20 / 32 / 40 / 50 / 80 A → 1.5k / 680 / 220 / 100 / open ohms; map
  needs bench characterisation.
- **Bench needed:** Plug a J1772 with known CC resistor (or sub
  resistors); capture PA7 raw at each value. Fit boundaries.
- **Enable:** Replace `cc_max_amps = 0` in the snapshot with the
  decoded value; clamp `effective_advertised_amps()` against it; add
  detector that trips if raw lands outside any defined band sustained
  ≥ 200 ms.

---

## Hardware-only safeties (always on)

- **PB12 force-open latch** — UL2231-style. Asserted on EVSE_FAULT
  entry by `safety_task::evse_transition`; released on exit.
- **GFCI CAL self-test pulse** at boot via PE3 — wired but currently a
  no-op (no sense input → no `FAULT_GFCI_SELF_TEST` detection).
- **Mechanical contactor coil** is supplied through the relay drive
  transistor; PE12 LOW = de-energised regardless of MCU state.

---

## Pre-deployment blocker list

To ship this firmware to a real install, the following gated
detectors **must** be lifted (UL2231 / J1772 / NEC compliance):

1. `FAULT_GFCI` — required by UL2231.
2. `FAULT_RELAY_WELD` — required by UL2231.
3. `FAULT_DIODE_CHECK` — required by SAE J1772 to detect
   missing/shorted EV diode.
4. `FAULT_HARD_OVER_CURRENT` — required to prevent contactor /
   wiring damage past hardware ratings.

The remaining (`AC_ABSENT`, `SOFT_OVER_CURRENT`, `CC_OUT_OF_RANGE`,
`STUCK_OPEN`) are strongly recommended but not strict compliance
gates.
