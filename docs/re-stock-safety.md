# Stock-firmware safety lift report (V1.0.066, GD32F205VG)

Mining target: `/home/rar/device-configs/esphome/rippleon/OpenEVCharger/recovery/stock-mcu-V1.0.066.bin`
(SHA256 `d61a0ed1...3fc814fb`, 512 KB, base `0x08000000`).

Goal: lift gating on three OpenEVCharger detectors —
NTC over-temp, GFCI sense, relay closed-feedback — by recovering the
stock firmware's calibration constants and runtime polling sites.

This is a static-analysis report; nothing was modified. All firmware
addresses are absolute flash addresses.

---

## Executive findings (TL;DR)

| Detector | Status after this pass | Confidence | Bench validation needed before flag flip? |
| -- | -- | -- | -- |
| **Q1 NTC over-temp** | **Resolved**: 150-entry LUT at `0x08024f28`; trip = 95 °C default (raw ≤ 300), max-allowed setpoint 120 °C | **HIGH** — table dump cross-checks with live PA3=1.19 V reading | Light bench cross-check recommended (heat one NTC and confirm one ADC channel slides through the LUT as expected). Flag flip is otherwise safe. |
| **Q2 GFCI sense** | **Resolved**: PE2 polled in self-test state machine `0x08012824`. No EXTI used (all EXTI vectors point at default trap stub). Stock firmware polls PE2 every state-tick (~200–1500 ms cadence). | **MEDIUM-HIGH** — fits the static decode, but PE2 being live as a GFCI fault output sense isn't yet bench-confirmed (PE2 currently reads idle high, classified as "DIP/strap" in `pinout.md`). | **Yes**: fault-inject the GFCI module and confirm PE2 toggles HIGH. Until that's done, do not enable the FAULT_GFCI detector — false-positive risk if PE2 is actually a strap. |
| **Q3 Relay closed-feedback** | **Not resolved**. PB12 turns out to be writable from one code path (`0x0800d30c` clears it via BRR) — likely a force-open output, NOT a sense input. No clean closed-feedback pin has surfaced in the firmware; the most probable mechanism is "compare PC0 (CT902 secondary) against expected current given PE12 command", but that's an inference, not a confirmed code path. | **LOW** | **Yes**: bench investigation still required. Suggested next step: scope every otherwise-unassigned MCU input while the contactor opens and closes under live AC. |

---

## Q1 — NTC over-temp threshold table

### Findings

The stock firmware does **not** use a β / R0 formula. It uses a
**150-entry direct-lookup LUT** mapping ADC raw → temperature index.

- **LUT base:** `0x08024f28`
- **Length:** 150 × `uint16` (300 bytes)
- **Reference:** sole literal-pool ref at `0x0800bc50`
  (used inside the temperature-task at `0x0800bba0+`).
- **Mapping:** `temp_°C = lut_index − 30` (so the LUT spans **−30 °C
  to +119 °C** in 1 °C steps).
- **NTC topology implied by LUT:** 10 kΩ NTC + **10 kΩ** pull-up to
  3.3 V, ADC 12-bit (0..4095). Cross-check: index 55 (= 25 °C) holds
  `2048` raw counts, which equals 3.3 V × 10/(10+10) ÷ 3.3 V × 4096 =
  2048. **NOT the 4.7 kΩ pull-up assumption in `pinout.md` —
  correct that.**
- **Trip threshold (factory default):** OverTempValue = 95 °C, stored
  as a plain `int16` at config offset `0x360`. Raw equivalent:
  **lut[125] = 300 counts**. Detector should fire when
  `ntc_raw <= 300`.
- **Clear / hysteresis:** the OpenEVCharger spec uses 10 °C hysteresis; the
  stock firmware does not appear to implement explicit hysteresis at
  this site. Use 85 °C clear → **lut[115] = 396 counts**.
- **Out-of-range guards** (function `0x0800bba0`, around `0x0800bc0a`):
  - `if raw > 0xF31 (3889)` → result clamped to `0x78` (= 120 °C),
    then stored. `3889` is exactly `lut[0]` (the −30 °C entry), so
    raw > 3889 = NTC open / disconnected.
  - `if raw < 0x9E (158)` → also clamped to 120. `158` is `lut[149]`,
    the +119 °C entry, so raw < 158 = NTC short / above range. (Both
    extremes resolve to the same 120 °C value, which the firmware
    treats as a hard over-temp, since 120 ≥ user setpoint of 95.)

### Function: temperature-task at `0x0800bba0` (key disasm)

```
0x0800bbb8  ldr r7, [pc] = 0x20002a80  ; ADC DMA buffer
0x0800bbba  ldr r5, [pc] = 0x200060a8  ; per-channel state (24 B × 4 ch)
0x0800bbbc  movs r6, 0x78               ; "out of range" sentinel = 120
loop r1 = 0..3:                         ; 4 NTC channels
  state = r5 + r1*24
  cnt = state[0x10]
  if cnt < 18: accumulate raw
  else:
    avg = (state[0xC] - state[0]_max - state[4]_min) >> 4   ; mean of middle 16
    if avg > 0xF31:  result = 0x78
    elif avg < 0x9E: result = 0x78
    else:
      r4 = 0x08024f28
      for i = 0..149:
        if avg > lut[i]: break
      result = i - 30
    state[0x14] = result
    config_struct[0x86A + 2*r1] = result   ; published to per-channel field
```

Note: 4 channels are scanned, but only PA3 (NTC1) and PB0 (NTC2) are
populated on this SKU per `pinout.md`. The other two LUT slots will
read garbage — the firmware presumably has a per-channel disable
elsewhere (not investigated this pass).

### Default config initializer (function `0x0800fb98`, "Device Parame Init")

```
0x0800fbf8  movs r0, 0x5f              ; 95 (decimal)
0x0800fbfa  strh.w r0, [r4, 0x360]     ; OverTempValue → config[+0x360]
```

Other defaults in same block (for completeness):

```
[r4,0x20]  = 0x42400000  = 48.0f       ; nominal current (probably default amps, possibly UI scaling)
[r4,0x24]  = 0x43900000  = 288.0f      ; nominal voltage
[r4,0x28]  = 0x42b40000  = 90.0f       ; (unknown — possibly a temp-related coefficient)
[r4,0x354] = 0x1680      = 5760        ; OverCurrent setpoint, scaled (= 48 A × 120)
[r4,0x358] = 0x7080      = 28800       ; OverVoltage setpoint (= 240 V × 120)
[r4,0x35c] = 0x2328      = 9000        ; UnderVoltage setpoint (= 75 V × 120)
[r4,0x360] = 0x5f        = 95          ; OverTempValue (no scaling, plain °C)
```

Float scaling factors `1.2f` (`0x3F99999A`) and `100.0f`
(`0x42C80000`) live at `0x08021110` / `0x08021114`. Used on
OverCurrent / OverVoltage / UnderVoltage to convert user input to
stored raw — i.e. `stored = user × 1.2 × 100 = user × 120`. NOT
applied to OverTempValue.

### Upper-bound clamp in config validator (function around `0x08020e84`)

The validator reads `[r4, 0x360]` (signed half), prints
`"OverTempValue:%d"`, and checks:

```
0x08020e88  ldrsh.w r0, [r4, 0x360]
0x08020e8c  cmp     r0, 0x79           ; 121
0x08020e8e  blo     0x8020ec6          ; passthrough if < 121
            ; otherwise → fall-through into Warning print
```

So **the firmware accepts user setpoints in `0..120 °C`**. Anything
≥ 121 is logged as `"Warning: OverTempValue:%d"`. Whether it's
clamped or just warned wasn't pinned down (the fall-through path is
intricate); treating ≥ 121 as out-of-range is the safe stock
interpretation.

### What to do in OpenEVCharger code

1. Replace the β=3380 / R0=10k Steinhart-Hart approximation with the
   stock LUT.
   - **File:** likely `hal/ntc.c` (per `safety.md`'s reference to a
     `safety_task::check_over_temp` detector).
   - **Action:** embed the 150-entry LUT (binary copy from
     `0x08024f28..0x08025054`), rewrite `ntc_raw_to_celsius()` as a
     linear scan + offset-by-30. Keep the existing per-channel
     filtering window (5 ticks) — it's compatible.
2. **Update the divider topology assumption** — pull-up is 10 kΩ, not
   4.7 kΩ. If any code uses the pull-up for sanity calculations, fix
   it. (`pinout.md` line 320 also needs correcting.)
3. **Trip / clear thresholds:** trip at raw ≤ 300 (= 95 °C),
   clear at raw ≤ 396 (= 85 °C, applying OpenEVCharger's 10 °C hysteresis).
4. **Build flag:** OpenEVCharger presumably already has `OPENEVCHARGER_NTC1_PRESENT`
   etc. (per `safety.md` row). Once the LUT is wired in, no flag flip
   is needed for NTC1 — it's already active. NTC2 toggle still
   bench-gated by gun-thermistor presence (separately tracked).

### Confidence: HIGH

Both the LUT contents and the trip threshold come straight from
flash, and the LUT origin is corroborated by the live PA3 reading
(1.19 V → 1477 counts → LUT index 67 → 37 °C, plausible bench-room
gun temperature on a powered unit). A bench cross-check (heat NTC,
log raw + reported °C) is still recommended as defence-in-depth, but
the OpenEVCharger flag flip from the legacy β formula to the stock LUT is
**safe even before that cross-check** because the lookup is
empirically grounded.

---

## Q2 — GFCI sense pin

### Findings

- **All seven EXTI vectors** in the table point at the default trap
  handler `0x08000182` (an infinite loop). Confirmed by reading the
  vector table at flash `0x40 + IRQn*4`. So **the stock firmware does
  not use any external interrupt** for safety sensing.
- **GFCI fault sense is polled, not interrupt-driven**, by a state
  machine at function `0x08012824` (8-state TBB-dispatched, runs on a
  timer triggered by `[0x20000104 + 0x10]`). r5 = `0x20000104`
  (state struct), r4 = `0x200026a0` (fault-flag byte struct).
- The state machine drives **PE3 (GFCI CAL pulse output)** and
  **PE4** (probably the "GFCI test latch" or pre-charge bleed; not
  fully unpacked), and **reads `GPIOE pin 2` (PE2)** in two states
  (cases 4 and 7) at `0x080128d0` and `0x08012928 / 0x08012938`.
- The read uses helper `0x08016be2` (`GPIO_ReadInputDataBit`), with
  `r0 = GPIOE_BASE = 0x40011800`, `r1 = 0x4` (= bit 2 = PE2). This
  is the **GFCI fault sense input**. It's ACTIVE-HIGH (the bit gets
  ORed into `[r4]` when PE2==1, cleared when PE2==0).
- Polling cadence: 1100 → 800 → 550 → 200 → 200 → 1500 → 500 ms
  per state, ~5 s for a full self-test cycle.
- The fault byte at `[r4]` collects two GFCI bits:
  - bit 1 = "PE2 high during steady-state" (= **fault: GFCI tripped
    while idle**)
  - bit 0 = "PE2 high during CAL pulse" (= **GFCI self-test PASSED:
    the module saw the simulated leakage and asserted its fault
    output**) — the absence of this bit is what raises a Self
    Leakage Fault (`r3=0xc` in the fault-string dispatcher at
    `0x0800edd4`).

### State-machine breakdown (function `0x08012824`)

TBB jump table at `0x0801285c..0801285c+8`:

| State | Address | Action | Next | Tick delay |
| -- | -- | -- | -- | -- |
| 0 | 0x08012864 | PE3=0; PE4=0 | 1 | 0x44C (1100) |
| 1 | 0x08012890 | PE3=1 (CAL on); PE4=0 | 2 | 0x320 (800) |
| 2 | 0x080128AE | PE3=0; PE4=0 | 8 | 0x226 (550) |
| 3 | 0x080128C0 | PE4=1 | 8 | 0xC8 (200) |
| 4 | 0x080128CE | **read PE2**: if 1 → set `[r4]` bit 1, else clear | 5 | (immediate) |
| 5 | 0x080128EE | PE4=0 | 6 | 0x5DC (1500) |
| 6 | 0x08012900 | check `[r4]` MSB-bit (`lsls r0, 0x1e; bmi`) — if set, transition to error | 7 | 0x1F4 (500) |
| 7 | 0x0801291E | re-test PE2; manage `[r4]` bit 0 (CAL-phase ok flag) | back to 0 | varies |

Cases 6 and 7 also `str r6, [r5, 0x10]` with r6=0 to **reset the state
machine to state 0**, completing a polling cycle.

### What to do in OpenEVCharger code

1. Add **PE2** to the pin map as `GFCI_FAULT_SENSE`, active-high,
   polled (no EXTI). Update `pinout.md`'s "Watched inputs" / "Confirmed
   but role unclear" section: PE2 is **not** a DIP/strap — it's the
   GFCI module's fault-output sense.
2. **Implement** in `hal/gfci.c`:
   - Periodic poll of `GPIOE.IDR & (1<<2)` at the same cadence as
     the safety_task tick (or at least every 200 ms during normal
     operation).
   - Replicate the 5-state CAL self-test: drive PE3 high for 800 ms,
     sample PE2 in the middle of that window, expect PE2 high. If
     PE2 stays low → `FAULT_GFCI_SELF_TEST`.
   - In normal operation (no CAL pulse): if PE2 high → `FAULT_GFCI`
     (real-time leakage detected by the module).
3. **Build flag:** flip `OPENEVCHARGER_GFCI_KNOWN=1` (or whatever the
   gating flag is in `safety_task`). Add `check_gfci()` to the
   tick, latched + power-cycle-only clear per UL2231.

### Confidence: MEDIUM-HIGH

The disasm decode is unambiguous — PE2 is the single GPIO read in
the GFCI state machine, and the fault-bit-set semantics line up with
"GFCI module asserted its fault output". But:

- `pinout.md` currently classifies PE2 as `in_pupd, ODR=1, IDR=1
  idle` (i.e., reads HIGH at rest). For a GFCI fault output that's
  HIGH when faulted, an idle-high reading is **inverted** from what
  we'd expect — meaning either:
  - PE2 is correctly classified as the GFCI sense, but the GFCI
    module's fault output is **active-low** (idle-high, low when
    leakage detected). The state-machine logic would then need a
    polarity flip (firmware doesn't appear to invert the bit it ORs in,
    so this hypothesis would mean idle-state is "fault asserted",
    contradicting normal operation).
  - PE2 is something else entirely and the firmware has a vestigial
    GFCI self-test path that's never actually entered (the dispatch
    is on `[r5+0x10]`; if that field is never written to non-zero,
    the state machine is dormant).

**Bench validation is required**: scope PE2 while the unit runs its
GFCI self-test on AC. If PE2 toggles HIGH during the PE3 CAL pulse,
the active-high interpretation is correct and OpenEVCharger can flip the
flag. If PE2 stays high constantly, this is the wrong pin (or
inverted polarity on the board).

---

## Q3 — Relay closed-feedback signal

### Findings

**No relay closed-feedback pin was confidently identified.**

What I tried:

1. **GPIOE PE12 mask (0x1000) write/read sites:** `mov.w r1, 0x1000`
   appears 4 times in the firmware. Of those:
   - `0x0800d30c` writes **PB12** (GPIOB + 0x1000) via BRR (clear
     bit). This appears to be a **force-open output** path
     (consistent with `safety.md`'s "PB12 force-open latch"
     interpretation), not a sense input. Caller is `0x0800b196`,
     looped over a 4-channel array.
   - `0x0800fbfe` reads **PD12** (DIP2/"KA"), already mapped.
   - `0x0801b5f2` and `0x0801ba5c` use `0x1000` as a *byte length*
     argument for SPI flash config save/restore (`"Save
     ConfigureValue:%d Bytes"` printf), unrelated to GPIO.
2. **GPIOE writes for PE12 itself:** the only writes to `GPIOE` go
   to PE3 (mask 0x8) and PE4 (mask 0x10) inside the GFCI state
   machine, plus PE13 as a TIM1 alternate function for J1772 CP. No
   firmware site directly toggles PE12 via the `GPIO_Write` helper.
   The contactor coil drive path may go via TIM1 alternate function
   (full-remap PE13/CH3 already used for CP) or via direct BSRR/BRR
   that doesn't go through the indirect helper.
3. **All `GPIO_ReadInputDataBit` callers (29 sites):** None reads
   GPIOE bit 12 (mask 0x1000). The only GPIOB read identified
   reads PB8 (mask 0x100, at `0x080127c6`), already classified as a
   board strap.

**Strongest remaining hypothesis:** the stock firmware does **not**
have a dedicated relay closed-feedback pin. Weld / stuck-open
detection is inferred from **load-current sensing on PC0 (CT902
secondary)**:

- Welded contactor: PE12 commanded LOW but PC0 reads non-zero
  current → "Relay Adhesion" fault (`r3=0xb` in dispatcher).
- Stuck-open: PE12 commanded HIGH and CP state expects current draw,
  but PC0 reads zero → "Relay Action" fault (`r3=0xd` in dispatcher).

I did not pin the trip path for either fault in the time budget.
The fault enumeration in dispatcher `0x0800edd4` confirms both fault
strings exist and are reachable, but the *raising* code path was not
traced.

### Disasm references

Fault dispatcher around `0x0800edd4` (snippet):
```
0x0800ef00  ldrb r0, [r4]
0x0800ef02  adr r1, "Relay Action"  ; fault string @ 0x0800f040
0x0800ef04  bl 0x800e68c            ; raise fault
0x0800ef08  movs r3, 0xd            ; fault id = 13

0x0800ef12  ldrb r0, [r4]
0x0800ef14  adr r1, "Relay Adhesion" ; @ 0x0800f050
0x0800ef16  bl 0x800e68c
0x0800ef1a  movs r3, 0xb            ; fault id = 11
```

These two paths are reached when `[r4, 0x10]` (a halfword fault
sub-code at `0x200026b0`) takes specific values; tracing the writers
of that halfword is the next step.

### What to do in OpenEVCharger code

**Do not flip the relay-feedback flag.** Status of
`OPENEVCHARGER_RELAY_FEEDBACK_KNOWN` should stay 0.

Bench investigation required (matches `safety.md` § "FAULT_RELAY_WELD"
already): scope every otherwise-unassigned MCU input while toggling
PE12 with the contactor energised on AC. The stock firmware's safety
supervisor probably reads it during a per-boot self-test — log
the SWD-snapshot pin map across PE12 OFF / PE12 ON.

If the bench investigation finds nothing toggling, the **PC0
inference path** (compare expected vs measured current) is the
fallback. That requires the over-current calibration to land first —
which `safety.md` already lists as a separate gated detector.

### Confidence: LOW

This question is unanswered. The static-only conclusion is that no
discrete sense pin was found. The bench step from `safety.md` is
still required.

---

## Confidence summary

| Detector | Q1 NTC | Q2 GFCI | Q3 Relay feedback |
| -- | -- | -- | -- |
| Static answer | LUT + 95 °C trip default | PE2, polled (no EXTI), active-high | Not found — likely no discrete pin |
| Confidence | HIGH | MEDIUM-HIGH | LOW |
| Safe to flip flag without bench? | **Yes** (LUT swap is a defensive change vs. β-formula assumption) | **No** — bench-confirm PE2 toggles correctly during CAL pulse | **No** — pin not identified |

The two recommended next bench steps, in priority order:

1. **GFCI:** scope PE2 while the unit is running stock firmware on AC.
   Look for PE2 going HIGH inside the PE3 CAL pulse window. If yes,
   PE2 is confirmed and OpenEVCharger can flip the GFCI gate. (1 hour
   bench.)
2. **Relay feedback:** snapshot all MCU inputs across PE12 off → on
   → off transitions under AC. Find the one that mirrors PE12 with
   ~10–50 ms lag. (1–2 hours bench.)
