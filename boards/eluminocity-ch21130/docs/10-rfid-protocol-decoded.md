# RFID reader protocol — fully decoded from live stock-firmware capture

**Date:** 2026-05-16
**Method:** LD_PRELOAD shim (`companion/tools/uart_trace.c`) wrapping
`/root/RFID` over a clean cold boot, capturing every `open` / `read` /
`write` / `close` / `ioctl` syscall the daemon makes for ~270 seconds
across multiple card-on/card-off cycles.
**Card under test:** non-Delta MIFARE UltraLight, UID `AE BF 7E 3E`.
**Raw trace:** `companion/test/data/uart-trace-stock-2026-05-16.log` (the
filtered view; ~3500 events of poll noise dropped, full ~275 KB log lives
on the bench unit at `/tmp/uart-trace.log` if needed).

This doc supersedes the earlier guesses in `docs/08-rfid-auth-flow.md`
and the bench-blocked status in `docs/09-rfid-bench-findings.md`. **Path
A (snoop shmem) is dead** (`docs/09 §11`). **Path B (replace stock
`/root/RFID`) is now fully specified** by what's below.

---

## 1. The full daemon lifecycle, observed

```
t=79.043  open("/dev/ttyAMA4", O_RDWR)              → fd=3
t=79.114  ioctl(fd=3, TCSETS, *termios)             → ok
t=79.2..80.85   (12× paired CLOSE fd=5/10)          ← glibc system() pipe fds
                                                      from /etc/funs-style
                                                      `echo … > /sys/class/gpio/…`
                                                      shell calls, see §2
t=80.861  open("/dev/spr320_pwm1", O_RDWR)          → fd=5
t=80.862  write(fd=5, "\x9a\x5b\x06\x00\x9a\x5b\x06\x00", 8) ← PWM init, see §3
t=80.86…  poll loop begins (see §4)
```

That is the entire init. There is **no UART handshake**, **no magic
register-write sequence**, **no auth handshake** beyond `TCSETS` on the
serial port and the 8-byte direct write to `/dev/spr320_pwm1`.

`/root/RFID` then enters the poll loop and stays in it. The PWM fd is
**never closed** — this is what holds the PWM peripheral in its active
state and is why the SPEAr3xx PWM driver close+reopen NULL-deref bug
(documented in `docs/09 §1`) fires whenever the daemon is restarted.
Our replacement must do the same: open `/dev/spr320_pwm1` once at
startup and hold the fd until process exit.

---

## 2. GPIO init via `system()`

We don't see the individual `open(/sys/class/gpio/…)` syscalls because
they happen inside the `/bin/ash` children that `system()` forks — the
shim is loaded in the RFID parent process only. The trace shows the
**indirect signature**: glibc's `system()` opens a pipe (we don't hook
`pipe()`, so no OPEN event), forks, waits, then closes both pipe ends —
which **does** go through our `close` hook.

Twelve paired `CLOSE fd=5 / CLOSE fd=10` events between t=79.2 and
t=80.85 = 12 `system()` invocations, which matches the static RE in
`docs/09 §1` exactly:

> stock `main()` runs 11 consecutive `system("echo … > /sys/class/gpio/…")`
> calls + `PWM_Init()` once at startup. Specifically:
> ```
> gpio 48 → out, 1
> gpio 57 → out, 1
> gpio 56 → out, 0
> gpio 55 → out, 0
> /dev/spr320_pwm1 → write 8 bytes (0x9a 0x5b 0x06 0x00 0x9a 0x5b 0x06 0x00)
>                    i.e. duty=period=0x00065B9A
> ```

(The 11 vs 12 discrepancy resolves to: `gpio_export + direction + value`
= 3 calls per GPIO × 4 GPIOs = 12, with the PWM byte-write being a
separate direct `open` + `write` rather than going through `system()`.)

The static RE was right. **Don't reinvent the GPIO/PWM init; just call
`system()` with these exact strings in the same order.**

---

## 3. PWM peripheral programming

```
open("/dev/spr320_pwm1", O_RDWR) → fd=5
write(fd=5, 9a 5b 06 00 9a 5b 06 00, 8 bytes)
```

The 8 bytes are two little-endian u32 values, identical:

```
period_ticks = 0x00065B9A = 416 666
duty_ticks   = 0x00065B9A = 416 666  (100% duty)
```

With the SPEAr3xx PWM clock (the driver derives this from the 48 MHz
APB bus, divider unknown without driver-source access), this comes out
to roughly a `1/<period>` Hz square wave — likely the reader chip's
**radio carrier reference** or **RF gate clock**. **The reader needs
this signal continuously**; closing fd=5 stops the clock, which
(a) makes the reader unable to detect cards, and (b) puts the driver
into its broken state.

**Reproduce this exactly. Do not touch the values.** Open with O_RDWR,
write the 8 bytes once, leave the fd open.

---

## 4. UART poll-loop protocol

### Frame format

All frames in both directions:

```
+-----+-----+----------+-----+
| LEN | CMD | PAYLOAD… | XOR |
+-----+-----+----------+-----+
```

- `LEN` (1 byte) = number of bytes in `[LEN..XOR-1]`, i.e. one less
  than the total frame size. A 4-byte frame has `LEN=3`.
- `CMD` (1 byte) = command/response opcode.
- `PAYLOAD` (0 or more bytes) = command/response data.
- `XOR` (1 byte) = XOR of all preceding bytes (`LEN ^ CMD ^
  payload[0] ^ …`).

All `[LEN][CMD][PAYLOAD…][XOR]` lengths and checksums verified against
the live capture.

### Command table

| TX  | direction | meaning | notes |
|-----|-----------|---------|-------|
| `03 20 00 23` | M→R | **Request_CardSN** (poll for card) | arg byte = `0x00` (always in this firmware) |
| `03 41 08 4a` | M→R | **UL_read** block 8 | arg byte = block number (UltraLight pages) |

| RX  | direction | meaning | notes |
|-----|-----------|---------|-------|
| `02 df dd` | R→M | "no card" status, idle | the steady-state response |
| `02 be bc` | R→M | "no card" status, post-session | observed once, immediately after a card was removed; possibly a one-shot "session ended" marker |
| `0c 20 04 UU UU UU UU MM MM MM MM 00 00 XX` | R→M | **card present**: 4-byte UID + 4-byte metadata | LEN=12. CMD=20 echoes the request. Byte 3 = UID length (`0x04` for ISO14443A 4-byte UID). Bytes 4-7 = UID. Bytes 8-11 = card metadata (likely SAK + ATQA, exact decode TBD). Bytes 12-13 = `00 00` (reserved/padding). |
| `12 41 PP PP PP PP PP PP PP PP PP PP PP PP PP PP PP PP XX` | R→M | **UL_read response** | LEN=18. CMD=41 echoes. 16 bytes of UltraLight memory (4 consecutive pages × 4 bytes). On our blank card all zeros. |

### Observed timing

- **Poll cadence**: `WRITE 03 20 00 23` happens every **~110 ms**
  (≈ 9 Hz). The interval is reader-bounded — the daemon's turn-around
  time from RX-complete to next TX is ~200 µs. So our replacement can
  run the same loop without throttling; the reader chip's response
  delay (~110 ms) sets the rate.

- **Card-present flow** (steady state with the card on the reader):
  - `WRITE 03 20 00 23` (poll)
  - `READ 0c 20 04 UU UU UU UU MM MM MM MM 00 00 XX` (card present)
  - `WRITE 03 41 08 4a` (UL_read block 8 — daemon's discovery follow-up)
  - `READ 12 41 PP×16 XX` (UL data)
  - `WRITE 03 20 00 23` (poll again — total cycle ~220 ms)

  Both reads happen inside the same poll cycle. The daemon does NOT
  read more blocks; only block 8. Possibly that's where Delta-issued
  cards stamp something the daemon validates against. We don't need to
  do this at all for our use case.

### Decoding the captured frames

```text
[118.539722] READ fd=3 ret=13 n=20 << 0c 20 04 ae bf 7e 3e 61 81 44 00 00 dd
            ─┬─ ─┬─ ─┬─ ─┬───────── ─┬───────── ─┬─── ─┬─
             │   │   │   │           │           │     └─ XOR checksum
             │   │   │   │           │           └─── padding (`00 00`)
             │   │   │   │           └─── 4 metadata bytes (SAK/ATQA candidates)
             │   │   │   └─── UID = AE BF 7E 3E
             │   │   └─── UID length = 4 (ISO14443A short UID)
             │   └─── CMD = 0x20 (echoes Request_CardSN)
             └─── LEN = 12

XOR check:  0x0c ^ 0x20 ^ 0x04 ^ 0xae ^ 0xbf ^ 0x7e ^ 0x3e
          ^ 0x61 ^ 0x81 ^ 0x44 ^ 0x00 ^ 0x00 = 0xdd  ✓
```

---

## 5. What stock does with the UID (and why our shmem snapshot was empty)

**Nothing observable.** The trace has zero file writes (`/Storage/…`,
`/tmp/…`, `/dev/…` other than the two known fds) across the entire ~270 s
run, including multiple full card-on→UL-read cycles. Also zero new
opens after the initial PWM open at t=80.86.

That leaves three possibilities for where the UID goes when stock
*does* recognise it as Delta:

1. **DETA-prefix filter at the source.** `/root/RFID` discards any UID
   whose first bytes aren't `DE TA…` before any IPC. The "no-write
   trace + UL_read still happens" pattern is consistent: parse →
   UL_read → check prefix → if no match, drop.
2. **Socket-based IPC** to a sibling daemon. Our shim doesn't hook
   `socket`/`connect`/`sendmsg`/`sendto`. Possible but unlikely given
   no socket-related strings in the static RE.
3. **Shmem write at a different offset.** Already disproved in
   `docs/09 §11` — full 256 KiB snapshots showed no UID bytes anywhere.

The morning shmem diff (`docs/09 §11`) settled (3). The daemon does
extract the UID and exercise UL_read on it (proved above). So the
DETA filter must be inside `/root/RFID` itself, **applied between the
UL_read result and any downstream write**. For our purposes this is
the answer we needed: **no shmem channel exists; we have to publish
the UID ourselves from a replacement daemon.**

---

## 6. Replacement daemon — implementation specification

Replace `delta-bridge/src/rfid.c` (the v0.5 we shipped to PR #14) with
a daemon based on this concrete protocol. Drop the antenna-on
speculation and the snoop-mode hedge. Open questions are all resolved
by the trace above.

### Init order (must match stock exactly)

1. `open("/dev/ttyAMA4", O_RDWR)` — store fd as `uart_fd`
2. `tcsetattr(uart_fd, …)` — 115200 8N1, raw mode, `VMIN=1 VTIME=2`
   (200 ms inter-byte timeout; stock's `TCSETS` value matches the
   defaults for these knobs based on the observed 110 ms RX completion
   pattern). Exact value can be reverse-decoded from a second trace
   that logs the ioctl arg bytes if needed — for v0.6 the defaults are
   fine.
3. For each of the 4 GPIOs in this order (`gpio 48, 57, 56, 55`):
   `system("echo NN > /sys/class/gpio/export")`,
   `system("echo out > /sys/class/gpio/gpioNN/direction")`,
   `system("echo V > /sys/class/gpio/gpioNN/value")` where V = `1` for
   gpio 48 and 57, `0` for gpio 56 and 55.
4. `open("/dev/spr320_pwm1", O_RDWR)` — store fd as `pwm_fd`.
5. `write(pwm_fd, "\x9a\x5b\x06\x00\x9a\x5b\x06\x00", 8)`.
6. **Never close pwm_fd.**

### Poll loop

Every cycle (`~110 ms` is the natural cadence; let the reader pace
us):

1. `write(uart_fd, "\x03\x20\x00\x23", 4)`
2. Read until a complete frame arrives. Per the trace, the reader
   sends responses as a single TTY chunk; we can `read(uart_fd, buf, 64)`
   and parse what we got. If the response is short, retry after a brief
   yield.
3. Parse the frame: validate LEN, CMD, XOR.
4. If frame is `02 df dd` or `02 be bc` → no card, idle.
5. If frame is `0c 20 04 UU UU UU UU …` → extract UID, debounce
   against last-published UID, publish on rising edge.

### Card-present optional follow-up

Stock does a `UL_read` block 8 immediately after a card-present
response. For our use case (publish UID to MQTT/HA), this is
**unnecessary**. Don't issue UL_read at all. Just publish the UID and
keep polling. The reader continues returning the card-present frame as
long as the card is on the antenna.

### Publishing

Same as v0.5: MQTT discovery topic for `sensor.last_uid`, publish
hex-encoded UID on debounced rising edge, retain. Drop the
`rfid_kill_stock` / `rfid_mode` config keys entirely — we no longer
care to coexist with stock; Path B is **replacing** stock outright at
a later phase, and standalone replacement at this layer is correct.

### Deployment

For v0.6 development: same wrapper-script trick used today (`/root/RFID
→ /Storage/our_rfid_daemon`). Eventually phase 1 of the §6 "use our
own binaries" plan in `docs/09` will fold this into a single binary
that replaces stock outright.

---

## 7. Tooling kept

- `companion/tools/uart_trace.c` + the `.so` build recipe in its
  file header — generic enough to point at any other stock binary on
  this unit. Useful for the next phase (Pri_Comm, Charging_Standard_RFID
  protocols, etc.). **Keep this around.**
- `companion/test/data/uart-trace-stock-2026-05-16.log` — primary
  evidence file for this doc. Keep.

---

## 8. What we still don't know (small)

- **Exact termios bytes in the TCSETS ioctl arg** — the shim only logs
  fd+req, not the user-pointer arg. A 5-line shim enhancement would
  fetch the 36-byte termios struct from the arg pointer and log it.
  Worth doing the next time we rebuild the shim, but not blocking.
- **What the 4 metadata bytes `61 81 44 00` mean.** Likely a packed
  `[SAK][ATQA-hi][ATQA-lo][?]` or similar; doesn't matter for our
  use case. If needed later, compare against ISO14443A standard.
- **Whether `02 be bc` is "card removed" or "session ended" or
  something else.** Observed exactly once at the very end of the
  capture, right after a card session. One observation isn't enough.
  Doesn't affect our implementation — we treat both `02 df …` and
  `02 be …` as "no card" by checking `LEN=2 ∧ XOR-valid`.
