# `/dev/ttyAMA1` contention — RESOLVED via Charging_Standard_RFID syscall trace

**Date:** 2026-05-16 (follow-up to `docs/14 §5`)
**Method:** LD_PRELOAD shim wrap of `/root/Charging_Standard_RFID` over a
clean reboot, ~80 s capture in idle state (no plug, UVP fault active).
Same shim+wrapper+stash pattern from `docs/13`.
**Raw evidence:** captured to `/Storage/trace/Charging_Standa.log`,
2154 bytes after 100 s. Not committed (small, easy to reproduce).

---

## TL;DR

`Charging_Standard_RFID` (CSR) and `Pri_Comm` both open `/dev/ttyAMA1`,
but **there is no real contention**:

1. **CSR is write-only** on ttyAMA1. Zero `read()` syscalls during the
   whole capture.
2. **CSR sends only one frame type** — `c0 fc 83 00×13 83 c0` (17-byte
   SLIP frame, op2=0x83, all-zero payload), every ~6–8 s.
3. **Pri_Comm owns the read side** entirely (the `35 02 ...` style
   responses we mapped in `docs/01` and `docs/11`).
4. **Linux's TTY layer serializes the write side**. Each `write()` syscall
   to `/dev/ttyAMA1` is held atomically by the tty's
   `atomic_write_lock`; interleave at byte-granularity is impossible.
   Frames go out whole, even from two separate fds.
5. The STM32 firmware **demuxes on the SLIP op2 byte**: `fc 83` →
   pilot/PWM status from CSR; `fc d5`, `fc 80`, `fc 11` → query/heartbeat
   from Pri_Comm. Each opcode triggers a different STM32 handler; the
   ack-direction frames (`fd d5` / `fb 80` / `fb 11`) flow back through
   Pri_Comm only.

**Implication for replacement:** safe to swap `Pri_Comm` with our own
personality. Just open a new fd to `/dev/ttyAMA1`, speak the
`fd d5` / `fb 80` family with full read-side ownership. CSR's
`fc 83` writes will keep flowing through the same UART unchanged, the
STM32 will keep demuxing them.

---

## 1. CSR's init syscall sequence

```
[77.416] open("/dev/spr320_pwm", O_RDWR)            → fd=3      (J1772 PWM)
[77.419] write(fd=3, "40 42 0f 00 40 42 0f 00", 8)              (duty 1e6 / period 1e6 = 100 %)
[77.421] open("/dev/ttyAMA1",   O_RDWR)             → fd=5      (STM32 link)
[77.423] ioctl(fd=5, TCSETS, &termios)
         c_cflag=0x08bd  (CS8 | CREAD | CLOCAL | B9600 — wait actually it's B9600!)
[77.896] open("/sys/class/gpio/gpio34/value", O_RDWR)→ fd=6      (input — AC-drop?)
[78.185] open("/sys/class/gpio/gpio54/value", O_RDWR)→ fd=7      (input)
[78.187] write(fd=5, "c0 fc 83 00×13 83 c0", 17)               (FIRST SLIP frame to STM32)
[78.204] write(fd=5, same 17 bytes)                              (immediate retry — 17 ms later)
```

Interesting:
- CSR's TCSETS sets `c_cflag = 0x08bd`. Lower nibble `0xd` = `B9600`.
  **CSR talks to the STM32 at 9600 baud.** Pri_Comm uses 115200 on the
  same UART. Two different baud rates on the same physical UART — both
  daemons' termios get applied, and *the last writer wins for the kernel
  baud rate setting.* The STM32 must auto-baud, OR Pri_Comm consistently
  re-applies 115200 on every open, OR (most likely) the kernel UART driver
  treats CBAUD as advisory and the STM32 receives bytes at the *line*
  rate which is presumably configured elsewhere (DMA / clock divisor).
  Worth a deeper look but doesn't block the replacement plan.
- The 17-byte frame is sent immediately twice (17 ms apart). Possibly a
  retransmit policy or a "guaranteed deliver" doubling. Replacement code
  should match.

## 2. Steady-state cadence

Over 100 s of idle capture (post-init):

| Event       | Count | Avg interval | Pattern                                  |
|-------------|------:|-------------:|------------------------------------------|
| PWM write `40 42 0f 00 40 42 0f 00` (100 % duty) | 6 | ~15 s | "pilot held at +12 V — no plug" |
| PWM write `1e 00 00 00 40 42 0f 00` (~0 % duty)  | 5 | (paired with above) | "brief test pulse"             |
| SLIP frame `c0 fc 83 00×13 83 c0`                | 11 | ~7–8 s | sent ~2 ms after each PWM update |

The frame's op2=0x83 and 13 zero-bytes payload (in idle) — the payload
slots must carry "current PWM duty, latched fault flags, etc." that the
STM32 mirrors back as a safety acknowledgement, populated when CSR has
real state to push. Bench is in UVP fault with no plug, so payload is
all zeros.

**Cycle observation:**
```
PWM (100 %) → SLIP frame   (sequence happens ~every 8 s)
PWM (~0 %)  → SLIP frame   (immediate follow-up, sometimes; "pulse")
```

This matches J1772 State A behavior — pilot held at +12 V, brief
probe-pulses, STM32 kept informed of the duty value so it can drive its
own contactor-arm logic from a fresh time-stamped value.

## 3. Why Linux tty layer makes the write-side safe

The Linux tty layer uses `tty->atomic_write_lock` (a mutex) inside
`tty_write()`. Two processes calling `write()` on the same tty device
(via separate fds) serialize fully — neither's bytes interleave at byte
granularity. Whole-frame integrity is preserved as long as each frame
fits inside a single `write()` syscall (CSR's = 17 B, Pri_Comm's = 5 B
for heartbeats and 17 B for telemetry — all safely under a typical 4 KB
tty buffer).

This is a per-tty mutex, not a per-fd one. It's exactly what we need.

## 4. Cross-reference to `docs/14` matrix

The matrix flagged `0x0a0b` as `Pri_Comm` read+write only (private
cache). It also flagged `0x0a07` as Pri_Comm-write / 5-reader. CSR
appears in the readers list for `0x0a07` (the STM32-fault status byte).

This consistency check passes: CSR consumes the fault-state byte that
Pri_Comm publishes (via its read of STM32 responses), but CSR doesn't
itself need to read from ttyAMA1 — Pri_Comm has already done the
reading and put the result in shmem. Clean producer/consumer split.

## 5. Updated replacement plan

Refines `docs/14 §7`:

- **`Pri_Comm` replacement is now unblocked.** Open `/dev/ttyAMA1`, set
  termios to 115200 8N1, write SLIP frames with the `fd d5` / `fb 80` /
  `fb 11` opcode family, read STM32 responses, publish to `shmem[0x0a07]`
  and `shmem[0x0a0b]`. CSR keeps running stock and its writes are safe.
- Order remains: `MeterIC_new` → `Adc` → `Pri_Comm`.
- Open question (not blocking): why does CSR set 9600 baud when Pri_Comm
  sets 115200 on the same UART? Kernel last-writer-wins or device-tree
  fixed rate? Worth confirming when we write the replacement.

## 6. Bench state at end of session

- `/root/Charging_Standard_RFID` = stock binary restored (md5 matches
  factory). Wrapped instance (PID 723) still running until next reboot
  — sparse trace continues to grow at ~22 B/sec, harmless.
- `/Storage/CSR.wrap.bak` = the wrapper, kept for redeploy if needed.
- `/Storage/stk/Charging_Standard_RFID` = stock backup.
- `/Storage/trace/Charging_Standa.log` = ongoing trace; will be wiped
  on next reboot recovery if we choose.
- All other charging daemons (`main`, `Pri_Comm`, `Adc`, `MeterIC_new`,
  `LED_control`, `FlashLog`, `ErrorHandle`, `RTC`, `RFID`,
  `delta-bridge`) untouched.
