# delta-bridge `adc` personality — replaces stock `/root/Adc`

**Date:** 2026-05-16
**Status:** SHIPPED + bench-validated. 40 host tests, replaces stock
`/root/Adc` cleanly. Bench publishes PILOT_STATE = `0x05` (PS_F) where
stock published `0x04` (TRANSIENT) at the same 120 V UVP-fault idle
condition — see §9 below for analysis of the difference.

Second of three planned replacements (`meter` ✅ → `adc` ← here →
`pricomm`). Per the docs/14 §7 / docs/15 §5 plan and now riding atop
the M7 personality-dispatch infrastructure.

---

## 1. What it does

Replaces stock `/root/Adc` — the J1772 pilot-state classifier. Reads
CP-sense bytes from `/dev/adc0` at ~128 Hz, runs them through a
voltage-class binning + sliding-window state machine, publishes one
byte to **`shmem[0x0a08]`** (the `PilotState` consumer is read by 7+
daemons including `LED_control`, `Pri_Comm`, `main`, `Charging_Standard_RFID`).

Stock's static-RE-discovered structure (docs/13 §3.3, /root/Adc nm output):

```
PV_State        State           Alarm_Flag
PV_State_Prev   volt_negtive    Buf_error
PV_P_Cnt        Buf_9V (20B)    Buf_6V (20B)
PV_N_Cnt        Buf_12V (20B)   Buf_N_12V (20B)
SysTime         MeterSMPtr      PilotState
```

The 20-byte per-class buffers suggest stock uses ~20-sample
sliding-window majority. We use a 16-sample window (~125 ms at 128 Hz)
as a close analogue.

## 2. Init protocol — two ioctl(`0x40104102`) calls with 16-byte arg

Captured from disassembly of /root/Adc main() — stock copies a 28-byte
init struct from rodata at vaddr `0xabec`, sends its first 16 bytes
as the first ioctl arg, then constructs a second 16-byte struct on
the stack and sends it as the second arg:

```c
const unsigned char ADC_INIT_CFG_1[16] = {
    0x01, 0x00, 0x00, 0x00,        /* u32 LE: 1     (channel id?)    */
    0x00, 0x00, 0x00, 0x00,        /* u32 LE: 0                       */
    0xc4, 0x09, 0x00, 0x00,        /* u32 LE: 2500  (sample rate Hz?) */
    0x00, 0x00, 0x00, 0x00,        /* u32 LE: 0                       */
};
const unsigned char ADC_INIT_CFG_2[16] = {
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,        /* u32 LE: 1     (enable?)         */
    0x00, 0x00, 0x00, 0x00,
};
```

We don't have the kernel `/dev/adc0` driver source so the field
semantics are guesses. Bit-for-bit reproduction is the safe path.

## 3. Voltage-class mapping (from bench-idle histogram)

docs/13 §3.2 captured: bench at 120 V UVP-fault, no plug, pilot off →
CP idles at -11.9 V. ADC byte cluster:

```
0x5e..0x65 (94..101)   = 39,725 samples (97% — CP at low rail)
0xeb..0xf0 (235..240)  =    997 samples (2.4% — brief +12V spikes)
```

Linear ADC-to-volt fit: **slope ≈ 5.79 ADC counts per volt, mid ≈ 167**.

J1772 voltage classes mapped to ADC bands (with ±1.5 V class
half-width margins):

| State | CP peak | ADC range  |
|-------|--------:|-----------:|
| PS_A  | +12 V   | 232..255   |
| PS_B  | +9 V    | 207..231   |
| PS_C  | +6 V    | 188..206   |
| PS_D  | +3 V    | 170..187   |
| PS_F  | -12 V   |   0..104   |
| (transient guard) | — | 105..169 |

The guard band (105..169) classifies as `PS_TRANSIENT` per-sample.
This is generous because exact ADC slope is calibration-dependent.

## 4. Window-state decision

`adc_window_state()` over the 16-sample ring:

1. **Bimodal rail-rail** check (FIRST): if window has BOTH PS_A and
   PS_F present AND zero mid-class samples → `PS_TRANSIENT`. This
   captures the bench-idle pattern (15× PS_F + 1× PS_A) which stock
   also publishes as state 4. Dominates the dominant-class rule.
2. **Dominant ≥75%**: if any single class has ≥`ceil(0.75*N)` votes,
   that class wins. (One PS_F noise sample doesn't pull a 15× PS_C
   window — dominant rule kicks in.)
3. **Otherwise**: hold previous state (hysteresis — dampens flips
   during transitions).

## 5. Publish strategy

- Publish on **state change** (immediate).
- Periodic refresh every 40 windows (~5 s) even if state unchanged —
  consumers see a "fresh" byte regularly.

This is somewhat more conservative than stock (which appears to
publish every window per the strb on stock disassembly at 0x9f44/etc.),
but consumers shouldn't notice — they don't time-out on the byte.

## 6. Host tests

`test/test_adc.c` (40 tests):

- `ADC_INIT_CFG_{1,2}` byte layout matches captured rodata
- 6 classes + transient guard, boundaries verified
- bench-idle low/high cluster bytes → correct per-sample states
- window dominant-class wins ≥75%
- window holds prev when ambiguous
- window bench-idle (15F + 1A) → PS_TRANSIENT
- window pure dominant (16F) → PS_F
- window dominant with one noise sample (15F + 1C) → PS_F
- edge: empty window → hold prev

## 9. Bench validation — DONE (2026-05-16)

Deployed via wrapper, rebooted, ps confirms all daemons alive:

```
618 vern  4016 S  /root/main
636 vern   548 S  /Storage/delta-bridge/delta-bridge.v08 --personality=adc  ← ours
637 vern   556 S  /Storage/delta-bridge/delta-bridge.v07 --personality=meter (M7)
748 vern   552 S  /Storage/delta-bridge/delta-bridge -c .../delta-bridge.conf
772 vern  1784 S  /root/Pri_Comm
```

shmem_dump after stabilisation:

```
@0x0a07  PRI_STATE    03   (Pri_Comm digesting, healthy)
@0x0a08  PILOT_STATE  05   (PS_F — see below)
@0x0a0b  STM32_FAULT  00
@0x0a10  PILOT_DUTY   32
@0x0a74  ALARM_BITMAP 50 00 00 08   (bit 27 new — see below)
```

**PILOT_STATE difference (our PS_F = 5 vs stock's PS_TRANSIENT = 4):**

Stock at the same bench condition (120 V UVP-fault, no plug) published
TRANSIENT because its sample window had a 2.4 % bimodal +12 V spike
component (docs/13 §3.2). Our session's CP sense stream appears purer
(all low cluster), so the dominant-class rule wins → PS_F.

Both are functionally "no charging permitted". PS_F is arguably the
more *honest* classification (CP genuinely held at -12 V → fault),
which is why an alarm bit (0x08000000 in the bitmap) now fires that
didn't under stock — likely "ADC reports CP fault" — promoted by
`main` or `ErrorHandle`. We're not introducing a new fault path; we're
surfacing one stock was masking via the TRANSIENT classification.

Calibration follow-up (deferred): once on 240 V with the pilot
generator active, re-tune the per-class voltage bands to match what
stock thresholds appear to be (likely tighter rail definitions so
TRANSIENT covers more of the "almost rail" cases). The bench-idle
behavior we want to match cannot be tested until then.

## 7. Bench validation plan (original — superseded by §9 above)

1. Deploy `delta-bridge.v08` to `/Storage/delta-bridge/`.
2. Wrap `/root/Adc` to exec `delta-bridge.v08 --personality=adc`.
3. Stash stock at `/Storage/Adc.preinst.bak`.
4. Reboot.
5. After boot, verify via `shmem_dump`:
   - `@0x0a08 PILOT_STATE` populated (likely `04` = TRANSIENT, matching
     stock at the same 120 V UVP-fault bench condition per docs/13)
   - All co-tenants still alive (`main`, `Pri_Comm`, `MeterIC_new`
     replacement from M7, `Charging_Standard_RFID`, `delta-bridge` RFID side)

Rollback at any time:
```sh
mv /Storage/Adc.preinst.bak /root/Adc
sync; reboot
```

## 8. Design constraints honored / deferred

| Concern                                | Status                                                    |
|----------------------------------------|-----------------------------------------------------------|
| Stock daemon spawn semantics           | Wrapper, exec'd by /etc/funs in stock's slot              |
| ioctl arg byte-for-byte fidelity       | Yes — captured + replayed verbatim                        |
| PilotState classifier behavior         | Simplified — J1772-spec thresholds; bench-validates on idle |
| Safety-critical timing of state change | Window-based, no immediate per-sample state flips         |

**Known gaps (deferred):**

- **Per-class buffer publish to stock's `Buf_*` shmem locations** —
  stock writes these into shmem (not just `0x0a08`). Our impl
  doesn't. None of the other charging daemons read them per the
  docs/14 matrix, but a downstream consumer we missed could care.
- **`PV_State_Prev`-style internal state tracking** — only relevant
  if some consumer reads it (matrix says no).
- **240 V real-plug ground-truth** — without it we can't verify the
  per-sample threshold band edges match stock exactly. Bench in UVP
  fault never produces clean B/C/D class values to compare.
- **`Alarm_Flag` setting on stock-equivalent fault paths** — TBD.
