# RFID bench findings + paths forward

**Date:** 2026-05-15 (end-of-session)
**Status:** v0.5 RFID module shipped to PR #14 — bench-blocked. Card reader works
under stock firmware; doesn't work under our daemon alone. Diagnosis points to
the PWM peripheral being a live clock for the reader chip, not just the buzzer.

This doc captures everything learned this session so tomorrow's resume doesn't
have to re-trace the dead ends. **Path B below is the eventual goal: replace
stock binaries with our own.** Path A is a quick-win snoop that may unblock
tomorrow's testing while we work on Path B.

---

## 1. What we know about the hardware

- **Reader is on `/dev/ttyAMA4`** at 115200 8N1, talking an XOR-framed
  protocol `[LEN][CMD][PAYLOAD…][XOR][0]`. Confirmed by:
  - `strings /root/RFID | grep tty` → `/dev/ttyAMA4`
  - `lsof`-equivalent (`/proc/<pid>/fd`) on the stock daemon → fd → `/dev/ttyAMA4`
  - Our v0.5 reader successfully opens it.
- **Reader-chip identity is unknown** but speaks the proprietary protocol
  above — confirmed valid by stock daemon disassembly (cmd `0x20` =
  `Request_CardSN`, `0x41` = `UL_read`, `0x11` = `Set_Antana`).
- **Antenna and clock are NOT UART-controlled.** Stock binary contains
  `Set_Antana` function (`0x8f98`) but **it is never called from `main()`**.
  Confirmed by callsite grep: only data reference (`.word 0x00012370` at
  `0x8f94`), zero `bl Set_Antana` instructions.
- **What actually brings the reader to life:** stock `main()` runs 11
  consecutive `system("echo … > /sys/class/gpio/…")` calls + `PWM_Init()`
  once at startup. Specifically:
  ```
  gpio 48 → out, 1
  gpio 57 → out, 1
  gpio 56 → out, 0
  gpio 55 → out, 0
  /dev/spr320_pwm1 → write 8 bytes (0x9a 0x5b 0x06 0x00 0x9a 0x5b 0x06 0x00)
                     i.e. duty=period=0x00065B9A
  ```
- **Critical kernel-driver bug**: the SPEAr3xx PWM driver crashes on
  close+reopen. Witnessed today: after our bridge ran, restarting stock
  `/root/RFID` triggered:
  ```
  Unable to handle kernel NULL pointer dereference at virtual address 00000008
  PC is at do_raw_spin_unlock+0x10/0xac
  spr320_pwm1_open → pwm_request → crash
  ```
  Means: whoever opens `/dev/spr320_pwm1` first **must hold it for the unit's
  uptime**. Any process that opens it after the previous holder closed
  triggers the oops.

## 2. What we tried + what failed

### v0.5 — kill stock RFID, take over UART, poll for UIDs

- Built, deployed, ran. Bridge opens `/dev/ttyAMA4` cleanly, polls `0x20`
  at 5 Hz. **Got zero response from the reader.**
- Hand-sent the exact stock-equivalent `Request_CardSN` bytes
  (`03 20 00 23 00`) via `dd` to `/dev/ttyAMA4`. **Zero response.**
- Reader is silent → either dead, in reset, or unclocked.

### Misdiagnosis: try sending Set_Antana via UART

- Added a `cmd 0x11` "antenna on" emission at our daemon startup, mirroring
  stock's `Set_Antana` function exactly.
- Deployed. **No change.**
- Then disassembled stock `main()` and found `Set_Antana` is dead code.
  Reader chip's antenna isn't UART-controlled. Reverted the patch
  (commit `eeec751` — kept the comment for the next person).

### After-the-fact theory we couldn't bench-confirm: PWM is a reader clock

The reader's clock or wake-keepalive likely comes from the PWM signal on
`spr320_pwm1`. The 0x00065B9A duty=period setting → that's a continuous
HIGH (since duty equals period, 100% duty). Probably just a GPIO-as-clock
trick where the kernel's PWM peripheral is the only easy way to drive a
sustained signal on that pin.

Without that signal, the reader's analog front-end can't generate the
13.56 MHz RF carrier and cards never get energised.

This explanation fits everything observed today:
- Stock works at boot (PWM is on)
- Killing stock → reader stops working (PWM was being held open by stock
  → fd close → kernel pwm_disable runs → carrier dies)
- Our reader can't pick up where stock left off (no PWM running, can't
  open spr320_pwm1 because of the kernel bug)
- Hand-sending UART bytes did nothing (chip is asleep, not just deaf)

### Final bench state today

- User rebooted unit. Stock `/root/RFID` auto-started. They held a non-Delta
  card to the reader → **nothing logged to `/Storage/IdTagToBeVerify`** ←
  this is consistent with stock's DETA-prefix filter rejecting the card.
- User killed `/root/RFID`. Started our v0.5 bridge. Tapped card →
  **nothing**, consistent with the PWM-died theory.

The decisive test we **did not run**: with stock `/root/RFID` running,
inspect `shmem[0x05E0]` to see if stock writes the UID *before* or *after*
the DETA check. If before, we can snoop without touching UART/PWM at all.

---

## 3. The decisive test we still need (RUN THIS FIRST TOMORROW)

After reboot, **don't kill stock RFID**. Hold a non-Delta card on the
reader. Then:

```sh
python3 /tmp/delta-cmd.py 'cd /Storage && ./shmem_dump > /tmp/snap.bin ; dd if=/tmp/snap.bin bs=1 skip=$((0x05e0)) count=32 2>/dev/null | hexdump -C'
```

(Or via the existing dumper:)

```sh
python3 /tmp/delta-cmd.py 'cd /Storage && ./shmem_dump 5 | grep -E "@05e0|@05f0|@0a79|@0aae"'
```

Three possible outcomes:

| `shmem[0x05E0]` content | Meaning | Path |
|---|---|---|
| ASCII-hex UID (e.g. `30 34 41 42 ...` = `"04AB..."`) | Stock writes UID before DETA check; our card IS being read by the hardware | **Path A** below (small fix) |
| All zeros / no change | DETA check is upstream of the shmem write | **Path B** below (rewrite) |
| Garbage / random | Reader is actually broken | Hardware investigation, not software |

---

## 4. Path A — snoop `shmem[0x05E0]`, leave stock running

**Assumes outcome 1 above.** Smallest delta from v0.5.

### Bridge change
- Add `rfid_mode = snoop|takeover` config key (default `snoop`).
- Snoop mode:
  - Don't kill `/root/RFID`.
  - Don't open `/dev/ttyAMA4`.
  - Poll `shmem[0x05E0..0x05F3]` for non-zero ASCII content + watch the
    `0x0A79` / `0x0AAE` change events (the agent's RE said these are the
    "UID state" and "event flag" bytes).
  - Same MQTT publish path: retained `last_uid` + non-retained
    `scan_event`.
- Takeover mode keeps the current v0.5 code (still useful if path B happens).
- PWM doesn't get touched.

### Pros
- ~100 lines of code change.
- No kernel-driver-bug risk.
- Reader keeps working for stock's auth pipeline too — both paths coexist.
- Bench-testable from existing state.

### Cons
- Still depends on stock's `/root/RFID` being alive (memory + cpu).
- Still uses stock's UART read loop — we don't really "own" RFID.
- Doesn't progress the bigger "use our own binaries" goal.

---

## 5. Path B — replace stock `/root/RFID` (and eventually more)

This is the **user's stated goal**: "I'd like to get us to a point we're
using our own binaries."

The full replacement needs three things our daemon currently lacks:

### B.1 GPIO init (easy)
Replicate the 11 system() calls from stock `main()`. Already documented in
section 1. Idempotent — safe to run on every bridge startup. Stock won't
fight us because we're setting the same values.

### B.2 PWM init (tricky — kernel bug constraint)
- Open `/dev/spr320_pwm1` once at bridge startup.
- Write 8 bytes `9a 5b 06 00 9a 5b 06 00` (duty + period both = 0x00065B9A).
- **Never close the fd.** Has to live for the bridge's process lifetime.
- Constraint: only works if **nothing else opened PWM first this boot**.
- That means: stock `/root/RFID` MUST NOT auto-start. Otherwise stock
  opens PWM first; we can't take over.

### B.3 Stop stock `/root/RFID` from auto-starting
Need to find what launches it. Suspects:
- `/etc/rc` runs `/etc/funs` which forks `main`, `RTC`, `Adc`,
  `MeterIC_new`, `snmpd`, `ErrorHandle`. **Notably NOT `/root/RFID`**.
- `Charging_Standard_RFID`, `LED_control`, `Pri_Comm`, `FlashLog`,
  `wpa_supplicant`, `udhcpc` also aren't in `/etc/funs` but are running.
- So somewhere ELSE launches `/root/RFID` — almost certainly `/root/main`
  forks it.

Confirm with `strings /root/main | grep -E '/root/RFID|RFID&|exec.*RFID'`
or grep the main binary for the substring `"/root/RFID"`.

Options to stop the auto-launch:
1. **Modify `/etc/funs`** — but it doesn't reference RFID. We'd need to
   modify `/root/main` instead.
2. **Replace `/root/RFID` binary** with a drop-in. Stock `/root/main` still
   forks it, but the impostor we wrote runs (does PWM/GPIO init, polls
   UART, skips DETA, writes shmem same as stock).
3. **Symlink `/root/RFID` → `/Storage/delta-rfid`** — same idea, allows
   keeping our binary in `/Storage` where it's update-friendly.

Option 2 or 3 is the cleanest. `/root` is on `mtdblock5` (rootfs).
**Open question:** is rootfs mounted read-write? `/etc/rc` does
`chmod 777 /dev/mtdblock5` on the block device but the mount opts aren't
visible. Need to check with `mount | grep /root` or
`cat /proc/mounts | grep /` on the live unit.

If rootfs is read-only, we can still **bind-mount** our binary over
`/root/RFID` from a writable location:
```sh
mount --bind /Storage/delta-rfid /root/RFID
```
Doesn't survive reboot unless added to a startup script — but
`/Storage/init.sh` (or similar) called from `/etc/funs` after the bind
would survive.

### B.4 The actual replacement binary
Two implementation options:

**B.4a — Standalone `delta-rfid` daemon** (separate process). Reuses our
existing `rfid.[ch]` parser. Adds GPIO + PWM init. Writes shmem like stock
would (`0x05E0` ASCII UID, `0x0A79 = 3`, `0x0AAE = 1`) so the stock
`Charging_Standard_RFID` auth pipeline still works for HA-managed
allowlists. Skips the DETA check entirely.

**B.4b — Fold into `delta-bridge` itself.** What we did for v0.5, but with
the GPIO/PWM init added. Same kernel-bug constraint (PWM fd lives forever).
Doesn't write back to shmem — publishes directly to MQTT.

I lean toward **B.4a** — keep daemons single-responsibility. Easier to
mix-and-match (e.g., user wants stock charging logic but our RFID, or
vice versa). Easier to debug. Smaller binaries, cleaner config.

### Pros (Path B)
- Eventually achieves the "use our own binaries" goal.
- DETA filter bypassed cleanly.
- Foundation for replacing other stock daemons later (the same
  pattern: write a drop-in, install via `/Storage`, bind-mount or
  symlink into `/root`).

### Cons (Path B)
- Bigger lift — needs GPIO init, PWM init, kernel-bug-aware lifecycle,
  binary swap path, possibly rootfs-write workaround.
- Higher bench-risk — kernel oops can require power-cycle.
- Subject to the kernel close+reopen bug if our binary ever crashes
  and respawns — would brick PWM until reboot.

---

## 6. Bigger-picture: "use our own binaries"

The user's stated direction. Phased plan:

### Phase 1 (now): RFID daemon
Replace `/root/RFID` (Path B above). Easiest because RFID is loosely
coupled — its only output is shmem + a file. Other daemons consume it
but don't fork it.

### Phase 2: replace `/root/RFID`'s consumer too
Replace `/root/Charging_Standard_RFID` with our own auth state-machine
that reads the same shmem bytes we wrote and decides accept/deny. At
this point we own the entire RFID → auth → publish-to-HA loop. The
existing v0.3 `set/authorize` HA switch becomes meaningful again.

### Phase 3: replace the secondary daemons
`LED_control`, `FlashLog`, `RTC` — small, single-purpose, low risk.

### Phase 4: replace `/root/Pri_Comm`
The ttyAMA1 protocol to the STM32F334 safety MCU. Most safety-critical
piece outside of the MCU itself. Should not be done lightly.

### Phase 5: replace `/root/main`
The big one. EVSE state machine, OCPP, J1772 logic. Probably the right
endpoint but a long road. We may decide to never do this and instead
fork `main` minus the cloud parts.

### Phase 6: skip `/root/snmpd`
Not needed if HA + MQTT is the only consumer. Just don't auto-start it.

---

## 7. End-of-session bench state

- **Unit uptime ~6 min** as of last check.
- **Running daemons:** `/root/main`, `/root/RTC`, `/sbin/watchdog`,
  `/root/Adc`, `/root/MeterIC_new`, `/root/ErrorHandle`,
  `/root/Charging_Standard_RFID`, `/root/LED_control`, `/root/Pri_Comm`,
  `/root/FlashLog`, `/root/snmpd`, `/root/wpa_supplicant`, `/sbin/udhcpc`.
- **`/root/RFID` NOT running** — user killed it manually.
- **`delta-bridge` last state:** the v0.5.1-with-antenna-cmd binary is on
  disk at `/Storage/delta-bridge/delta-bridge` (md5
  `663b933871276e44525796a66ba00d11`). User may or may not have it
  running by tomorrow — the antenna cmd is harmless so don't worry
  about replacing it before the next test.
- **PWM driver state after the kernel oops earlier this session:**
  unknown. The reboot should have reset it, but the user only rebooted
  ~6 min before final exchanges. Likely fine now.
- **Web UI at** `http://10.75.1.244:8080/` (admin/bench) if bridge is up.

## 8. Open PRs

- **#11** — M1 shmem offset rewrite. MERGED.
- **#12** — M2 write controls. MERGED.
- **#13** — M3 web UI. MERGED.
- **#14** — M4 RFID daemon. **OPEN**, CI green. Bench-blocked per this
  doc. Decision pending: merge as-is (v0.5 ships the framework even if
  it can't be useful standalone without Path A/B follow-up) or hold.

## 9. Tomorrow's first move

In strict order:

1. **Reboot the unit.** Clean state.
2. **Don't kill stock `/root/RFID`.** Let it run.
3. **Hold non-Delta card on reader.**
4. **Inspect `shmem[0x05E0..0x05F3]`** for ASCII-hex UID.

Then branch on the result:

- **UID present** → implement Path A (`rfid_mode = snoop`). Small commit,
  ship as v0.5.1, merge PR #14.
- **UID absent** → commit to Path B. Steps: (a) confirm where
  `/root/RFID` gets launched (`strings /root/main`), (b) write a
  drop-in replacement that does GPIO + PWM init + DETA-less polling,
  (c) figure out the rootfs-write story, (d) deploy + bench-test.

Either way, capture the result in a follow-up doc (`10-rfid-…`) and
update PR #14 description.

---

## 10. Things we still don't know

- **Reader chip identity.** The UART protocol is proprietary. No
  obvious MFRC522 / PN532 / CR95HF signature in strings. Could be a
  small MCU running custom firmware on the daughterboard.
- **Whether stock writes `shmem[0x05E0]` before or after DETA check.**
  The decisive test (§3).
- **Whether rootfs is mounted read-write.** Affects deploy strategy
  for Path B.
- **Whether the SPEAr3xx PWM driver close+reopen bug is in our kernel
  build only or upstream.** If upstream, we can't fix it. If our build,
  a kernel rebuild is available but expensive.
- **What `Charging_Standard_RFID` does with a UID that was written to
  shmem but isn't a Delta card.** RE-doc-08 says it appends to
  `/Storage/IdTagToBeVerify`, but didn't trace whether stock RFID's
  filter prevents that write in the first place.


---

## 11. Morning-after — the decisive test, executed

**Date:** 2026-05-16 morning, fresh reboot (uptime ~99 s at first snap).
**Procedure exactly as §9:** rebooted → stock `/root/RFID` left running →
non-Delta card held on reader → full 256 KiB shmem snapshots taken before
card, with card, and after card removed → 3-way diff.

### Snapshots

| Snap | When | shmem source file |
|------|------|-------------------|
| s0   | no card, post-boot | `/tmp/s0_nocard.bin` |
| s1   | card seated        | `/tmp/s1_card.bin`   |
| s2   | card removed       | `/tmp/s2_nocard.bin` |

### 3-way diff (full output)

```
offset   [s0] [s1] [s2]
0x00000  9d   72   73     ADC oscillator (drifting, ignore)
0x001d4  bc   bf   bf     unknown 1-byte drift (ignore)
0x00362  00   ff   ff     RSSI s32 LE
0x00363  00   ff   ff       ┘ −58 in s1, −56 in s2 — matches WiFi
0x00364  00   ff   ff       ┘ log "RSSI from -72 to -59" timing
0x00365  00   c6   c8       ┘ confirmed not RFID
0x00a70  00   01   01     RFID activity latch (sticky 0→1, not cleared
                          when card removed) — NOT the card-present flag
```

**Seven bytes changed across the full transition. None of them are a UID.**
Verified the candidate regions explicitly:
- `shmem[0x05E0..0x05F3]` (RE-doc-08's candidate) — all zeros in all 3 snaps
- `shmem[0x0A68..0x0A77]` (alt candidate) — all zeros except the sticky bit at `0x0A70`

### Other negative evidence

- `/Storage/IdTagToBeVerify` — does not exist before or after.
- `/Storage/EncodeLogMessage` — size 11502, unchanged across card transitions.
  Tail contains only WiFi RSSI lines, no RFID entries.
- `ipcs -m` — exactly ONE shmem segment exists (key 0x153E, our target,
  nattch=11). No alternate IPC channel for stock to use.

### Verdict

**Path A is dead.** Stock `/root/RFID` does not surface card UIDs into shared
memory (and there is no other shmem to surface them into). The reader
hardware does detect the card — the sticky bit at `0x0A70` flipped at the
moment we presented one — but the UID never reaches a place we can read
without taking over the UART ourselves.

The decisive observation: the bit at `0x0A70` is a one-way activity latch,
**not** a per-card present/absent flag. So even the trivial "did stock see
a card just now?" inference isn't reliable on this offset.

**Committing to Path B** (replace stock `/root/RFID`). See §5 for the plan
and §6 for the broader phased "use our own binaries" direction. The next
work item is what §5 calls out: confirm where `/root/RFID` gets launched,
write a drop-in replacement, deploy via the rootfs-write story (mtd write
or `/Storage`-shadow if the launcher checks both).

### What we still don't know (was §10 #2, now answered)

- ~~**Whether stock writes `shmem[0x05E0]` before or after DETA check.**~~
  ✅ Resolved: stock never writes it. Either filter-rejects before the
  shmem write step, or the snoop offset RE doc was wrong.
