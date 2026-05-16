# Pri_Comm — LD_PRELOAD syscall trace addendum

**Date:** 2026-05-16
**Method:** Same LD_PRELOAD shim as `docs/10-rfid-protocol-decoded.md`
(`companion/tools/uart_trace.c`) wrapping `/root/Pri_Comm` over a clean
cold boot. ~90 s of capture against the bench EVMU30 in idle state
(no plug, no current).
**Raw evidence:** `companion/test/data/pri_comm-trace-stock-2026-05-16.log`
(reassembled SLIP frames + collapsed heartbeat runs; 331 lines from
17 690 raw byte-events).

This doc **does not supersede** `docs/01-Pri_Comm-protocol.md` — that
file's wire-RE + static analysis remains the canonical reference for
frame semantics. This addendum adds the bits that need to be observed
*at the daemon's syscall boundary* to see them at all: init syscalls,
daemon parsing strategy, observed cadence, and process-supervision
context.

---

## 1. Daemon lifecycle (syscall-precise)

```
t=79.802762  open("/dev/ttyAMA1", O_RDWR | O_NOCTTY)    → fd=3   (flags=0x102)
t=79.803077  ioctl(fd=3, TCGETS=0x5401, *termios)       → 0     (read existing)
t=79.803264  ioctl(fd=3, TCSETS=0x5402, *termios)       → 0     (apply new)
t=84.814437  write(fd=3, "\xc0\xfc\xd5\xd5\xc0", 5)     → 5     (FC D5 handshake)
t=84.848…    read(fd=3, ., 1) × 17                              (FD D5 response,
                                                                 parsed byte-by-byte)
t=84.849984  write(fd=3, "\xc0\xfb\x80\x80\xc0", 5)     → 5     (start FB 80 heartbeat)
…           (steady state — see §3)
```

- **Five-second gap (79.8 → 84.8) between init and first send.** This is
  the `sleep 5` in `/etc/funs` between `/root/main &` and the second
  daemon batch — `/root/Pri_Comm` is launched by `/root/main`, so it
  inherits the supervisor's deferred-start scheduling. Reproduce this
  in any replacement daemon **if** /root/main expects to wait for the
  STM32 to come up before issuing pilot commands.
- **`TCGETS` before `TCSETS`.** RFID's daemon (docs/10) only did
  `TCSETS`. Pri_Comm reads the current termios first, presumably so it
  preserves baud-rate flags that the kernel may have set differently
  on `ttyAMA1` (which is shared with U-Boot serial output prior to
  Linux taking over). Safest practice in a replacement.
- **No GPIO/PWM init.** The only `system()` calls observed (2 paired
  CLOSE fd=5/10 events at t=84.96 and t=85.95) happen *after* the
  first poll cycle — those are most likely the early `system()` calls
  in `/root/Pri_Comm`'s status-monitoring path (e.g. an `iwconfig` /
  `route` / `cat /proc/loadavg` style command — exact subject TBD).
  The STM32 is on its own reset domain; Pri_Comm doesn't drive a reset
  pin from Linux side.

## 2. Daemon's parsing strategy

The trace shows the daemon issues `read(fd=3, buf, 1)` repeatedly to
consume each frame **one byte at a time**. A 17-byte SLIP frame is 17
separate `read()` syscalls (~120 µs apart). The byte stream looks like:

```
[84.848608] READ fd=3 ret=1 << c0    ← SLIP END (start)
[84.848725] READ fd=3 ret=1 << fd    ← op1
[84.848821] READ fd=3 ret=1 << d5    ← op2
[84.848906] READ fd=3 ret=1 << 10    ← payload[0]
…12 more zero bytes…
[84.849749] READ fd=3 ret=1 << e5    ← checksum
[84.849808] READ fd=3 ret=1 << c0    ← SLIP END (terminator)
```

This is a 17 × `read()` cost per frame plus syscall overhead. We can
easily improve on this in a replacement daemon by using `VMIN=17`
termios (or any larger buffered read), since the response is fixed
length. But for **wire compatibility** it doesn't matter — the STM32
side doesn't care how the host drains its UART.

## 3. Cadence observed

Over 90 s of idle-state capture (881 + 100 + 1 = 982 TX frames):

| Cadence | TX pattern | Count | Avg interval |
|---------|------------|------:|--------------|
| Init    | `c0 fc d5 d5 c0` | 1 | n/a (one-shot) |
| Heartbeat | `c0 fb 80 80 c0` | 881 | ~102 ms (≈ 9.8 Hz) |
| Telemetry | `c0 fb 11 11 c0` | 100 | ~1 000 ms (≈ 1 Hz) |

The heartbeat is the most frequent — it's the host's "are you still
alive, give me ack" — and it's NOT exact 10 Hz; it's
"send next poll immediately when the previous response completes",
which gives ~100–125 ms intervals depending on STM32 turnaround.
Telemetry interleaves once per second.

## 4. Checksum formula re-verified

Against today's new captures (not in docs/01's data set):

```
RX:  c0 fd 11 00 00 07 3d 00 00 0f fb 00 00 00 00 5f c0
     check: (0x11 + 0x07 + 0x3d + 0x0f + 0xfb) & 0xff = 0x16f & 0xff = 0x5f ✓

RX:  c0 fc 80 10 00 00 00 00 00 00 00 00 00 00 00 90 c0
     check: (0x80 + 0x10) & 0xff = 0x90 ✓

RX:  c0 fd d5 10 00 00 00 00 00 00 00 00 00 00 00 e5 c0
     check: (0xd5 + 0x10) & 0xff = 0xe5 ✓
```

Formula from docs/01 stands: **`csum = (op2 + Σpayload) & 0xFF`**, with
`op1` excluded.

## 5. Telemetry-payload byte variability (new observation)

Across 100 telemetry replies in the trace, the structure
`c0 fd 11 00 00 07 BB 00 00 0f CC 00 00 00 00 KK c0` shows two slowly
drifting bytes:

| Position | Range observed | Distinct values | Hypothesis |
|----------|----------------|-----------------|------------|
| `BB` (payload[3]) | `0x38..0x3e` (56..62) | 7 | Slow-changing measurement: temperature reading? Pilot threshold counter? |
| `CC` (payload[7]) | `0xfa..0xfc` (250..252) | 3 | Similar slow drift; tightly clustered. Possible companion register to BB. |

All other payload bytes (`payload[0..2]=00 00 07`, `payload[4..6]=00 00 0f`,
`payload[8..11]=00 00 00 00`) were invariant over 90 s of idle state.
docs/01's static-analysis comments suggest these payload positions map
to: pilot duty, pilot voltage, contactor state, GFCI test result, fault
bitmap. Without exercising the unit (plug-in, fault inject) we can't
distinguish which `payload[i]` corresponds to which. Easy follow-up
when bench is on 240 V again.

## 6. Implication for the broader "use our own binaries" plan

Pri_Comm is the **leaf-most user-side daemon** in the EV charging path
(host ↔ STM32). Replacing it cleanly means re-implementing:

1. The 17-byte SLIP-framed `c0 … c0` protocol with the documented
   opcodes (`FC D5`, `FB 80`, `FB 11`) and the `csum = op2 + Σpayload`
   formula.
2. The ~10 Hz heartbeat + 1 Hz telemetry interleaving.
3. The shmem write of the decoded telemetry (we don't see this in the
   trace because shmat/shmwrite aren't syscalls our shim hooks; needs
   the existing static RE in `docs/01 §"Shmem layout"` for offsets,
   plus the live shmem snapshot diff method from `docs/05 §Bytes that
   DO move` to resolve the bad offsets — same M1 work that's still
   open from `project_eluminocity_companion_bridge` memory).

Compared to RFID (docs/10), Pri_Comm is **easier to replace at the
protocol-layer** (cleaner framing, fewer command types, no PWM-clock
black box) but **harder to replace overall** because it's the safety
control path — `/root/main` and the OCPP stack both depend on
shmem-mediated reads from Pri_Comm's output. Replacing it requires
shmem-write conformance, not just UART-side conformance.

Phasing recommendation (refines `docs/09 §6`):

- **Phase 1** (RFID, docs/10): protocol-layer + standalone publisher,
  no shmem-write conformance required.
- **Phase 2** (this doc + an M1 follow-up): finish the shmem-offset
  rewrite, then re-implement Pri_Comm. This unblocks the rest of the
  "use our own binaries" sequence because every downstream daemon
  reads from the shmem region Pri_Comm produces.
- **Phase 3+**: per docs/09 §6 (Charging_Standard_RFID → secondary
  daemons → main).

## 7. What we still don't know

- **Exact termios bytes in the TCSETS arg.** Same as docs/10 §8. Shim
  doesn't dereference user-pointer args yet.
- **What the 2 `system()` calls do.** They happen post-init, ~150 ms
  apart. Trace doesn't show subject — would need a strace-style
  improvement (or just RE the binary).
- **What `BB` and `CC` mean.** Idle state isn't informative enough.
  Plug-in test on 240 V will discriminate.
- **What FC D5 returns on subsequent calls.** docs/01 says "the STM32
  queues last meter snapshot and delivers it on the first query of a
  fresh session." Our trace had FC D5 sent exactly once, immediately
  after open. Subsequent queries didn't happen in this 90 s window.
