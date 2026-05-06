# Safety system status

Canonical inventory of OpenEVCharger safety detectors: what is active,
what is gated, and what each gated detector needs before it can be
safely enabled. Source of truth — `projectstate.txt` summarises this;
code comments cross-reference it.

Last updated: 2026-05-06 evening (M6 ✅ end-to-end).

## Channel-role correction (2026-05-03)

A bench experiment grounding the front-block "NTC" pins zeroed the
ADC channels we'd been calling "AC" (PA2) and "NTC1" (PA3),
confirming both as populated thermistors. The previous "Mains
Voltage @ 0.06151 V/count" calibration was a coincidence.

V/I sensing comes from the BL0939 metering IC at U11 (Shanghai
Belling), bit-banged SPI on PB9 (SCLK) / PD15 (SDI) / PD14 (SDO).
PE2 ← BL0939 RCD alarm (active-low). PE3 → external CAL-injection
transistor for GFCI self-test.

Updated channel roles:

| ADC pin | Rank name (legacy) | Actual role | Populated? |
| -- | -- | -- | -- |
| PA2 | `ADC_RANK_AC` | Gun-cable / J1772-handle NTC | Bench unit: no. Production: yes. |
| PA3 | `ADC_RANK_NTC1` | Wall-plug end NTC | Yes |
| PB0 | `ADC_RANK_NTC2` | Probably AC-mains-presence sense | No (not a thermistor) |
| PC0 | `ADC_RANK_CT` | Reserved (BL0939 owns I sense) | TBD |
| PC1 | `ADC_RANK_LCT` | Reserved | TBD |
| PA7 | `ADC_RANK_CC` | J1772 CC sense | Yes (OEM divider, non-standard topology) |
| PA4 | `ADC_RANK_CP` | CP read-back, positive half only | Yes |
| PC5 | `ADC_RANK_PE` | PE continuity sense | Yes (topology unconfirmed) |

---

## Active detectors (live + bench-validated)

| Detector | Trigger | Trip → | Bench validation |
| -- | -- | -- | -- |
| IWDG watchdog | Task lockup ≥ 1 s | Hardware reset | `hal/wdg.c` |
| `FAULT_CRASH_LOOP_SAFE_FAIL` | ≥ N watchdog resets in window | EVSE_FAULT, latched | `persist/crash_state.c` |
| `FAULT_GFCI` | PE2 LOW sustained 60 ms (3 ticks) | EVSE_FAULT, latched, **power-cycle clear** | 2026-05-05 — current injection on PE → external module trip → fault raised → contactor force-opened |
| `FAULT_GFCI_SELF_TEST` | Boot-time CAL pulse on PE3, expect PE2 round-trip | EVSE_FAULT, latched | 2026-05-06 — chip latches PE2 ~370 ms post-release, holds ~280 ms, clean self-release. Tuning: 500 ms pulse + 1000 ms recover (`OPENEVCHARGER_GFCI_CAL_SELF_TEST=1` default) |
| `FAULT_RELAY_WELD` | BL0939 IA ≥ 500 mA while PE12 commanded open, after 3.2 s settle | EVSE_FAULT, latched | 2026-05-06 — load-cap discharge bench-observed 1051 mA for ~1 s after a 6 A session ends; settle window suppresses |
| `FAULT_RELAY_STUCK_OPEN` | BL0939 IA < 500 mA while PE12 commanded closed in CHARGING for 3.2 s | EVSE_FAULT, latched | 2026-05-06 — bench tester current-pull plug ramps slower than a real EV; 3.2 s debounce tolerates it |
| `FAULT_HARD_OVER_CURRENT` | BL0939 IA > advertised × 1.20 sustained > 5 s | EVSE_FAULT, latched, halts charging | 2026-05-06 — fault landed at exactly 5 s with 6 A advertised + 10 A draw |
| `FAULT_SOFT_OVER_CURRENT` | BL0939 IA > advertised × 1.05 sustained > 30 s | duty −10 %, repeat to 6 A floor, raise (self-clearing) | 2026-05-06 — fault landed at exactly 30 s. UI alert (red flash + buzzer) suppressed via `FAULT_LATCHED_MASK` |
| `FAULT_CP_NO_PILOT` | J1772 state E sustained 60 ms | EVSE_FAULT, latched | Live since M3 |
| `FAULT_CP_REGRESSION` | C → B mid-charging | event-only (no fault raise, no state change) | 2026-05-05 — downgraded; graceful end-of-charge vs BMS cutoff vs tester unwind all look identical from EVSE side |
| `FAULT_PE_CONTINUITY` | PC5 raw > 400 for ≥ 10 ticks | EVSE_FAULT, latched | Live; F10 caveat: PC5 reads raw≤1 with no live mains regardless of PE state, needs install-side validation |
| `FAULT_OVER_TEMP` | Hottest-of-two NTC reaches LUT-derived trip | EVSE_COOLING_DOWN (auto-recover with hysteresis) | LUT 396/525 |
| `FAULT_AC_ABSENT` | BL0939 V_RMS below threshold | self-clearing | F8 threshold tuning still possible (V cal landed F1) |
| `FAULT_BOOT_SELF_TEST` | ADC sanity / relay-open / CP pilot floor / GFCI CAL fail | EVSE_FAULT, latched | All four sub-checks bench-validated |
| `FAULT_ADC_OUT_OF_RANGE` (runtime) | AC/CT/LCT/CP rank out of band, debounced 5 ticks | self-clearing | Live; suppressed in BOOT/SELF_TEST/FAULT |
| Hardware force-open latch | EVSE_FAULT entry | PB12 HIGH → contactor latched open via UL2231-style hardware latch | Always on |
| Single-writer relay/PWM | All actuation through `safety_task` | Spec § 4.2 architecture | Always on |

---

## Gated detectors (build-flag-gated, default off)

### `FAULT_DIODE_CHECK` — deferred to v1.1 hardware revision

- **Why gated:** CP read-back divider on this PCB is one-sided —
  swings 0..1.18 V at +12 V CP, clamps near raw=0 for any negative
  CP excursion. The diode check needs to see the EV's diode clamping
  the negative half to ~−1.5 V; with the negative half not
  observable, software-only recovery is impossible.
- **Confirmed 2026-05-06:** Stock V1.0.066 firmware also skips
  diode check on this PCB (binary analysis: no diode-related
  strings, no injected-ADC channel config, only continuous DMA scan
  on PA4). EV onboard charger validates its own diode at handshake;
  EVSE-side check is defense-in-depth only.
- **Path forward:** v1.1 hardware revision adding a bipolar CP
  read-back daughterboard. Full investigation in
  `docs/diode-check-investigation.md`.

### `FAULT_CC_OUT_OF_RANGE` — gated, F6

- **Why gated:** OEM CC divider on PA7 is **not** the standard SAE
  J1772 pull-up topology. Bench reads raw≈12 with no plug
  connected (M2's earlier "raw=4095" measurement was contradicted
  by re-probing). Without a characterisation of how the OEM
  divider scales 1.5k / 680 / 220 / 100 / open Ω resistors at the
  J1772 plug, we can't decode `cc_max_amps` reliably and can't
  set safe out-of-range bounds.
- **Decoder is live** — the band lookup runs and publishes
  `cc_max_amps` to HA already. Only the *raise* path is build-flag-
  gated behind `OPENEVCHARGER_CC_DETECTOR=1`.
- **Bench needed (F6):** Plug a J1772 with known CC resistor (or
  sub resistors via a decade box) into the bench unit's J1772
  cable; capture PA7 raw at each of: open / 1.5k / 680 / 220 /
  100 Ω. Verify the published `cc_max_amps` matches the labelled
  resistor. Fit boundaries that cleanly reject "outside any band"
  reads.
- **Enable steps:** confirm the band-table boundaries map correctly
  on this PCB → set `OPENEVCHARGER_CC_DETECTOR=1` in CMakeLists →
  rebuild + reflash → verify the unit raises `FAULT_CC_OUT_OF_RANGE`
  if a non-J1772 resistance is presented (e.g. 50 Ω short to PE
  → out of band).

---

## Hardware-only safeties (always on)

- **PB12 force-open latch** — UL2231-style. Asserted on EVSE_FAULT
  entry by `safety_task::evse_transition`; released on exit.
  PB12 is **not** a sense input on this PCB — driving PB12 HIGH
  while PE12 HIGH forces the contactor open via a hardware latch.
- **GFCI CAL self-test pulse** at boot via PE3 — now active. See
  `FAULT_GFCI_SELF_TEST` row above.
- **Mechanical contactor coil** is supplied through the relay drive
  transistor; PE12 LOW = de-energised regardless of MCU state.

---

## Pre-deployment status

UL2231 / J1772 / NEC compliance gates:

1. ~~`FAULT_GFCI`~~ — **DONE** (`78b6c16`, tag `gfci-live`).
2. ~~`FAULT_GFCI_SELF_TEST`~~ — **DONE** 2026-05-06.
3. ~~`FAULT_RELAY_WELD`~~ — **DONE** 2026-05-06 (BL0939 IA-based,
   no discrete sense pin needed; PB12 turned out to be the
   force-open latch, not a sense).
4. ~~`FAULT_RELAY_STUCK_OPEN`~~ — **DONE** 2026-05-06.
5. ~~`FAULT_HARD_OVER_CURRENT` / `FAULT_SOFT_OVER_CURRENT`~~ —
   **DONE** 2026-05-06 (BL0939 + F1 calibration).
6. ~~`FAULT_AC_ABSENT`~~ — **DONE** (BL0939 V_RMS); F8 threshold
   re-tune in real volts is a refinement, not a blocker.
7. **`FAULT_DIODE_CHECK`** — **deferred to v1.1 hardware revision.**
   Stock fw also skips. Mitigation: EV-side diode self-check at
   handshake.
8. **`FAULT_CC_OUT_OF_RANGE`** — gated (F6 bench item; not strict
   compliance — recommended).

Remaining bench-blocking item before tag: **F5 — full charging
session against a real 240 V EV.** Doubles as the both-legs-through-
one-CT topology validation (the BL0939 cal landed at bench against
a single-leg EVSE-tester pull plug; balanced split-phase loads may
sum to ~0 in the single CT — needs real EV to confirm).
