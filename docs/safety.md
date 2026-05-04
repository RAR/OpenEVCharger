# Safety system status

Canonical inventory of OpenBHZD safety detectors: what is active, what
is gated, and what each gated detector needs before it can be safely
enabled. Source of truth — `projectstate.txt` summarises this; code
comments cross-reference it.

Last updated: 2026-05-03 (evening — channel-role correction).

## Channel-role correction (2026-05-03 evening)

A bench experiment grounding the front-block "NTC" pins zeroed the
ADC channels we'd been calling "AC" (PA2) and "NTC1" (PA3),
confirming both as populated thermistors. The previous "Mains
Voltage @ 0.06151 V/count" calibration was a coincidence — a
disconnected NTC pin happened to float-rail near a value that scaled
to a plausible mains number. Real V/I sensing on this PCB likely
runs through the U11 PGA (gain bits PB9 / PD15, output → some ADC
channel still TBD); enabling it is a separate bench investigation.

Updated channel roles:

| ADC pin | Rank name (legacy) | Actual role | Populated? | Notes |
| -- | -- | -- | -- | -- |
| PA2 | `ADC_RANK_AC` | Gun-cable / J1772-handle NTC | Yes | "Second NTC" by OEM intent |
| PA3 | `ADC_RANK_NTC1` | Wall-plug end NTC | Yes | "First NTC" by OEM intent |
| PB0 | `ADC_RANK_NTC2` | Probably AC-mains-presence sense | No (not a thermistor) | Reads 565..686 raw with mains; ungated detector still TBD |
| PC0 | `ADC_RANK_CT` | CT secondary (after U11?) | TBD | V/I sensing via U11 not yet wired |
| PC1 | `ADC_RANK_LCT` | CT secondary (load CT?) | TBD | Same — needs U11 init + cal |
| PA7 | `ADC_RANK_CC` | J1772 CC sense | Yes | Mapping to A unvalidated |

Renames pending future commit (wide blast radius — `ADC_RANK_AC`
referenced across boot self-test + comms parse offsets):
- `ADC_RANK_AC` → `ADC_RANK_NTC_GUN`
- `ac_adc_raw` → `gun_ntc_adc_raw` (FC41D side already done)

The OVER_TEMP detector currently checks PA3 (NTC1) only. PA2 (gun
NTC) is the more safety-critical of the two — it's the cable + plug
that physically gets hot under load — and should be added to the
detector once the in-flight `core/over_temp.c` refactor lands. See
TODO at the bottom.

---

## Active detectors

| Detector | Trigger | Trip → | Notes |
| -- | -- | -- | -- |
| IWDG watchdog | Task lockup ≥ 1 s | Hardware reset | `hal/wdg.c` |
| `FAULT_CRASH_LOOP_SAFE_FAIL` | ≥ N watchdog resets in window | EVSE_FAULT, latched | `persist/crash_state.c`, `safety_task::check_safe_fail` |
| `FAULT_CP_NO_PILOT` | J1772 state E sustained 60 ms | EVSE_FAULT, latched | `safety_task::check_cp_e` |
| `FAULT_BOOT_SELF_TEST` | ADC sanity / relay-open / CP-A floor at boot | EVSE_FAULT, latched | `safety_task::run_boot_self_test` |
| `FAULT_OVER_TEMP` | Per-channel NTC raw ≤ trip threshold sustained 100 ms (5 ticks) | EVSE_FAULT, self-clearing with 10 °C hyst | `safety_task::check_over_temp`. NTC1 (PA3, wall-plug NTC) on by default; NTC2 (PB0, non-thermistor) masked off. FC41D-side °C conversion uses the stock fw's 150-entry LUT (`fc41d/.../ntc_lut.h`); MCU-side raw thresholds still β-derived (532/672) pending Phase-2 migration to LUT-derived (396/525). **TODO**: also wire PA2 (gun NTC, currently named `ADC_RANK_AC`) into the detector — the gun is the more safety-critical of the two channels. |
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

### `FAULT_GFCI` — sense pin + polarity confirmed; detector code still pending

- **Bench-confirmed 2026-05-04** via `tools/gpio_diff.sh`. With AC
  live, driving the GFCI module's external trip line HIGH dropped
  PE2 from 1 → 0; release returned PE2 to 1. PE2 is therefore the
  GFCI fault sense, **ACTIVE-LOW** at the MCU (module pulls PE2
  low when faulted; idle HIGH, likely open-drain output with an
  internal/external pull-up).
- **Polarity inverts the static-decode hypothesis.** The stock-fw
  state machine (flash `0x08012824`) ORs PE2's value into a fault
  byte at `[0x200026a0]`; the agent's read of "bit 1 set = fault"
  was probably wrong — bit 1 set most likely means "PE2 read HIGH"
  = "no fault, healthy". The static analysis stands as a structural
  decode; only the polarity interpretation needs flipping.
- **Bonus finding:** PD6 (USART1-RX printk line) also moved during
  the wiggle — almost certainly capacitive coupling from the trip
  wire on the bench harness, not a real second sense path. Documented
  in `pin_map.h`, no further action.
- **Enable (TODO):** Wire `hal/gfci.c` polled detector at the
  safety_task tick. Configure PE2 as input pull-up (or float — the
  module + its own pull-up keeps idle HIGH). Debounce 3–5 ticks of
  PE2 LOW → raise `FAULT_GFCI`. Latched, power-cycle-only clear per
  spec § 4.2 / UL2231. Optionally replicate the stock 8-state CAL
  self-test (drive PE3 + PE4, expect PE2 LOW mid-cycle) for
  `FAULT_GFCI_SELF_TEST` coverage.

### `FAULT_RELAY_WELD` / `FAULT_RELAY_STUCK_OPEN` — gated

- **Static analysis (2026-05-03, `docs/re-stock-safety.md`):** Stock
  fw was searched across all 29 `GPIO_ReadInputDataBit` callsites
  for a GPIOE bit-12 / 0x1000-mask read. None matched. The fault
  dispatcher at `0x0800edd4` confirms "Relay Adhesion" (id=0xb)
  and "Relay Action" (id=0xd) strings exist and are reachable, but
  the *raising* code path goes through a fault-sub-code halfword
  at RAM `0x200026b0`, not a discrete sense pin.
- **Why gated:** Strong inference — stock fw infers weld /
  stuck-open from **PC0 (CT902 secondary load-current)** rather
  than a discrete feedback pin. Welded contactor: PE12 commanded
  LOW but PC0 reads non-zero current. Stuck-open: PE12 HIGH and
  CP state expects current draw, but PC0 reads zero. Detection on
  this hardware therefore *couples* to the V/I path
  (HARD_OVER_CURRENT below) and cannot land independently.
- **Bench needed:** (a) scope every otherwise-unassigned MCU input
  during PE12 toggles under live AC to rule out a missed sense
  pin; (b) if confirmed absent, calibrate PC0 first.
- **Enable:** EITHER identify a real sense pin (then flip
  `OPENBHZD_RELAY_FEEDBACK_KNOWN=1`), OR land V/I path then add a
  current-inferred weld detector that watches PC0 vs PE12 + CP.

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

- **Why gated:** CT secondary on PC0/PC1 routes through the U11 PGA
  (gain-select bits PB9 / PD15) before reaching an ADC. U11 is not
  yet initialised, the gain register is unknown, and the
  raw → amps mapping is uncalibrated. Enabling at any threshold
  without all three false-trips immediately or, worse, silently
  misses real overcurrent.
- **Bench needed:** (1) probe / scope U11 to identify part number +
  gain-config protocol, (2) inject known currents (e.g. 0 A, 16 A,
  32 A, 48 A) through the primary, capture PC0/PC1 raw at each
  configured gain, (3) fit scale + offset; verify linearity. Decide
  soft-trip threshold (= advertised amps × 1.10, say) and hard-trip
  (≥ 60 A).
- **Enable:** Add `hal/u11.{c,h}` for the PGA, land calibration
  in `boot_config`. Update `system_state.active_amps_x10`; add
  detector that compares active vs `effective_advertised_amps()` ×
  tolerance.

### `FAULT_AC_ABSENT` — gated

- **Why gated:** PB0 (NTC2 channel) is the strongest candidate for
  AC-mains-presence sense — reads 565..686 raw with mains live, ?
  without. Threshold + hysteresis are unknown until the bench is
  re-probed with mains explicitly cycled. NB: V/I sensing on this
  PCB does NOT route through PB0 — that's via the U11 PGA on
  PC0/PC1 (see HARD_OVER_CURRENT above).
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
