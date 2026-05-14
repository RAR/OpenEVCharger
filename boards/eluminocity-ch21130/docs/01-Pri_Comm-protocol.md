# Pri_Comm Inter-MCU Protocol

Reverse-engineering of `/root/Pri_Comm` (and the older `Pri_Comm_cqc` build), the
Linux-side bridge that talks to the STM32F334C8T6 safety MCU over `/dev/ttyAMA1`
in a Delta Electronics EVMU30 / Eluminocity CH-21130 EV charger.

All findings combine **static analysis of an unstripped ARM ELF with debug
symbols** and a **live `/dev/ttyAMA1` capture (2026-05-14)** using a
musl-static ARM probe (`bench/ttyama1_probe_musl`) deployed over the RS-232
console. Where a fact rests on a string or a function name (rather than
instruction-level or wire evidence) it is marked **[name-evidence]** or
**[string-evidence]**. Wire-confirmed facts are marked **[wire]**.

## Live capture summary (2026-05-14)

Probe binary: `bench/ttyama1_probe_musl` (49596 bytes, musl static armv5te).
Unit state: mains energised, no vehicle connected — held constant across both
runs. Captures 05 and 06 are the *same* unit state; the only difference is that
Pri_Comm was restarted between them (visible as capture 06's first `FC D5`
returning queued meter data instead of the `0x10` version byte).
Full raw captures: `docs/05-ttyAMA1-live-capture.txt` and
`docs/06-ttyAMA1-live-capture.txt` — both captures are the same
mains-energised state, taken as two separate runs.

### Key confirmed facts

| fact | detail |
|---|---|
| STM32 is **poll-driven** | 3 s and 4 s passive-listen windows were completely silent — the MCU never transmits unsolicited frames |
| Response frame is **fixed 17 bytes** | C0 + op1 + op2 + 13-byte payload + csum + C0 for all three commands |
| Checksum formula **confirmed** | `(op2 + Σpayload) & 0xFF` — verified on all three responses (see below) |
| `0xFD` is "data response" op1 | Version (`FC D5`) and status (`FB 11`) both reply with op1=`0xFD`; static analysis had an off-by-one for `FB 11` (expected `FC` — actual is `FD`) |
| `0xFC` is "ack/alt" op1 | Alt-status (`FB 80`) replies with op1=`0xFC` |

### Wire frames

```
TX: c0 fc d5 d5 c0                                    (version query FC D5)
RX: c0 fd d5 10 00 00 00 00 00 00 00 00 00 00 00 e5 c0
    csum check: (0xd5 + 0x10) & 0xFF = 0xe5 ✓  payload[0]=0x10 = FW version
    NOTE: first FC D5 after Pri_Comm restart returns meter data payload
    (same format as FB 11) instead of the version payload; subsequent
    calls return 0x10... — the STM32 queues last meter snapshot and
    delivers it on the first query of a fresh session.

TX: c0 fb 11 11 c0                                    (status query FB 11)
RX: c0 fd 11 00 00 07 2a 00 00 0f fb 00 00 00 00 4c c0   (capture 05)
RX: c0 fd 11 00 00 07 20 00 00 0f fb 00 00 00 00 42 c0   (capture 06)
    csum check: (0x11+0x07+0x2a+0x0f+0xfb) = 0x14c & 0xFF = 0x4c ✓
    Both captures are the SAME unit state (mains energised, no vehicle). The
    between-frame differences below are measurement wander on a constant
    input, NOT a condition change.
    payload[0-1] = 0x0000  (Irms = 0 — no vehicle drawing current — likely BE u16)
    payload[2-3] = 0x072a / 0x0720 (1834 / 1824). Across every frame in both
      captures this word reads 1824–1834 (~10-count spread). Since the input
      was held constant, that wander means bytes 2-3 are a LIVE measured field
      (Vrms ADC word, or line frequency) — not a constant. Absolute scaling
      still unknown; needs a known/varied reference input to pin down.
    payload[6-7] = 0x0ffb = 4091  CONSTANT in every frame of both captures —
      not a measurement; likely a fixed status/capability word from STM32.
    payload[8-12] = all 0x00  (temps? EVSE state? TBD — need a connected vehicle)

TX: c0 fb 80 80 c0                                    (alt-status FB 80)
RX: c0 fc 80 10 00 00 00 00 00 00 00 00 00 00 00 90 c0
    csum check: (0x80 + 0x10) & 0xFF = 0x90 ✓  IDENTICAL in both captures.
    payload[0] = 0x10 — constant; heartbeat/ack response, not meter data.
```

Binaries analysed:

| file | size | source file | notes |
|---|---|---|---|
| `Pri_Comm` (35 KB) | `Pri_Comm.c` | newer build, 32 alarms, fw-upgrade path | analysed |
| `Pri_Comm_cqc` (23 KB) | `Pri_Comm_cqc.c` | older build, 24 alarms, no fw-upgrade | spot-checked |

## TL;DR

`Pri_Comm` is a single-threaded daemon (`main()` runs the whole loop) that owns
`/dev/ttyAMA1` at **9600 8-N-1, no parity, no flow control, raw mode**. Frames
are **SLIP-encoded** (RFC 1055; `END=0xC0`, `ESC=0xDB`, `ESC_END=0xDC`,
`ESC_ESC=0xDD`). A frame is `0xC0 | opcode1 | opcode2 | payload* | checksum | 0xC0`;
opcode1 tags direction/family (`0xFB`/`0xFC` host→MCU, `0xFB`/`0xFC`/`0xFD`
MCU→host) and opcode2 selects the actual command (e.g. `0x11` status query, `0x80`
status push, `0xD5` version, `0xB5`–`0xB8` firmware upgrade). Pri_Comm
synchronously drives request/response; on a 5-second timeout in either
`UartSend()` or `UartRecv()` it asserts the **"Pri MCU Lost"** alarm in
`shmem[0xa0b] |= 0x10`. The shared-memory segment (`shmget(0x153e, 0x40000,
0777)`) is the carrier between Pri_Comm and the rest of the application stack
(`main`, `Charging_Standard`, `MeterIC_new`, …): MCU response bytes are
demuxed into well-known offsets, and downstream daemons write commands back
into the same region for Pri_Comm to relay.

## UART configuration

```c
// main() @ 0x9974: open
fd = open("/dev/ttyAMA1", O_RDWR | O_NOCTTY);  // a318 → flags 0x102

// main() @ 0x9a3c: flush, then set raw 9600 8N1
tcflush(fd, TCIFLUSH /* 0 */);                  // 0x9a40
ioctl(fd, TCGETS /* 0x5401 */, &t);             // 0x99f4
t.c_iflag = 0;                                  // 0x9a04
t.c_oflag = 0;                                  // 0x9a00
t.c_cflag = 0x000008BD;                         // 0x99fc  ← constant @ a320
t.c_lflag = 0;                                  // 0x9a28
t.c_cc[VTIME] = 10;   /* 1.0 s */               // 0x9a20 (fp[-650])
t.c_cc[VMIN]  = 0;                              // 0x9a18 (fp[-649])
ioctl(fd, TCSETS /* 0x5402 */, &t);             // 0x9a5c
```

`c_cflag = 0x08BD` decomposes as:

| mask | meaning |
|---|---|
| `0x000D` (CBAUD) | B9600 |
| `0x0030` (CSIZE) | CS8 |
| `0x0080` | CREAD |
| `0x0800` | CLOCAL |

No `PARENB` (`0x100`), no `CSTOPB` (`0x40`), no `CRTSCTS` (`0x80000000`),
no `CBAUDEX` (`0x1000`). So **9600 baud, 8 data bits, no parity, 1 stop bit,
no hardware flow control**. VTIME=10/VMIN=0 → `read()` returns after up to
1.0 s even without data.

Evidence: constant pool word at `0xa320` is `0x000008BD` (`objdump -s`),
loaded via `ldr r3, [pc, #2336] @ a320` at `0x99f8`. Termios layout assumes
glibc `struct termios` ordering (iflag, oflag, cflag, lflag, line, cc[19]).

## Frame format

```
 +----+--------+--------+======================+======+----+
 | C0 | OP1    | OP2    | payload (0..N bytes) | csum | C0 |
 +----+--------+--------+======================+======+----+
       \________________________________________________/
        SLIP-escaped on the wire: every literal 0xC0 in
        OP1/OP2/payload/csum is replaced with `DB DC`, and
        every literal 0xDB with `DB DD`. Delimiters (C0) are NOT escaped.
```

### Encoding details (from `UartSend` @ 0x8788)

`UartSend(int fd, uint8_t op1, uint8_t op2, uint8_t *payload, size_t plen)`

Build sequence (line refs into `/tmp/Pri_Comm.disasm`):

1. **start delimiter** `0xC0` → `mvn r2, #63` at `0x87fc`, stored to buf.
2. **op1** byte (cmd family) at `0x8824`.
3. **op2** byte (sub-cmd) at `0x884c`.
4. **payload loop** `0x886c`–`0x8990`: for each byte `b`:
   - if `b == 0xC0` → write `0xDB 0xDC` (`mvn #36`, `mvn #35` at `0x8898`, `0x88c0`)
   - else if `b == 0xDB` → write `0xDB 0xDD` (`mvn #36`, `mvn #34` at `0x8908`, `0x892c`)
   - else write `b` unmodified (`0x8968`).
5. **checksum** seeded to 0 (`0x87c8`), then `chk = op2 + Σ payload[i]` (raw,
   pre-escape; `0x899c` adds op2, loop `0x89b0`–`0x89e4` adds payload bytes).
6. **checksum byte** SLIP-escaped using the same logic (`0x89f0`–`0x8aa4`).
7. **end delimiter** `0xC0` at `0x8ae8`.
8. `write(fd, frame, length)` in a 5-second poll loop (`0x8b10`–`0x8bb4`).
9. On timeout: `shmem[MeterSMPtr + 0xa0b] |= 0x10` → "Pri MCU Lost" bit
   (`0x8bd0`–`0x8be4`). On success: that bit is cleared (`0x8b60`–`0x8b8c`).

### Decoding details (from `UartRecv` @ 0x8c00)

`UartRecv(int fd, uint8_t *out_buf)` → returns positive frame length or 0
on timeout / framing error.

1. Wait up to 5 s for **start delimiter** `0xC0` (`0x8c70` `read(fd, &b, 1)`,
   compare `#0xC0` at `0x8c8c`).
2. Read one more byte = **op1**. Only `0xFB`, `0xFC`, `0xFD` are accepted as
   valid response families (`cmp #0xFB/#0xFC/#0xFD` at `0x8cc4/0x8cd0/0x8cdc`);
   anything else causes the whole frame to be discarded and a re-sync.
3. Reset deadline, then **byte stream loop** `0x8d60`–`0x8f94`:
   - read 1 byte;
   - if `0xDB`, read 1 more and substitute `0xDB 0xDC → 0xC0`
     (`0x8df0`–`0x8e0c`), `0xDB 0xDD → 0xDB` (`0x8e38`–`0x8e54`),
     anything else → abort frame;
   - if `0xC0` → terminator, finalize (`0x8e64`–`0x8e94`);
   - otherwise → store and advance.
4. Verify checksum: `chk_calc = Σ buf[2 .. len-2]` (the loop covers buf[2+i],
   i=0..len-3 — `0x8ea4`–`0x8ee8`), compared to `buf[len-1]`
   (`0x8eec`–`0x8f08`). On mismatch the function returns 0 (no copy).
5. On match, `memcpy(out_buf, &buf[1], len)` (`0x8f80`), then return `len`.
6. Same 5-second deadline as `UartSend`; on timeout sets the "Pri MCU Lost"
   bit (`0x8d3c`).

### Checksum — confirmed **[wire]**

Both sides use `(op2 + Σ payload) & 0xFF`. The static-analysis "asymmetry"
concern was a misread of `UartRecv`'s buffer indexing: `buf[2..len-2]` starts
at `buf[2]` which is `op2` (the raw receive buffer has `[0]=C0_delimiter`,
`[1]=op1`, `[2]=op2`, `[3..]=payload`), so the receiver loop actually includes
`op2`, matching `UartSend` exactly. All three wire responses verify cleanly.

**Use `csum = (op2 + Σ payload_bytes) & 0xFF` in any clean-room implementation.**

### SLIP delimiter constants in the binary

Loaded via `mvn` (one-cycle, no constant-pool hit) — easy to find on grep:

| ARM imm | one's complement | byte |
|---|---|---|
| `mvn r2, #63` (`#0x3F`) | `0xC0` | START/END delimiter |
| `mvn r2, #36` (`#0x24`) | `0xDB` | ESC |
| `mvn r2, #35` (`#0x23`) | `0xDC` | ESC_END (escaped 0xC0) |
| `mvn r2, #34` (`#0x22`) | `0xDD` | ESC_ESC (escaped 0xDB) |

Inverse path uses `cmp r3, #192`, `cmp r3, #219`, `cmp r3, #220`, `cmp r3, #221`.

## Command catalog

All TX call-sites in `main` (`grep "bl ... <UartSend>"` over the disassembly).
Columns:
- **op1** = frame family byte.
- **op2** = sub-command byte.
- **plen** = payload length passed as the 5th (stack) argument.
- **expected reply** = the op1/op2 pair that `main` compares the next
  `UartRecv()` against. `—` = no reply expected.

| op1 | op2 | plen | direction | expected reply (op1, op2) | what it does | evidence (offset in `Pri_Comm`) |
|---|---|---|---|---|---|---|
| `0xFC` | `0xD5` | 0 | host → MCU | `0xFD 0xD5` | **Read MCU version / OTP info.** Called as the very first frame each cycle; on a positive reply `main` continues into the upgrade-decision block. `Pri_Comm_cqc` does not issue this. | `0x9b20` (send), `0x9b60`/`0x9b6c` (reply check), `0xc3ec` (second site at end of loop) |
| `0xFC` | `0xB5` | 0 | host → MCU | reply consumed via blocking `UartRecv` loop @ `0x9d2c` | **Begin firmware download.** Sender pre-fills a 256-byte buf with `0xFF` (`0x9ccc`); MCU acks before next step. Gated on `shmem[0xa63] == 1` (firmware-update enable flag) — see "State machine" below. | `0x9cec` |
| `0xFC` | `0xB6` | 128 | host → MCU | none (fire-and-forget per block) | **Send firmware data block (128 bytes).** Loops over the buffer from `/mnt/PrimaryFW` in 128-byte chunks (`r3 << 7` indexes the chunk at `0x9d80`). Inner loop variable `i` increments per send (`0x9dfc`). | `0x9dc4` |
| `0xFC` | `0xB7` | 0 | host → MCU | reply consumed via `UartRecv` @ `0x9e84` | **Commit / verify block group.** Sent every 16 blocks (`i & 0xF == 0` at `0x9e08`). Waits for MCU ack before continuing. | `0x9e34` |
| `0xFC` | `0xB8` | 0 | host → MCU | none | **End firmware download.** Sent once after all chunks (`i << 7 >= total_size` at `0x9ed0`). | `0x9ef4` |
| `0xFB` | `0x11` | 0 | host → MCU | `0xFD 0x11` **[wire]** | **Periodic status query.** Main loop's "give me everything" poll. The MCU's reply (`0xFD 0x11`, 13-byte payload) is parsed into `MeterSMPtr+0…MeterSMPtr+0x1d4` (see "Shared-memory layout") and clears `Alarm` bit `0x10000` ("OVP — sample fresh"). Static analysis previously inferred reply as `0xFC 0x80` — live capture corrects this to `0xFD 0x11` (op2 echoes the request, op1=FD="data response"). | `0x9f94`, replies at `0x9fd4` (`0xFC`), `0x9fe0` (`0x80`) |
| `0xFB` | `0x80` | 0 | host → MCU | `0xFC 0x80`, alt path matches `0xFB 0x11` | **Status push request / heartbeat (alt form).** Second sender site in `main`. Same reply parser as `0xFB 0x11`. Possibly a "force one-shot push" vs the periodic poll, but a wire trace is needed to confirm whether the MCU treats them differently. Not present in `Pri_Comm_cqc`. | `0xa670` (send), `0xa6b8`/`0xa6c4` (reply check) |
| `0xFC` | `0x83` | 24 | host → MCU | none | **Push 24-byte control / event block.** Sender memsets the 24-byte buffer to 0 and sets `buf[1]=1` (`0x9b364`–`0x9b36c`), then sends. Issued when the MCU's reported "Pri-mode" byte is `0xa08 == 5` AND the local `Alarm` bit `0x80` is set (event-driven). Reads `MeterSMPtr+0x1d0..0x1d4` first → suspected "ack last event / clear MCU-side latch". | `0xb390` |

### Response opcode space observed in `main`'s parsers

| op1 | op2 | parsed where | meaning (inferred) |
|---|---|---|---|
| `0xFC` | `0x80` | `0x9fd0`+, `0xa6b4`+, `0xaab4`+ | status push from MCU (payload contains Vrms, Irms, AmbTemp, InletTemp, alarm bitmap, EVSE state, …) |
| `0xFB` | `0x11` | `0xaab8`/`0xaad0` | ack of `0xFB 0x11` reception (some MCUs echo the request before pushing status) |
| `0xFD` | `0xD5` | `0xc428`/`0xc438` | version / OTP info response |

So **`0xFB` and `0xFC` appear in both directions, while `0xFD` is MCU→host
only**. The op2 byte usually echoes the request.

## Shared-memory IPC layout

The Linux side creates a 256 KB SysV shared-memory segment that all daemons
attach to:

```c
// main() @ 0x98d8
shmid = shmget(0x0000153E /* MeterSMKey */, 0x40000 /* 256 KB */, 0x1FF /* 0777 */);
MeterSMPtr = shmat(shmid, 0, 0);
```

Offsets actually touched by `Pri_Comm` (`MeterSMPtr` = `*(uint8_t **)0x1620c`):

| offset | size | direction | meaning (inferred) | evidence |
|---|---|---|---|---|
| `+0x000` | u8 | MCU→ | Vrms low byte (status payload byte 0) | `0xb3b0` `ldrb r3, [r3]` then merged with byte 1 → `Vrms` |
| `+0x001` | u8 | MCU→ | Vrms high byte | `0xb39c` |
| `+0x004` | u8 | MCU→ | Irms low byte (status payload byte 4) | `0xb3e0`–`0xb3e4` |
| `+0x005` | u8 | MCU→ | Irms high byte | `0xb3cc` |
| `+0x154 + 3` | u8 (bitfield) | both | error bits (shm bookkeeping). bit `0x40` set when `shmat`/`open` fail (`0x9968`). bit `0x40` also set if `open("/dev/ttyAMA1")` fails (`0x99c0`). | `0x9944`–`0x9970`, `0x99a4`–`0x99c8` |
| `+0x1d0 + 3` | u8 | MCU→ | high byte of "pilot error voltage" (`0xbc48`); also overwritten on logging (`0xbd80`) | `0xb88c`, `0xbc48` |
| `+0x1d4` | u8 | MCU→ | low byte of "pilot error voltage" | `0xb8a8`, `0xbc60` |
| `+0xa00 + 7` | u8 | MCU→ | EVSE secondary state (compared `== 0` at `0xb848`, set `= 3` at `0xb2e0` on any-alarm) | `0xb840`, `0xb2d8`–`0xb2e4` |
| `+0xa00 + 8` | u8 | MCU→ | EVSE primary state. Values seen: 1 (idle), 4 (charging — compared at `0xb830`/`0xbc18`), 5 (event/fault — compared at `0xbc18`). | `0xb828`–`0xb834`, `0xb7b8`–`0xb7c4` |
| `+0xa00 + 11` (`0xa0b`) | u8 | Pri_Comm→ | UART link health. bit `0x10` set on `UartSend`/`UartRecv` 5 s timeout (`0x8be0`, `0x8d38`, `0x8fe8`); cleared on successful round-trip (`0x8b88`, `0x8f3c`). Drives the **"Pri MCU Lost" alarm** (msg index 29 in `AlarmMessage`). | `0x8b60`–`0x8b8c` |
| `+0xa10` | u8 | both | "set/target current" byte. OTPCheck does `duty_original = ((u8)x * 0x6E) + 0xC8` derating math (`0x9590`–`0x95a0`) — i.e. `OCP_val = 110·I_set + 200`. Read in `main` at `0xb6c8`-ish. | `0x9150`–`0x91a0`, `0x9684`–`0x9694` |
| `+0xa20 + 4` | u8 | host→ | "applied current" byte (post-derating). Same `110·x + 200 → OCP_val` math at `0x9ae4`–`0x9af0`. Likely written by upstream `main`/`Charging_Standard` and forwarded to MCU via `0xFC 0x83`. | `0x9ad4`–`0x9af0`, `0x9684`–`0x96ac` |
| `+0xa60 + 3` (`0xa63`) | u8 | both | **firmware-upgrade gate.** Compared to 1 at `0x9b98` and `0x9f6c` — when set, `main` enters the `0xFC 0xB5/0xB6/0xB7/0xB8` upload sequence. Cleared to 0 on any error (e.g. `open("/mnt/PrimaryFW")` failure, malloc failure, send timeout — `0x9bd0`, `0x9c10`, etc.). | `0x9b88`–`0x9b9c`, `0x9bcc`–`0x9bd0`, etc. |
| `+0xbf0 + 5` | u8 | OTPCheck→ | "derating active" flag (OTPCheck sets to 1 when ambient temp > threshold, 0 when cleared). | `0x90fc`–`0x910c`, `0x923c`–`0x9244` |

The 0xa00 block clearly looks like a packed MCU-status snapshot region
(little-endian fields, single-byte state codes, alarm bytes). The 0x1d0
region is the latest-frame buffer Pri_Comm uses for logging.

## Global state (BSS) inside `Pri_Comm`

(Names from the symbol table — verbatim debug symbols.)

| addr | sym | size | meaning |
|---|---|---|---|
| `0x16164` | (bss start) | — | — |
| `0x16168` | `Vrms` | 4 | RMS voltage (u16 packed into u32) |
| `0x1616c` | `Irms` | 4 | RMS current |
| `0x16170` | `AmbTemp` | 2 | ambient temperature (used in OTPCheck threshold compares: `0xF45=3909`, `0x176=374`, `0x42E=1070`, `0x52B=1323`, `0x248=584`, `0x175=373`) |
| `0x16172` | `InletTemp` | 2 | plug NTC temp |
| `0x16174` | `OTPFlag` | 2 | OTP latched flag |
| `0x16176` | `OTP_PLUG_Cnt` | 2 | plug-OTP event counter |
| `0x16178` | `duty_original` | 2 | CP duty-cycle backup before derating |
| `0x1617c` | `fds1` | 4 | open fd for `/dev/ttyAMA1` |
| `0x16180` | `OCP_val` | 4 | computed OCP target = `110·I_set + 200` |
| `0x16184` | `Alarm` | 4 | 32-bit alarm bitmap (one bit per `AlarmMessage[]` slot) |
| `0x16188`–`0x16264` | various `*Time` | 12 each | `struct timeb` snapshots for OVP/UVP/OCP/OTP/derating debouncing (`StartTime`, `EndTime`, `PollingStartTime`, `PollingEndTime`, `OVPTime`, `OVPTime_R`, `UVPTime`, `UVPTime_R`, `OTP_start_time`, `OTP_end_time`, `OTP_Chk_Start_time`, `OTP_PLUG_start_time`, `OTP_PLUG_end_time`, `OCP_start_time`, `Derating_start_time`, `Derating_end_time`, `chktime`) |
| `0x16270` | `CMD_Start_time` | 12 | last cmd timestamp |
| `0x1627c` | `Acktime` | 12 | last ack timestamp |
| `0x1620c` | `MeterSMPtr` | 4 | base of attached shmem segment |
| `0x161ac` | `TxDataBuf` | 24 | scratch buffer for the `0xFC 0x83` payload |
| `0x15164` | `AlarmMessage[32]` | 0x1000 | 128-byte/entry alarm-string table |

### Alarm bit map (partial — derived from `bic` instructions in the recovery sweep at `0xa384`–`0xa5ac`)

The recovery sweep clears `Alarm` bits in a fixed order; pairing that with
the `AlarmMessage[]` string order suggests a 1:1 mapping. Bits actually seen
set/cleared in `main` + `OTPCheck`:

| bit | line | likely alarm | sweep evidence |
|---|---|---|---|
| `0x00000001` | OR @ `0xb468` | OVP set | `bic 0x10000` recovery @ `0x9ff0` (the response-side clear path) |
| `0x00000002` | OR @ `0xb724`, BIC @ `0xb7a8`/`0xb7dc` | OVP-cleared latch | matches recovery pattern |
| `0x00000004` | OR @ OTPCheck `0x904c`/`0x92e8`, BIC @ `0x964c` | Ambient OTP | OTPCheck sets, recovery clears |
| `0x00000008` | BIC @ `0xa218`/`0xa5ac` | RA_RAM recovery |  |
| `0x00000010` | BIC @ `0xa1e8`/`0xa57c` | RA_FLASH recovery |  |
| `0x00000020` | BIC @ `0xa12c`/`0xa4c0` | RA_CPU recovery |  |
| `0x00000040` | OR @ OTPCheck end | Ambient NTC fail |  |
| `0x00000080` | BIC @ `0xa0d0`/`0xa464` | WELDING recovery |  |
| `0x00000400` | BIC @ `0xa0a0`/`0xa434` | RCD recovery |  |
| `0x00001000` | BIC @ `0xa070`/`0xa404` | EMGSTOP recovery |  |
| `0x00002000` | BIC @ `0xa040`/`0xa3d4` | Ambient OTP recovery |  |
| `0x00008000` | BIC @ `0xa014`/`0xa3a8` | OCP recovery |  |
| `0x00010000` | BIC @ `0x9ff0`/`0xa384` | OVP recovery (also the "status fresh" clear) |  |
| `0x00020000` | BIC @ `0xa1b8`/`0xa54c` | RA_DATA recovery |  |
| `0x00040000` | BIC @ `0xa15c`/`0xa4f0` | RA_WATCHDOG recovery |  |
| `0x00080000` | OR @ `0xb874`, BIC @ `0xbbfc` | **Pilot-error-voltage log trigger** (string `"Pilot error voltage"` is sprintf'd from this branch) |  |
| `0x00100000` | BIC @ `0xa0fc`/`0xa490`/`0xa6d4` | UVP recovery |  |
| `0x00200000` | OR @ OTPCheck `0x904c` | Hard immediate ambient OTP (AmbTemp > 0xF45=3909, ~temperature × 0.01 K → 390.9 K) | OTPCheck only |
| `0x00400000` | OR @ OTPCheck `0x9754`, BIC @ `0x9814` | Plug NTC (InletTemp out-of-range short-circuit?) | OTPCheck only |
| `0x00800000` | OR @ OTPCheck `0x9768` | Plug NTC (open-circuit?) | OTPCheck only |
| `0x01000000` | BIC @ `0xa18c`/`0xa520` | RA_CLOCK recovery |  |
| `0x08000000` | OR @ `0xbda4` | (per `bc18` EVSE-state-5 branch) — possibly an MCU-event-acked latch |  |
| `0x10000000` | OR @ `0xaa58`/`0xaee4` | **"Pri MCU Lost"** (msg index 29) |  |
| `0x20000000` | OR @ `0xaf88` | (charging-related fault flag) |  |
| `0x40000000` | OR @ `0xbfa0` | (charging-related fault flag) |  |
| `0x80000000` | OR @ `0xbf60` | (charging-related fault flag) |  |

The mapping between bit number and `AlarmMessage[idx]` index is **not** a
straight `1<<idx` — the recovery code carefully picks specific bits. A live
trace will be needed to lock the full table.

## State machine / lifecycle

`Pri_Comm`'s `main` is a single linear function (no fork/thread; symbol
table has no `pthread_*` references). Annotated outline:

1. **Attach shmem** (`shmget` + `shmat`). On failure flag a bookkeeping byte
   (`shmem[0x157] |= 0x40`) and exit cleanly (return 0 at `0x99cc`).
2. **Open UART** `/dev/ttyAMA1`. On failure flag the same byte and exit.
3. **Configure termios** to 9600 8N1 raw (above).
4. **`tcflush`** the input.
5. **`sleep(5)`** at `0x9a8c` — wait for the safety MCU to finish its own
   boot/POST before talking to it. Important: a clean-room replacement must
   honor this — the MCU does *not* tolerate early traffic. (Suggested by the
   `sleep(5)` placement, no string evidence.)
6. **Initialise time-base globals** (`ftime` into `StartTime`, `EndTime`,
   `PollingStartTime`, etc — `0x9a94`–`0x9ad0`).
7. **Compute initial `OCP_val`** from the (zero-init'd or previously-set)
   target current in `shmem[0xa24]` (`0x9ad4`–`0x9afc`).
8. **Enter main loop @ `0x9b00`** — runs forever; each iteration:
   1. Send `0xFC 0xD5` (version/OTP read). Receive `0xFD 0xD5 ...` if
      possible; otherwise silently fall through.
   2. **If `shmem[0xa63] == 1`** → run the firmware-upload pipeline:
      - `open("/mnt/PrimaryFW", O_RDWR|O_CREAT)` (`0x9ba8`).
      - `malloc(1 MiB)`, `memset(0xFF, 1 MiB)`, then `read(fd_fw, buf, 1 MiB)`
        in a loop until full (`0x9c38`–`0x9cc8`).
      - Send `0xFC 0xB5` (start). Wait for ack (`UartRecv` returns ≠ 0).
      - For each 128-byte chunk: send `0xFC 0xB6` with that chunk; every 16
        chunks send `0xFC 0xB7` (commit) and wait for ack.
      - When all chunks sent, send `0xFC 0xB8` (end).
      - Free buffer, set `shmem[0xa63] = 0` on completion or failure.
   3. **Send `0xFB 0x11`** (status query). Receive `0xFC 0x80 <payload>`.
      Parse payload into shmem (Vrms, Irms, temps, EVSE state, alarm bytes).
      Clear `Alarm` bit `0x10000`. If response is missing or malformed,
      `Alarm |= 0x10000000` (Pri MCU Lost).
   4. **Compare voltage/current/state thresholds** and set/clear alarm bits:
      - Vrms > `0x7147` (29031) for > 5 s → set `Alarm |= 0x1` (OVP).
      - Vrms < `0x6d60` (28000) for > 5 s → set UVP bit.
      - Vrms within window → clear OVP/UVP bits.
      - Several similar windows for ambient/plug temperatures.
   5. **Call `OTPCheck()`** when `DiffTimeb(now, chktime) > 0x1387 (4999 ms)`
      — i.e. every 5 s. OTPCheck recomputes `duty_original` and `OCP_val`
      from `shmem[0xa10]`, sets `Alarm`/derating flags from `AmbTemp` and
      `InletTemp`.
   6. **If EVSE primary state == 5** AND `Alarm[0] & 0x80` (i.e. some event
      latch) → build a 24-byte `TxDataBuf` (memset 0, `buf[1]=1`) and send
      `0xFC 0x83`.
   7. **If EVSE primary state == 1** AND various recovery checks pass →
      sweep `Alarm` bits clear with `bic` (`0xa384`–`0xa5ac`).
   8. **`ftime(now)`** and use `DiffTimeb` for the next interval gate.
   9. **At end of loop**: a second `0xFC 0xD5` (`0xc3ec`) and back to
      `0x9b78` (loop top). The `usleep` constants suggest pacing on the
      order of ~20–50 ms between iterations (constant at `0xa34c`).
9. There is **no clean shutdown path**: the loop runs until SIGKILL.

### Handshake

There is **no explicit handshake** beyond the `sleep(5)` cold start. The
first command of every loop iteration is `0xFC 0xD5` (or `0xFB 0x11` in the
older `Pri_Comm_cqc`); the MCU is expected to be live and responsive at
9600 baud the moment the UART is opened.

### Heartbeat / liveness

- Every loop iteration ends with at least one `0xFB 0x11` (status query).
  Loop pacing → effectively a heartbeat every few hundred ms.
- Each send and receive has a **5 s deadline** (`time(now) - time(start) > 5`
  at `0x8bb0` and `0x8fbc`). Any miss sets `Alarm |= 0x10000000` ("Pri MCU
  Lost"). A successful round-trip clears it.

### Error recovery

The daemon **does not** reset its UART, re-open the device, or rate-limit on
errors — it just keeps trying. Long-running miscommunication keeps
`Alarm[0x10000000]` asserted; downstream consumers (`main`, web/snmp) act on
that bit.

## `Pri_Comm` vs `Pri_Comm_cqc`

Both binaries contain byte-identical copies of `UartSend` (0x478 bytes) and
`UartRecv` (0x40c bytes) — only their entry-point addresses differ.
`OTPCheck` is also present in both but **shorter in cqc** (0x614 vs 0x87c
bytes), correlating with the missing derating math.

### Symbol-table delta

`Pri_Comm` adds these globals (not in cqc):

```
Acktime           CMD_Start_time      Derating_start_time  Derating_end_time
OCP_start_time    OCP_val             OVPTime              OVPTime_R
UVPTime           UVPTime_R
```

…and these libgcc soft-float helpers (cqc has no double-precision math):

```
__adddf3 __subdf3 __muldf3 __divdf3 __aeabi_d2uiz __aeabi_dadd
__aeabi_ddiv __aeabi_dmul __aeabi_drsub __aeabi_dsub __aeabi_f2d
__aeabi_i2d __aeabi_l2d __aeabi_ui2d __aeabi_ul2d __floatdidf
__floatsidf __floatundidf __extendsfdf2 __fixunsdfsi
```

### Strings delta (alarms only)

Pri_Comm_cqc lacks the following alarm string pairs (indices 24–31 in the
newer build's `AlarmMessage[]`):

- `RCDLOCK alarm` / `... recovered`
- `AC drop` / `AC drop recovered`
- `Firmware upgrade fail` / `... recovered`
- `PILOTERROR_Negative alarm` / `... recovered`
- `Relay driver fault` / `... recovered`
- `Pri MCU Lost` / `Pri MCU Lost recovered`
- `Wifi module fail` / `Wifi module recover`
- `RFID module fail` / `RFID module recover`

So `AlarmMessage[]` in cqc is `0xC00 = 24 × 0x80` bytes vs `0x1000 = 32 × 0x80`
in the newer build.

### Command-set delta

| op1 op2 | Pri_Comm | Pri_Comm_cqc |
|---|---|---|
| `0xFC 0xD5` (version) | ✔ | — |
| `0xFC 0xB5`–`0xB8` (firmware) | ✔ | ✔ |
| `0xFC 0x83` (event push) | ✔ | ✔ |
| `0xFB 0x11` (status query) | ✔ | ✔ |
| `0xFB 0x80` (status query alt) | ✔ | — |

### Build identity

Both binaries are built with `GCC 4.6.2 20110813 (STMicroelectronics/Linux
Base 4.6.2-97)` for `arm-926ejs-linux-gnueabi`, glibc 2.10.2. The cqc
variant matches the source-file name `Pri_Comm_cqc.c` (in DWARF). "CQC" most
likely = **China Quality Certification** — a region-specific stripped-down
build with fewer alarms and no derating logic.

## Unknown / unresolved

Items resolved by the 2026-05-14 live capture are struck through.

1. ~~**Checksum convention on the wire.**~~ **RESOLVED [wire]:** both sides use
   `(op2 + Σ payload) & 0xFF`. See "Checksum — confirmed" above.
2. **`0xFB 0x11` vs `0xFB 0x80`.** Live capture shows `FB 11` → `FD 11` reply
   (op2 echoed, 13-byte payload with actual meter data); `FB 80` → `FC 80`
   reply (op1 one lower, same 13-byte payload as the version response with
   byte 0 = 0x10). The distinction is now clearer — `FB 11` is a data query,
   `FB 80` may be a heartbeat/ack — but confirming requires capturing a session
   where the MCU is seeing live mains voltage or a connected vehicle.
3. **`0xFD` response space.** All three probe commands returned `0xFD` op1 for
   data responses. The MCU was quiet between polls (no unsolicited `0xFD`
   frames in 3+4 s listens). A longer capture under load may surface
   asynchronous fault events.
4. **Status-payload layout** — partially updated from live data:
   - Payload is 13 bytes (not 16 as previously guessed); frame is 17 bytes total.
   - `FD 11` response, mains energised / no vehicle: `00 00 07 2a 00 00 0f fb 00 00 00 00 00`
   - Bytes 0-1 = `0x0000` (Irms = 0A — no vehicle drawing current — likely big-endian u16, units TBD).
   - Bytes 2-3 = `0x0720`–`0x072a` (1824–1834) — a LIVE measured field: it wanders ~10
     counts across frames even though the input was held constant. Vrms ADC word or line
     frequency; absolute scaling (via Vgain=342?) still TBD.
   - Bytes 6-7 = `0x0ffb` = 4091 (constant in every frame — fixed status/capability word, not a measurement).
   - Bytes 8-12 = all zero (temps? alarm nibbles? state?) — need a connected vehicle/load to move them.
   - Decode requires stimulating the unit with known inputs (vary line voltage, draw current, connect a vehicle).
5. **Full `Alarm` bit-to-message mapping.** Static analysis covers ~20 of
   the 32 bits; the remaining ones (RA_TIMING, RA_IO, RA_INTERRUPT,
   RA_ADC, RCDTRIP, GMI, PILOTERROR, PILOTERROR_Negative, INITIAL,
   RCDLOCK, AC drop, Firmware upgrade fail, Relay driver fault, Wifi fail,
   RFID fail) need cross-reference against the inbound MCU status payload
   under fault injection.
6. ~~**MCU-side checksum scope.**~~ **RESOLVED** — same as #1.
7. **`shmem` regions touched by *other* daemons.** Pri_Comm reads
   `shmem[0xa10]`, `shmem[0xa20+4]`, `shmem[0xa63]`, `shmem[0xa00+7]`,
   `shmem[0xa00+8]`. The supervisor `main` and the `Charging_Standard` /
   `MeterIC_new` binaries are the producers; their offsets need
   cross-mapping but that's outside this document.

## Recommended next steps

In rough order, lowest effort first:

1. ~~**Tap `/dev/ttyAMA1`** to confirm SLIP framing and checksum.~~ **DONE
   (2026-05-14)** — probe confirms SLIP, 17-byte fixed responses, checksum
   `(op2 + Σ payload) & 0xFF`. Raw capture in `docs/05-ttyAMA1-live-capture.txt`.
2. **Decode the `0xFD 0x11` payload byte-by-byte** by stimulating known inputs
   while the probe intercepts live traffic (or by reading shmem offsets under
   known conditions):
   - vary line voltage by a known amount → watch how bytes 2-3 track it (and
     settle whether it reads Vrms or line frequency).
   - draw measured current → bytes 0/1 (currently 0x0000) should rise.
   - heat plug NTC → find which byte maps to `InletTemp`.
   - flip EMGSTOP or connect a vehicle → watch the zero bytes for state transitions.
3. **Cross-check this analysis against `Pri_Comm_cqc`** for any
   contradictions in shmem offsets.
4. **Capture a `/Storage/EncodeLogMessage` reference run** (boot, plug, draw
   current, unplug, fault-injection) so the alarm-bit-to-message mapping can
   be pinned down deterministically.
5. **Cross-reference `main`** (the supervisor, 87 KB binary) for the *writer*
   side of the shmem command queue, especially `shmem[0xa10]`/`shmem[0xa20+4]`
   (current-set commands) and `shmem[0xa63]` (firmware-upgrade trigger).
6. **Once steps 2–5 land**, write the clean-room implementation. The
   protocol is small enough to fit in a few hundred lines of C; the
   challenge is fault parity (32 alarms, debounce windows, derating
   thresholds), not the wire format.

## Pin / topology cross-references (out-of-scope confirmations)

The CPU side of `/dev/ttyAMA1` is the SPEAr320S's UART1 (PL011). The MCU
side is STM32F334C8T6 USART2 or USART3 (TBD by schematic). Galvanic
isolation by 2× TI ISO7241C — these are bidirectional digital isolators
rated for 25 Mbps so they're transparent at 9600 baud. The 9600-baud choice
is conservative for what the silicon can do; likely a hold-over from the
factory bring-up bench. A clean-room build can stay at 9600 to keep frame
timing compatible with the stock STM32 firmware until a firmware swap is
also planned.
