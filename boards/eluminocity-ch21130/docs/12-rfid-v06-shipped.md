# RFID v0.6 — bench-validated, shipping

**Date:** 2026-05-16 end of day
**Status:** ✅ **WORKING.** v0.6 replacement daemon detects the bench's
non-Delta MIFARE UltraLight card and publishes the UID to MQTT in
real time:

```
[13:21:38] delta-bridge/evmu30-bench/rfid/last_uid: 04AEBF7E3E6181
[13:21:38] delta-bridge/evmu30-bench/rfid/scan_event: {"event_type":"card_scanned","uid":"04AEBF7E3E6181"}
```

UID `04AEBF7E3E6181` matches stock's wire-captured frame
`0c 20 04 ae bf 7e 3e 61 81 44 00 00 dd` (this morning's LD_PRELOAD
trace) byte-for-byte. The replacement daemon is functionally indistinguishable
from stock at the UART layer, except that we publish all UIDs without
the DETA-prefix filter (by design — HA owns any allowlist policy).

---

## 1. The bug we hit

After the protocol decode (docs/10) and a clean code structure that
host-tested at 620/620, our daemon's `read(/dev/ttyAMA4)` always
returned 0 bytes on the bench, even with a card on the reader. Stock
worked on the same hardware (verified via `shmem[0x0A70]` activity
latch). Every diagnostic eliminated obvious suspects — GPIOs were
correct, PWM was opened+written, polls went out (`write returned 4`),
binary linkage was fine, deployment was clean, the bridge process
was alive. Reads just returned nothing.

## 2. The root cause

**`struct termios` layout mismatch between musl (NCCS=32, sizeof=60)
and the SPEAr3xx kernel's UART driver (expects glibc-2.10-style
NCCS=19, sizeof=44).**

Our daemon called `tcsetattr(fd, TCSANOW, &t)` which musl's libc
forwards to `ioctl(fd, TCSETS, &t)` with `&t` pointing at musl's
60-byte struct. The kernel driver dereferences only the first 44
bytes, but interprets them per its own (smaller) layout — so musl's
`c_cc[6]` (VMIN) lands at the kernel's `c_cc[6]` correctly, but musl's
`c_ispeed` field at byte offset 52 lands beyond the kernel's struct.
The kernel ends up reading whatever musl put at offset 36–44 as
`c_ispeed` and `c_ospeed`. Those bytes happen to be unused padding
in musl's layout, often zero.

Result: kernel sets the UART termios with reasonable c_iflag /
c_oflag / c_cflag (those land at the same offsets in both layouts —
just 4 u32s at the start), but with garbage c_ispeed/c_ospeed and
sometimes c_cc tail bytes. RX path was effectively dead.

## 3. The fix

In `src/rfid.c`, replace the `tcsetattr()` call with a raw
`ioctl(fd, TCSETS, STOCK_TERMIOS)` where `STOCK_TERMIOS` is a 60-byte
hardcoded buffer containing the **exact bytes** stock `/root/RFID`
sends on its TCSETS:

```c
static const unsigned char STOCK_TERMIOS[60] = {
    /* c_iflag */ 0x00, 0x00, 0x00, 0x00,
    /* c_oflag */ 0x00, 0x00, 0x00, 0x00,
    /* c_cflag */ 0xbe, 0x08, 0x00, 0x00,    /* CS8|CREAD|CLOCAL */
    /* c_lflag */ 0x00, 0x00, 0x00, 0x00,
    /* c_line  */ 0x00,
    /* c_cc[19] */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x80, 0x27,
    /* c_ispeed */ 0x40, 0x00, 0x00, 0x00,
    /* c_ospeed */ 0x00, 0x00, 0x00, 0x00,
    /* trailing zero pad */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
```

The kernel UART driver overrides the CBAUD bits to 115200 (platform
binding); the c_ispeed=0x40 is meaningless but kept for verbatim
parity with stock.

## 4. How we discovered it

Extended `companion/tools/uart_trace.c` (the LD_PRELOAD shim from
docs/10 §7) to deref the ioctl arg pointer and log the first 60 bytes
of `struct termios` for TCGETS/TCSETS. Wrapped stock `/root/RFID` and
captured one TCSETS event during boot:

```
[78.774778] IOCTL fd=3 ret=0 req=0x00005402 arg=0x00012338 termios=
  00 00 00 00 00 00 00 00 be 08 00 00 00 00 00 00
  00 00 00 00 00 00 0a 00 00 00 00 00 00 00 00 00
  00 00 00 00 03 00 00 00 00 80 27 40 00 00 00 00
  00 00 00 00 00 00 00 00 00 00 00 00
```

Plugged those exact bytes into our daemon, rebooted, held the card,
the UID published immediately.

## 5. Lessons

- **Never trust a libc wrapper across a glibc/musl boundary for
  kernel-ABI structs.** Anywhere we cross-compile a musl-static binary
  to run against a glibc-2.10 kernel target, structs that flow through
  ioctls must be hand-packed or the wrapper bypassed.
- **The LD_PRELOAD shim approach pays off compound interest.** First
  used to decode the RFID protocol (docs/10), then the Pri_Comm
  protocol (docs/11), now to capture stock's exact termios bytes. The
  reusable-technique memory entry `reference_ld_preload_syscall_shim`
  has all three uses to reference.
- **Don't accept "host-tested looks the same" when bench doesn't
  work.** The host tests pass against musl-static-on-host (same libc),
  so they couldn't have caught this. The bench test was always the
  truth.

## 6. What's committed in this PR

This commit (PR #14, branch `eluminocity-m4-rfid`):

- `src/rfid.c` — `STOCK_TERMIOS[60]` + raw `ioctl(TCSETS)`
- `src/rfid.h` — unchanged (interface from earlier v0.6 commit)
- `src/config.{c,h}` — `rfid_kill_stock`/`rfid_poll_hz` deprecation
- `src/main.c` — `-c <path>` arg parser (was always broken; M0 bug)
- `test/test_rfid.c` — 74 tests including wire-verbatim capture
- `tools/uart_trace.c` — ioctl arg-deref enhancement
- `tools/rfid_probe.c` — standalone test binary (kept for future bench work)
- `docs/10-rfid-protocol-decoded.md` — frame format + init contract
- `docs/11-pri_comm-syscall-trace.md` — companion Pri_Comm trace
- `docs/12-rfid-v06-shipped.md` (this file) — the gotcha

PR #14 is ready to merge. Bench-validated against a real card.

## 7. Bench state at end of session

- `/root/RFID` → wrapper script that exec's
  `/Storage/delta-bridge/delta-bridge -c /Storage/delta-bridge/delta-bridge.conf`
- `/root/RFID.stock` — preserved as backup; rollback recipe is
  `cp /root/RFID.stock /tmp/x && mv -f /tmp/x /root/RFID && sync; reboot`
- `/Storage/RFID.stock` — second backup copy on persistent storage
- `/Storage/delta-bridge/delta-bridge` is v0.6 (md5
  `0e6c7f0bb588fdfd1726b730b30d8bdb`)
- Delta charger is fully functional: MQTT, web UI, RFID, all running
