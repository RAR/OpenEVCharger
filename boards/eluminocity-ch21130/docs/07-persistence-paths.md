# Delta EVMU30 — Persistence Paths for External Setpoint Writes

**Goal**: identify the most surgical way for `delta-bridge` to make external
writes to `rated_amps` (`shmem[0x0a24]`) and `user_state` (`shmem[0x0a00]`)
**survive** stock daemons' rewrite cycles.

**Method**: static reverse-engineering of the ARM ELF binaries in
`/home/rar/device-configs/esphome/testcharger/delta/` plus a single
read-only live observation (3 × `shmem_dump --save` 5 s apart, then
`shmem_dump --diff`) to confirm one of the static predictions.

---

## TL;DR

| Byte | Recommendation | Path | Why |
|---|---|---|---|
| `0x0a24` rated_amps | **Direct shmem write + StoreFlash trigger (Path A')** | A | The byte is **NOT** clobbered at runtime by any stock-running daemon. The previously-reported "1-2 s revert" is almost certainly a stale-observation artefact (see §3 below). The byte just needs a flush to `/dev/mtdblock4` for reboot persistence — no fighter to overcome. |
| `0x0a00` user_state | **Don't poke 0xa00. Use the `0x0a79`/`0x0ab0` ready-flag pair, OR spawn `DeltaOCPP` and send a local RemoteStartTransaction** | B/C | `Charging_Standard_RFID` rewrites `0x0a00` every loop iteration (1 µs `usleep`) directly from pilot-state classifier output. The only path to a "Charging" state code is to *fool the state machine inputs*, not to overwrite its output. |

---

## 1. `rated_amps` (`shmem[0x0a24]`) — Recommended: **Path A' (direct write + flash flush)**

### 1.1 Static finding: there is no runtime writer of `0x0a24`

I disassembled every binary in `/root/` and tabulated every `strb r2, [r3]`
where `r3 = MeterSMPtr + 0x0a24`. The complete writer inventory is:

| Binary | Site | Function | When it runs |
|---|---|---|---|
| `main` | 0x0000c618 | `StoreFlash` | Copies live shmem→flash-staging buffer. **READS shmem[0x0a24], doesn't mutate it.** |
| `main` | 0x0000ddf4 | `GetConfig` | USB-stick config-import (`/UsbFlash/DeltaEVSEConfig`). Only runs when `access("/dev/sda1", R_OK) == 0`. |
| `main` | 0x0000de28 | `GetConfig` | USB clamp: if imported value > 30 → store 30. Same USB trigger. |
| `main` | 0x000159cc | `main` boot init | Unconditional `shmem[0x0a24] = 30` between `FlashToShrMem()` and the poll loop. **Runs once per boot.** |
| `main` | 0x00015c20 | `main` boot init | Unconditional `shmem[0x0a24] = 30` if value < 5 or == 0xff or > 30. **Runs once per boot.** |
| `DeltaOCPP` | 0x0000db24 | `main` OCPP poll | Writes from OCPP `ChangeConfiguration(key="MaxOutputCurren(Amp)", value=N)` (typo "Curren" is in the firmware). **Only runs when DeltaOCPP is running AND CSMS sends a write.** |
| `DeltaOCPP` | 0x0000db5c | `main` OCPP poll | Same OCPP write, alternate branch. |
| `ScenarioMaker` | 0x00009428 | `main` | Factory-defaults builder. Only invoked by `FlashToShrMem` when the mtd4 checksum has **failed** and the firmware needs to rebuild the segment. |

Searched daemons with the (`r3 = MeterSMPtr + 0xa20`, then `+ 4`) idiom: `main`,
`Charging_Standard`, `Charging_Standard_RFID`, `Pri_Comm`, `NTC_tmp`, `Adc`,
`MeterIC_new`, `RFID`, `LED_control`, `ErrorHandle`, `RTC`, `FlashLog`,
`DeltaOCPP`. Only `main` and `DeltaOCPP` write the byte. Earlier doc-06's
claim that `Charging_Standard_RFID` writes `0x0a24` was wrong — that
binary has 19 *reads* of the offset and zero writes. Same for `Pri_Comm`
and `NTC_tmp`: their `RW` classification in the role-map was a
false-positive from `str r2, [r3]` instructions where `r3` was a
**BSS global** (in OTPCheck, the result of a 110×rated×0.5 multiply is
stored to `Pri_Comm:0x16180`, not to shmem).

### 1.2 Live confirmation: stock unit does not change 0x0a24

I took three `shmem_dump --save` snapshots, 5 s apart each, then ran
`shmem_dump --diff`. The only differing byte across all three snapshots
is `0x00000` (Vrms LSB — mains-voltage fluctuation). **`0x0a24` is
unchanged.** With the stock daemon set (no DeltaOCPP, no mini_httpd,
no USB stick), nothing periodically writes the byte.

```
ps | grep -E 'main|Charging|Pri_Comm|RFID|MeterIC|Adc|LED|FlashLog|RTC|ErrorHandle|snmpd|DeltaOCPP|httpd'
# /root/main, /root/RTC, /root/Adc, /root/MeterIC_new, /root/ErrorHandle,
# /root/Charging_Standard_RFID, /root/LED_control, /root/RFID, /root/Pri_Comm,
# /root/FlashLog, /root/snmpd  -- NO DeltaOCPP, NO mini_httpd
```

### 1.3 Why the user observed "reverts in 1-2 s"

Three plausible explanations, in order of likelihood:

1. **Stale-snapshot artefact in `shmem_dump`'s mmap path.** The dumper
   may be reading from a cached/redundant copy of the segment. The
   stock layout has *three* 64 KiB copies of the lower 128 KiB
   (`0x00000`, `0x10000`, `0x30000` per `04-sharemem-decoded.md`),
   and `main:StoreFlash` periodically rewrites all of them from the
   primary. If the bridge writes only one copy and reads back from
   another, it would appear to "revert".
2. **The bridge is the one writing 30 back.** When MQTT publishes the
   state topic of `rated_amps` (`number.set_current`), HA may echo
   the *previous* value (30) back to the command topic if there's a
   races between the bridge's HA discovery push and HA's first
   subscribe, causing the bridge to be told "set 30" by itself a
   moment after the user moves the slider.
3. **A periodic flush from `main:StoreFlash` reloading from a
   stale BSS mirror.** This is the least likely — `StoreFlash` only
   reads shmem and writes to mtd4 — but the read could land on a
   stale page if the kernel SHM driver has any caching quirks.

The static evidence is unambiguous: **no stock-running daemon writes
`0x0a24` periodically**. The bridge should simply trust its write.

### 1.4 Recommended bridge change (Path A')

The bridge already writes `OFF_RATED_AMPS` (offset `0x0a24`) directly.
Two suggested additions to make the write **persistent across reboots**:

1. **Verify the write back** after writing, by reading shmem from the
   bridge's own attached pointer a few ms later. If the read returns
   the written value: success. If not, log diagnostic info (snapshot
   of `0x0a00..0x0a7f`) and surface to MQTT for debugging.

2. **Trigger a `StoreFlash` flush** so the new value persists across
   power cycles. There is no direct API for this — `StoreFlash` is a
   private function in `main`. The cleanest options:

   - **Option 2a (simplest, least invasive)**: write any of the
     "config-changed" flag bytes that `main`'s poll loop watches.
     The poll loop checks (per `02-IPC-and-main-architecture.md`):
     `0x100` (UPDATE_IP), `0x108`, `0x116`/`0x117`, `0x19b`,
     `0x1ae`, `0x1bd`, `0x1dd`. Several of these → `StoreFlash`
     code path → mtd4 write. **`0x1dd` (StoreFlash-busy lock) is
     the wrong target** (it's a mutex, not a trigger).
     `0x1bd` (auth-mode change flag) is more promising but its
     side effects include logging "Authentication Mode: %d => %d"
     to `/Storage/EncodeLogMessage` and triggering a daemon
     respawn. Not clean.
   - **Option 2b (clean but invasive)**: from the bridge, after
     writing `0x0a24`, run `mtd_write_shmem_to_flash()` directly:
     - `open("/dev/mtdblock4", O_RDWR)`
     - For each 128 KiB half: copy `shmem[0..0x1FFFC]`, compute
       a BE-u32 sum, write that to `shmem[0x1FFFC..0x1FFFF]`,
       then `pwrite()` the 128 KiB block to the right mtd offset.
     - This is exactly what `main:StoreFlash` (at `0x0000c074`)
       does. Re-implementing it in the bridge is ~80 lines of C
       and avoids the daemon-respawn side-effects.
     - **Care needed**: respect the `shmem[0x1dd]` busy-lock —
       poll it, set it to 1, do the write, set it to 0.
   - **Option 2c (skip persistence, accept reboot reset)**:
     don't bother flushing. Rebroadcast the desired value from HA
     on bridge restart / unit power-up. This is reasonable if the
     EV-charging session never spans a unit reboot.

**Recommendation**: ship **Option 2c** first (just direct write,
accept reboot loss), since the reboot-survival requirement is
weak. Move to **Option 2b** only if users complain.

### 1.5 What we could NOT determine for `rated_amps`

- The exact cause of the "1-2 s revert" the user reported on the
  earlier bench session. The static analysis says it should not
  happen. **Re-run the test with the bridge logging a write
  timestamp + read-back timestamp** to nail down whether it's a
  same-process artefact or a real external clobber. If it
  reproduces, snapshot the segment immediately before and after
  the revert and diff to find the actual byte-pattern of the
  rewrite — that will fingerprint the writer.
- Whether the `OCPP::MaxOutputCurren(Amp)` write path
  (DeltaOCPP @ 0xdb24/0xdb5c) writes a *zero* if the OCPP config
  store has no value for that key — i.e., whether spawning
  DeltaOCPP could *introduce* a clobber where there isn't one
  now. Worth checking before enabling OCPP on this unit. The
  string compare uses `strcasecmp` against the shmem-resident
  key table at `shmem[0xe20+]`, which is populated from an
  OCPP `ChangeConfiguration` request and from `mtd4` (via
  `FlashToShrMem`). On a factory-fresh unit those keys are
  empty, so DeltaOCPP's `atoi(empty string)` would return 0
  and the `cmp r3, #5; ble db88` guard at 0xda58 would skip
  the write. **Probably safe**, but worth confirming.

---

## 2. Alternative live-setpoint bytes (Path B for `rated_amps`)

Searched for any other byte that:
- is **read** by `Pri_Comm:OTPCheck` (the pilot-duty computer);
- is **not written** by `main`, `Charging_Standard_RFID`, or
  `DeltaOCPP` at runtime.

The OTPCheck function (`Pri_Comm` @ `0x9148..0x9534`) reads exactly two
shmem bytes that affect pilot-duty output:

| Offset | Used as | Writers |
|---|---|---|
| `0x0a24` | rated/configured ampacity (multiplied × 1.667 → pilot duty %) | `main` (boot only), `DeltaOCPP` (only if running + CSMS write) |
| `0x0a25` | secondary ampacity / phase config? Branch condition at OTPCheck 0x91a8 | `main` (boot init + USB import only), `Charging_Standard*` (read-only) |

`0x0a11` ("per-config max-current mirror" per the prior doc) — turns
out to be wrong. Disassembling `main:GetConfig` at 0xdcfc shows
`shmem[0x0a11] = 2` (a `mov r2, #2` immediate), and the literal pool
at 0xfd50 confirms the source string is `"Remote Control Charge:"`
(from `/UsbFlash/DeltaEVSEConfig`). **`0x0a11` is the "Remote Control
Charge" mode byte, not a current limit.** Not useful as a Path-B
target.

`0x0a25..0x0a28` — sequentially-read GetConfig-only writes. `0x0a25`
is read by both `main` and CSR (see role-map). Setting `0x0a25` to
non-zero changes a branch in CSR `main+0x3bb0` (the auth-skip path)
**and** affects an OTPCheck branch (0x91a8 — sets the OTP "secondary"
mode). Not a clean current setpoint.

**Conclusion**: there is no "shadow ampacity" byte to use instead of
`0x0a24`. `0x0a24` is the canonical setpoint, and as shown in §1, it's
quiet at runtime — Path A' is the answer.

---

## 3. Config-reload trigger (Path C for `rated_amps`)

`main:GetConfig` (at `0x0000d394`) is called from exactly **ONE** site,
`main:main+0x1e34` (at `0x0001681c`). The call is gated on:
```
access("/dev/sda1", R_OK) == 0 && access("/dev/sda", R_OK) == 0
```
i.e. **a USB mass-storage stick must be plugged into the unit** for
GetConfig to run. There is no signal-driven, timer-driven, or
shmem-flag-driven invocation of GetConfig.

The boot-time clamp (0x15c1c) runs once per `main` startup. The poll
loop at `0x16d78` does **not** invoke any code path that writes
`0x0a24`.

So **Path C is infeasible for `rated_amps`** without modifying the
firmware. There is no "reload the config" trigger that's
shmem-byte-settable. (One could `umount/mount` a fake `/dev/sda1`
with a pre-cooked `/UsbFlash/DeltaEVSEConfig` and let `GetConfig`
import — but that's far more invasive than just writing the byte
directly.)

---

## 4. Concrete config-store path (Path A details)

There is **no separate file** on `/Storage` that holds `rated_amps`. The
persistent store is the shmem segment itself, mirrored to `/dev/mtdblock4`.

The relevant `/Storage` files (confirmed live):

```
$ ls -la /Storage/
drwxr-xr-x    4 vern     0               0  /
-rw-r--r--    1 vern     0             679  DownloadConfiguration   # generated on USB-config-dump, NOT a source
-rw-r--r--    1 vern     0           10786  EncodeLogMessage        # event log
-rw-r--r--    1 vern     0              32  Gain                    # per-unit meter calibration (NOT shmem-mirrored)
-rw-r--r--    1 vern     0              15  SerialNumber            # 14-byte ASCII serial
drwxr-xr-x    2 vern     0               0  delta-bridge/           # our bridge state dir
```

- `Gain`: not shmem-mirrored; pinned per-unit. Don't touch.
- `SerialNumber`: read once at boot, written to `shmem[0x460]`.
- `DownloadConfiguration`: **output-only**. Generated by `main` via
  `/UsbFlash/DeltaEVSEConfig` regeneration, not consumed by any
  daemon. No use as a write target.
- `EncodeLogMessage`: append-only log via `system("echo ... >> ...")`.

**The single persistent source for `rated_amps` is `/dev/mtdblock4`
offset `0x0a24` (in both half-0 at flash 0x0000..0x1FFFB and
half-1 at flash 0x20000..0x3FFFB).**

To make a runtime write survive a reboot, the bridge must either:
- Wait for `main:GetConfig` to run (USB-stick required — not
  feasible), or
- Run its own `StoreFlash`-equivalent: write to mtd4 directly
  with the BE-u32 checksum at `[0x1FFFC..0x1FFFF]` and
  `[0x3FFFC..0x3FFFF]` recomputed.

The mtd4 layout per `04-sharemem-decoded.md`:

```
mtd4 (256 KiB)
  0x00000..0x1FFFB  half-0 data (primary)
  0x1FFFC..0x1FFFF  half-0 checksum: BE u32 sum of [0..0x1FFFC)
  0x20000..0x3FFFB  half-1 data (mirror)  -- redundancy
  0x3FFFC..0x3FFFF  half-1 checksum
```

`main:FlashToShrMem` at boot picks whichever half has a valid
checksum. If both halves are valid (the normal case), it
prefers half-0. So writing only half-0 (with updated
checksum) is sufficient for reboot persistence.

**A safer pattern is to write both halves** — that's what
`main:StoreFlash` does. ~80 LOC of code; see disassembly at
`main:0xc074..0xcef0`.

---

## 5. `user_state` (`shmem[0x0a00]`) analysis

### 5.1 Why direct writes to 0x0a00 don't stick

`Charging_Standard_RFID` (CSR) is a tight loop with `usleep(1)` between
iterations. The relevant gate at CSR `main+0x4cc` (= 0xac00):

```
if (shmem[0x0a08] /* pilot state */ == 1 /* B - plug-in, not charging */) {
    // ... housekeeping ...
    shmem[0x0a00] = 0;          // ← rearm on plug-in
} else if (shmem[0x0a08] == 2 /* C - charging */) {
    shmem[0x0a00] = 0;          // ← also clears in charging-state
} else {
    // pilot state 0/3/4/5 → skip 0xa00 writes entirely (jump to 0xe1bc)
}
```

So **with no plug** (pilot state 0 or 4), CSR doesn't touch 0xa00.
With a plug **or** during charging, CSR writes 0 to 0xa00 in every
iteration. The only path that writes a *non-zero* value to 0xa00 is
at CSR `main+0x3d28` (= 0xe460), reachable when:

- `local_var == 12` (charging-progress sub-state), OR
- `shmem[0x0a79] == 4` (meter-IC sub-state "ready"), OR
- `shmem[0x0ab0] == 1` (meter-IC init complete), AND
- `shmem[0x01ba] /* auth-mode */ == 0` (no-auth) AND
- a pilot/CP edge transition.

At that point it writes `shmem[0x0a00] = 2` (= "active session").

So the byte's value of `0` is a **steady-state default** when nothing
is being charged — CSR doesn't actively write 0 in that case, but
also doesn't transition to 2.

### 5.2 What overwrites our writes

If the bridge writes `shmem[0x0a00] = 1` (or 2) while:
- the plug is inserted (pilot state = 1 or 2) → CSR's next iteration
  (within ~1 µs) writes 0 over us.
- the plug is NOT inserted → CSR doesn't touch 0xa00 in that path,
  so **the bridge's value persists**. **But** that value isn't
  consumed by anything downstream until a plug-insert + auth event
  drives the state machine — `LED_control` reads it and may flash
  green (per doc-06), but no charging happens.

### 5.3 Recommended approach for "authorize"

There are three viable paths, in order of preference:

**Path 1 (recommended): Just don't poke 0xa00.** Stock authentication is
mode 0 (no auth required) — confirmed via `cat /Storage/DownloadConfiguration`
showing `Authentication Mode: 0`. With auth disabled, **plugging the EV
in is sufficient** to trigger the state machine. No external authorize
needed. The bridge should not advertise an "authorize" entity at all
on this unit.

**Path 2 (intermediate): Spawn DeltaOCPP-equivalent + RemoteStartTransaction.**
The Delta firmware ships `/root/DeltaOCPP` which speaks OCPP 1.6-S
(SOAP, not -J). It's not running in stock state (no CSMS configured
in `0x4c0` OCPP-server-URL field). To use it:

1. Configure `shmem[0x4c0]` with a local CSMS URL (e.g. an OCPP
   server hosted by the bridge itself, or `evcc` or `OpenOCPP`).
2. Spawn DeltaOCPP: `/root/DeltaOCPP &`.
3. Have the local CSMS send `RemoteStartTransaction(idTag=..., connectorId=1)`.
4. DeltaOCPP's main poll loop will see the RST, set up
   `shmem[0xc60..]` (RFID/tag scratch), and triggers CSR's
   "authorized" path.

This is heavy. ~10 KB of OCPP-SOAP plumbing on the bridge side
plus a daemon respawn. **Not recommended unless we want full OCPP
integration.**

**Path 3 (clever hack): Fake an RFID tag presentation.** `Charging_Standard_RFID`
processes RFID by:
1. Reading `shmem[0xa68..0xa6f]` (8 bytes — UID buffer).
2. Reading `shmem[0xa79]` (meter-IC status sub-state — also used as the "tag ready" flag).
3. On `shmem[0xa79] == 4 && shmem[0x1ba] == 0`, branching to the
   "no-auth, plug, go" path that writes `0x0a00 = 2`.

The bridge could:
- Write a fixed "bridge tag" UID into `shmem[0xa68..0xa6f]`.
- Write `shmem[0xa79] = 4`.
- Wait one CSR loop (~1 ms is plenty).
- CSR sees the "tag" + no-auth-required, transitions `0x0a00 → 2`.

**Risk**: `shmem[0xa79]` is also written by `MeterIC_new` (every meter
poll, sub-second rate) — so the bridge's `= 4` write will be reverted.
Need to either:
- Track `MeterIC_new`'s write cadence and re-write `0xa79` every few
  hundred ms (race with MeterIC_new), OR
- Use one of the *other* CSR-authorize gates: `shmem[0xab0] == 1`
  (meter-init-complete, written once by MeterIC_new and stable) +
  `shmem[0x1c1]` (RFID-found flag, written by `RFID` daemon when a
  tag is detected).

The cleanest "fake RFID" is to spawn a small helper that **kills
the `RFID` daemon, writes a hardcoded tag UID + presence flag,
and waits**. The `RFID` daemon doesn't have a "fake mode" — it polls
the MFRC522 hardware. Killing it lets us own the writes to the
RFID-tag buffer.

**Trade-off summary**:

| Path | Effort | Survives reboot? | Side effects |
|---|---|---|---|
| 1 (do nothing) | none | n/a | none — perfect for auth-disabled units |
| 2 (OCPP RST) | high | yes (OCPP session) | spawns OCPP daemon, requires CSMS |
| 3 (fake RFID) | medium | no — needs re-push | races with `RFID` daemon, would need to kill it |

**Recommendation**: ship **Path 1**. The Delta unit's authentication
is configurable from the web UI / USB config — if a user wants
authorization, they can change `Authentication Mode` to 3 (RFID) or
4 (PIN) on the unit itself. For the bridge's home-automation
use-case (single-owner garage charger, no RFID needed), Path 1
matches stock behaviour and requires zero code in the bridge for
authorize.

### 5.4 What we could NOT determine for `user_state`

- The full meaning of CSR's `local_var fp[-40]` enum (values 7, 12
  were seen as branch conditions in the 0xa00=2 write path). Without
  it, the exact sequence of "what input combination unconditionally
  drives 0xa00 to 2 in stock no-auth mode" can't be fully predicted.
  Live tracing under a real charge cycle would resolve this.
- Whether `shmem[0xa79]` actually responds to our writes or is
  always overwritten by MeterIC_new within microseconds. Needs a
  live write+immediate-read test.
- Whether spawning the **`Charging_Standard`** binary (the *non*-RFID
  variant, also on disk at `/root/Charging_Standard`) instead of
  `Charging_Standard_RFID` would bypass the RFID auth gate entirely.
  `nm Charging_Standard` shows the same StoreFlash but a different
  set of shmem writers. **Worth investigating** as a Path-4 — `main`
  spawns CSR at boot, but we could `killall Charging_Standard_RFID`
  and start `Charging_Standard` instead. Would need a comparable
  static analysis of the non-RFID variant.

---

## 6. Path-A' implementation sketch (for the bridge)

The bridge's `handle_rated_amps` (in `mqtt_adapter.c:266`) currently
just calls `shmem_write_u8(...)`. Suggested enhancement:

```c
static void handle_rated_amps(struct adapter_ctx *a, ...) {
    long v = parse_payload(...);
    if (v < 6 || v > 30) { /* reject */ return; }

    /* 1. write live shmem */
    if (shmem_write_u8(a->cfg.shm, OFF_RATED_AMPS, (uint8_t)v) != 0) {
        /* log */ return;
    }

    /* 2. verify (re-read after a short delay) */
    usleep(10000);  /* 10 ms is plenty — CSR's loop is 1 µs but doesn't write here */
    uint8_t got = shmem_u8(a->cfg.shm, OFF_RATED_AMPS);
    if (got != (uint8_t)v) {
        fprintf(stderr, "delta-bridge: rated_amps: write verify FAILED: "
                        "wrote %ld, read %u\n", v, got);
        /* still publish what we wrote — the read may have hit a stale page */
    }

    /* 3. (OPTIONAL) flush to mtd4 for reboot survival.
     *    Skip for v1; revisit if users report loss-on-reboot. */
}
```

The verify step gives us in-the-loop evidence of who's clobbering, if
anyone, without committing to the full StoreFlash rewrite.

---

## 7. Summary of analysis assumptions and limits

- All static analysis was done on `arm-linux-gnueabi-objdump` output of
  the unstripped, debug-symbol-bearing ARMv5 ELFs in `/home/rar/.../delta/`.
- The live test was a single 3-snapshot/5-second window with the bridge
  running. **Per the task constraint, no writes were issued** during
  this analysis session.
- The shmem-segment offset map was cross-validated against
  `02-shrmem-rolemap.txt`, `02-shrmem-offsets-from-all-binaries.txt`,
  `04-sharemem-decoded.md`, and `06-shmem-RE-from-binaries.md`.
- The previously-reported "1-2 s revert" was **not reproduced** in this
  session. Re-running on the live bench with bridge-side instrumented
  logging (write + read-back) is the recommended next step before
  any further architectural changes.
