# Diode Check Investigation (FAULT_DIODE_CHECK / F2)

_2026-05-06 — investigates whether the stock V1.0.066 firmware does a
J1772 diode-check, and what hardware/software F2 would actually
require if we choose to implement it._

## Question

The OEM ROC001 sells as a J1772 EVSE. SAE J1772 §6.5 calls for an
EVSE-side check that the EV's onboard diode is present (clamps the
CP negative half to ~−1.5 V). If the EV has no diode, a hostile
"cable claiming to be a vehicle" can pull the CP-HIGH side down via
a passive resistor and trick the EVSE into closing the contactor with
no real EV present.

So: does the stock firmware do this check, and if so, how does it
work on a PCB whose CP read-back divider clamps to 0 V on negative
half?

## Findings — stock firmware does NOT do diode check

Three converging pieces of evidence from the V1.0.066 SWD dump
(`recovery/stock-mcu-V1.0.066.bin`).

### 1. No diode-related strings in the firmware binary

```
$ strings -a stock-mcu-V1.0.066.bin | grep -iE 'diode|DIODE|missing.*pilot|pilot.*low|negative half'
(no output)
```

The fault-string table in the binary lists:
```
Unknown Fault, Meter Fault, CP failure, EEM Comit Fault, PE Fault,
Leakage Fault, Ext Leakage Fault, Self Leakage Fault, Others Fault,
ADC Self Fault, PC Self Fault, flash Self Fault
```

Plus OCPP-standard error codes:
```
ConnectorLockFailure, EVCommunicationError, GroundFailure,
InternalError, OtherError, OverVoltage, PowerMeterFailure,
PowerSwitchFailure, UnderVoltage
```

The closest is **"CP failure"** (generic), but every other detector
in the firmware has its own specific string ("Ext Leakage Fault",
"Self Leakage Fault", "Relay Adhesion", "OverTemperature"). A diode
check would be expected to have a label like "Diode Fault" or
"Cable Fault" if it existed.

### 2. No injected-ADC channel configuration in the firmware

A diode check requires sampling PA4 specifically during the negative
half of the CP waveform — the +12/−12 V swings inside each 1 ms PWM
period. That requires either:
- ADC injected sampling triggered by the PWM timer (what OpenEVCharger
  does — ADC0 inject @ T0_TRGO for cp_high, ADC1 inject @ T0_CH3 for
  cp_low), OR
- A comparator + edge-time capture, OR
- ADC continuous-mode + software-coordinated read at known phase.

Searching the firmware for any literal pool reference to GD32 ADC
register addresses:

```
ADC0 base (0x40012400): 1 hit  (ADC init at 0x0800cb70)
ADC1 base (0x40012800): 0 hits
ADC INJSQR (inject sequence reg): 0 hits
ADC IDATA0 (inject result reg):   0 hits
ADC CTL1   (inject trigger sel):  0 hits
```

Only one ADC base address is ever loaded — and the surrounding init
code at `0x0800cb70` configures regular (DMA-scan) channels via
repeated calls to `ADC_RegularChannelConfig()`. No
`ADC_InjectedChannelConfig` calls. **The stock firmware never reads
PA4 with timer-coordinated phase information.**

### 3. The hardware divider physically can't support it

Bring-up M3 characterized the OEM CP read-back divider:

| CP voltage | PA4 raw | PA4 voltage |
|---:|---:|---:|
| +12 V | 1462 | 1.18 V |
| 0 V | ~728 | 0.59 V |
| any negative | 0 (clamped) | 0.00 V |

The divider is one-sided — every negative CP value reads PA4 raw=0,
indistinguishable. A diode-clamped CP-LOW at −1.5 V and a
hard-shorted CP-LOW at 0 V both produce raw=0. No software can
recover what the hardware throws away.

## How the stock firmware classifies J1772 states without negative-half data

It works because **state classification doesn't actually need the
negative half** — only the HIGH-phase voltage matters (+12 / +9 /
+6 / +3 V for A / B / C / D). With continuous DMA scan, PA4 reads
are time-averaged across the PWM period. The firmware combines the
average raw reading with the commanded PWM duty cycle to back out
the HIGH-phase voltage:

```
avg_raw ≈ duty × HIGH_phase_raw + (1 − duty) × 0
        = duty × HIGH_phase_raw
HIGH_phase_raw = avg_raw / duty
```

So state A (no PWM, +12 V steady) reads avg_raw ≈ 1462. State B at
10% duty advertising 6 A reads avg_raw ≈ 0.10 × ~1100 = 110. The
two are distinguishable, and the EV's HIGH-phase clamp (+9 V vs
+12 V) shows up cleanly in the back-computed value.

This works for state classification. It fundamentally cannot do
diode check.

## What F2 would require if we choose to implement

### Path A: Hardware mod + software cal

1. **Daughtercard / wire mod on PA4.** Add an offset-bias resistor
   network that shifts the divider's reference so the full ±12 V CP
   range maps into 0..3.3 V on the ADC. Concrete options:
   - Pull PA4 to a midpoint reference (~1.65 V via 1:1 voltage
     divider from VREF or 3.3 V) so the divider's effective output
     becomes (CP × scale + 1.65 V).
   - Replace the divider with a symmetric op-amp gain stage with
     bipolar input (would need a small daughterboard).
   - Trace the OEM PCB for any unpopulated footprint that already
     has a bipolar CP read-back (unlikely — the stock firmware would
     have used it).

2. **5-point cal in software.** With the bias mod in place, drive CP
   to known voltages via the bench tester or scope-controlled PWM,
   record raw at each:
   - State A: drive CP to +12 V (no PWM), record raw.
   - State F: drive CP to −12 V continuous, record raw.
   - States B/C/D: with EV-side resistor matrix on the bench tester,
     record raw during the LOW phase (cp_low_mv() already samples
     this via ADC1 inject).
   - Plus 0 V and one intermediate point.
   Fit a piecewise-linear curve. Storage exists in
   `struct calibration` (anchor + slope) but would need extension
   to hold separate negative-half coefficients (or just verify the
   linear fit holds end-to-end with the new bias).

3. **Detector code.** Add `check_diode_lost()` in safety_task:
   - On J1772 A→B transition, arm a "first negative-half sample
     needed" flag.
   - Sample `cp_low_mv()` once the classifier has settled (≥3 ticks
     of B-band debounced).
   - If `cp_low_mv > -10000`: latch `FAULT_DIODE_CHECK`. The fault
     enum entry already exists in `core/fault.h` (id=6) — just needs
     the detector wired up.
   - Else: mark this session as diode-verified, don't re-check until
     the next plug-in cycle.

4. **Bench validation.** Test with both:
   - Bench tester current-pull plug (has the J1772-spec diode →
     cp_low should clamp to ~−1.5 V) → no fault raised.
   - A cable with the diode physically lifted or shorted → expect
     FAULT_DIODE_CHECK within a few ticks of A→B.

### Path B: Skip it for v1, document the carve-out

Defensible choice given:

- Stock firmware ships without it. The OEM doesn't see this as a
  must-have for their (Chinese / fleet) market.
- The EV's onboard charger validates its own diode at handshake.
  The EVSE-side check is defense-in-depth.
- The hardware mod is non-trivial (a daughterboard or a careful
  wire mod), reducing the value of a software-only F2 attempt.
- v1.0.0-roc001 is bench-validated end-to-end without diode check.
  Adding it later as v1.1 with a documented hardware revision is
  cleaner than a half-hearted software-only attempt.

§D in `status-2026-05-06.md` already documents `FAULT_DIODE_CHECK` as
an open carve-out blocked on F2; this doc adds the "and why the
stock firmware also skipped it" detail.

## Recommendation

**Path B for v1.** The stock firmware ships without it on the same
hardware, and the EV's own diode-validation makes the EVSE-side
check defense-in-depth rather than load-bearing. Revisit when:

- A v1.1 hardware revision adds bipolar CP read-back (small
  daughterboard or PCB respin), OR
- A market we're targeting (UL2231 listing, US residential) makes
  it a hard requirement, OR
- A specific incident surfaces a hostile-cable scenario where
  diode check would have caught it.

Until then, leave the enum in place (`FAULT_DIODE_CHECK` = 6) and
the carve-out documented. The detector code path is the smallest
piece of work — the hardware enablement is the gate.
