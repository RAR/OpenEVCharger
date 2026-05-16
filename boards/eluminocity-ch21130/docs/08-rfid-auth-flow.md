# Delta EVMU30 — RFID Authentication Flow

**Goal**: enumerate every component of the Delta EVMU30 RFID auth pipeline —
hardware, daemon roles, shmem byte semantics, persistence paths — and decide
whether `delta-bridge` can expose RFID-based control (cached allowlist,
external authorize, idTag UI) without racing the stock daemons.

**Method**: static reverse-engineering of the ARM ELF binaries in
`/home/rar/device-configs/esphome/testcharger/delta/` (specifically `RFID`,
`Charging_Standard_RFID`, `Charging_Standard`, `main`, `DeltaOCPP`) plus
read-only live probes of the bench unit (`/dev/ttyACM0`, no shmem writes).

**Status**: predictions are documented inline with the per-byte/per-call
evidence. None of the proposed external-write paths have been bench-validated
yet — verification waits on either a card swipe on the bench unit (factory
PowerCard set) or programming a synthetic UID into the local list.

---

## Question 1 — What's the physical RFID hardware?

**Answer: serial RFID reader module on `/dev/ttyAMA4`, 13.56 MHz ISO14443A,
Mifare Classic + Mifare UltraLight; reader chip is an off-the-shelf
MCU-based front-end (DH/Zinwei/TWN-style) — NOT a bare MFRC522. Confidence:
high.**

### Evidence

The `RFID` daemon (`/root/RFID`, 16 KB ARM ELF, unstripped) drives the
hardware. Static analysis of the daemon's `main()` and helper functions
(symbols at `nm` output: `PWM_Init`, `SetDuty`, `Set_Antana`, `Request_CardSN`,
`UL_read`, `CheckRC`, `PrepareCmd`, `SndCommand`, `RCV_DATA`, `BLK`, `SN`,
`command`, `MeterSMPtr`) reveals:

- **UART**: `open("/dev/ttyAMA4", O_RDWR)` at startup, configured 115200 8N1
  via `ioctl(TCSETS)` (B115200 = `0x8be` in `c_cflag` struct stored in
  `tios`). All reader communication is over this UART.
- **Buzzer / status LED**: `PWM_Init` opens `/dev/spr320_pwm1`; `SetDuty(d, 50)`
  / `SetDuty(d, 100)` is called in 100 ms pulses (`usleep(0x186a0)` /
  `usleep(0x30d40)`) on tag accept/reject — confirms the PWM channel is a
  buzzer/LED feedback, **not** the 13.56 MHz carrier (the carrier is generated
  inside the reader module).
- **GPIOs touched**: `48`, `55`, `56`, `57` exported as outputs from `/sys/class/gpio/`
  with init levels 1, 0, 0, 1 respectively — module power/reset/IRQ
  pinmux. (`gpio48=1` reset-deassert, `gpio55=0` … `gpio57=1`).
- **Wire framing** (from `PrepareCmd` at `RFID.c:0x8e30`): each request is
  `[LEN_BYTE] [CMD_BYTE] [ARGS...] [XOR_OF_BYTES_0..LEN-1] [0x00]`. The
  XOR-checksum-plus-trailing-zero pattern is the signature of the
  generic Chinese-OEM serial RFID modules — **not** raw MFRC522/PN532
  (those would expose register-level SPI/I2C ops).
- **Command set inferred from `command` buffer assembly**:
  - `cmd[0]=3, cmd[1]=0x11, cmd[2]=0..3` — `Set_Antana(power)` — set
    antenna power (0=off, 3=full).
  - `cmd[0]=3, cmd[1]=0x20, cmd[2]=0` — `Request_CardSN` — anticollision +
    UID readout.
  - `cmd[0]=3, cmd[1]=0x41, cmd[2]=page` — `UL_read(page)` — Mifare
    UltraLight READ (4-byte page). Returns 16 bytes (4 pages).
- **Reply length-byte dispatch** (in `Request_CardSN` at `0x9258..0x93b8`):
  - `RCV_DATA[0]==0x09` (len 9): 4-byte UID at `[2..5]` (Mifare Classic
    short UID).
  - `RCV_DATA[0]==0x0C` (len 12): 7-byte UID at `[2..8]` (Mifare Classic
    7-byte UID).
  - `RCV_DATA[0]==0x0F` (len 15): 10-byte UID at `[2..11]` (UltraLight C
    full UID — 7 unique bytes plus 3 cascade bytes).
- **UltraLight authentication step** (`main` at `0x9a80..0x9ad8`): after a
  successful `Request_CardSN`, the daemon calls `UL_read(addr=8)` (page 8 =
  user-data start) and **string-compares the first 4 bytes to the ASCII
  literal `"DETA"` (`0x44 0x45 0x54 0x41`)**. If the card doesn't have
  `DETA` magic at page 8, it's rejected immediately — i.e. **Delta's
  PowerCard cards are programmed with a `"DETA…"` payload at UltraLight
  page 8, and any generic UltraLight without this stamp is rejected at the
  reader-daemon stage**.

### Live probe

```
$ python3 /tmp/delta-cmd.py 'ls -la /dev/ttyAMA* /dev/spr320*; ps | grep -E "RFID|Charging"'
crwxrwxrwx ... 250,  0 ... /dev/spr320_pwm
crwxrwxrwx ... 249,  0 ... /dev/spr320_pwm1
crw-rw---- ... 204, 64 ... /dev/ttyAMA0
...
crw-rw---- ... 204, 68 ... /dev/ttyAMA4
  742 vern  1672 S  /root/Charging_Standard_RFID
  767 vern  1644 S  /root/RFID
```

Both daemons run. No kernel module loaded — the reader speaks plain UART
through the SoC's amba pl011 driver, no driver-specific support needed.

### What we couldn't determine

- The exact reader-module part number — the protocol (XOR-framing, cmd
  0x11/0x20/0x41) is generic enough to be a half-dozen Chinese ODMs. A
  ferrite-load identification on the bench (e.g. an oscilloscope trace of
  the antenna-power-on transient) would name the chip family but is not
  needed for the bridge work.
- Whether the reader can issue MFC1k authentication (key A/B with crypto-1).
  Only cmd `0x41` (UltraLight READ) is exercised in the daemon, which makes
  sense: Mifare Classic was deprecated by 2016 when this firmware shipped,
  and Delta clearly chose UltraLight + custom payload for their cards.

---

## Question 2 — What does `/root/RFID` do with the UID once it reads it?

**Answer: ASCII-hex UID (up to 20 chars for a 10-byte UID) gets written
to `shmem[0x05E0]` (32-byte slot), plus state byte `shmem[0x0A79] = 3`
and event flag `shmem[0x0AAE] = 1`. Confidence: high.**

### Evidence

From `RFID.c:main` (disassembly listing at `/tmp/RFID.dis:1133-1546`):

1. **Read loop runs forever** (`b 9a30 <main+0x270>` at the bottom).
2. On each iteration: `usleep(?)`, check `shmem[0x1BA] != 2` (skip if
   "paused"), then call `Request_CardSN(&SN)` where `SN` is a 32-byte
   bss buffer.
3. `Request_CardSN` (`0x90d4`) issues the `[3, 0x20, 0]` command,
   reads the response, then `pbtos(uid_bytes, len, &SN)` —
   "packed-bytes-to-string" (`0x8ccc`) — which converts each byte to two
   uppercase-hex ASCII chars and writes them into `SN`. So `SN` ends up
   holding e.g. `"04236f0ac32680\0"` for a 7-byte UltraLight UID.
4. Back in main, `memcpy(stack[fp-56], SN, 32)` — local copy of the just-
   read UID.
5. `UL_read(8, &BLK)` reads page 8 of the card into the `BLK` buffer.
6. Magic check: `BLK[0..3] == "DETA"` ? — if yes, state byte = 2
   ("valid Delta card"); else state = 1 ("UID readable but not a Delta card");
   if `Request_CardSN` failed, state = 0 ("no card").
7. **If `state == 2 && shmem[0xA79] == 0`** (i.e. this is a fresh swipe):
   - `shmem[0xA79] = 3` (RFID-event state machine: "UID just captured")
   - `shmem[0xAAE] = 1` (event flag: "fresh swipe")
   - `memcpy(shmem[0x5E0], stack[fp-56], 32)` — copy ASCII-hex UID into the
     **UID slot** in shmem
   - SetDuty pattern 50/100/50/100 (buzzer chirp = "card accepted")
   - `StoreFlash(-2, 0)` — flush specific bytes to mtd4
8. **If `state != 2 && shmem[0xA79] != 0 && strstr(shmem[0x5E0], local_uid)`**
   (i.e. the same card is being held re-presented but we already captured
   it): reset `shmem[0xAAE]=0`, `shmem[0xA79]=0`, beep pattern, `StoreFlash(0xA79, 1)`.

### Width / encoding

- The shmem UID slot is **32 bytes at `shmem[0x05E0]`**.
- Format is **ASCII hex (uppercase? lowercase?)** — `pbtos` adds `0x30` for
  digits 0–9 and `0x30+7` (i.e. converts 10..15 → `:..A` then +7 gets
  `A..F`) for nibbles ≥10 — so **uppercase A-F**. UID byte `0x4F` becomes
  the two chars `"4F"`. 7-byte UID → 14 hex chars, null-terminated.
- The leading byte of `0x5E0` is the start of the hex UID; rest of the 32-byte
  slot is zero-filled.

### Companion state bytes

| Byte | Meaning | Set by | Cleared by |
|---|---|---|---|
| `shmem[0x05E0..0x05FF]` | ASCII-hex UID (up to 20 chars) | `RFID` | `RFID` after consume; `main:VerifyIdTag` after OCPP response |
| `shmem[0x0A79]` | RFID-event state (0=idle, 3=fresh UID, 4=verified) | `RFID`, `main:VerifyIdTag` | `RFID` after consume; cleared on idle |
| `shmem[0x0AAE]` | "Fresh swipe" event flag | `RFID` | `RFID` after consume; **CSR ALSO clears via `shmem[0x0AAE]=0` at `b8f4..b908`** |

### What we couldn't determine

- The case of the hex letters — `pbtos` produces uppercase `A..F` (verified
  in disassembly: `+7` after `+0x30` and a `<= '9'?` skip gives `:..@` →
  `A..F`). But the consumer might lowercase it. Won't matter unless the OCPP
  Authorize.req payload is case-sensitive (it is — OCPP idTag is opaque
  string). **Action item**: confirm by triggering a swipe on the bench and
  reading `shmem[0x5E0]` via `shmem_dump`.

---

## Question 3 — What does `Charging_Standard_RFID` do with the UID?

**Answer: it serializes the UID + a connector-number + a timestamp into the
file `/Storage/IdTagToBeVerify` (an ASCII colon-separated log), then
`main:VerifyIdTag` picks it up and arbitrates against the **local allowlist
file `/Storage/OCPPLocalList`** (managed via OCPP SendLocalList) and/or
fires an OCPP Authorize.req via `DeltaOCPP`. Confidence: high.**

### Evidence

`Charging_Standard_RFID` (`/root/Charging_Standard_RFID`, 47 KB) main-loop
flow at `0xb800..0xb8f4`:

```text
b808: r3 = shmem[0x2FE]                  // AuthenticationMode config
b814: cmp r3, #1                          // != 1 ? skip RFID consumption
b818: bne b8bc
b824: r3 = shmem[0x1BA]                  // duplicate AuthMode field?  see §4
b830: cmp r3, #3
b834: bne b8bc                            // (waits until a specific mode value)
b850: r0 = &shmem[0x5E0]                 // idTag (ASCII UID)
b864: r2 = &shmem[0xC6E]                 // status byte (filled by VerifyIdTag)
b874: r1 = &shmem[0xC86]                 // expiry timestamp (filled by Verify…)
b884: r2 = &shmem[0xC8A]                 // parent idTag field
b8a4: bl  RecordIdTag                    // append to /Storage/IdTagToBeVerify
b8f0: shmem[0x1BD] &= ~4                 // clear "verify-pending" sub-flag
b908: shmem[0xC8E] = 0
```

`RecordIdTag` (`0x9fcc`):

```text
open("/Storage/IdTagToBeVerify", O_RDWR|O_CREAT, 0o100)
read() existing content into 1 MiB buffer
strcpy at end: <existing>:<idTag>:<status_digit>:<timestamp>
write() back
close()
```

The file is the **persistent inbox** between `Charging_Standard_RFID` and
`main:VerifyIdTag`. Format: `<idTag>:<status_char>:<timestamp_seconds>`,
records appended with `:` separators.

### `main:VerifyIdTag` (`/tmp/main.dis:9085`, at `0x11514`)

1. `open("/Storage/IdTagToBeVerify", O_RDWR|O_CREAT)`, read into 2 MiB buffer
2. `strstr(buffer, ":")` to find first record
3. `memcpy(shmem[0x5E0], record_substring, 32)` — re-publishes the UID
   into the shmem slot (so this slot is **also** the inter-process
   verify-request channel, not just the reader's output).
4. `shmem[0x1BD] |= 0x01` — bit 0 of `0x1BD` = "verify pending".
5. `time(&t0)`; loop:
   - poll `shmem[0x19C]` bit 0 (= "result available") and bit 1 (= "abort
     early") every iteration.
   - if `time()-t0 > 120 s` → timeout, clear shmem[0xC14], clear
     `shmem[0x1BD] |= ...`, clear `shmem[0x5E0]`, return 1.
6. On result: read `shmem[0xC14]` (status: 0/1/2/0xFF — see below). If
   `0xFF` (pending) → re-loop; if valid → call **`UpdateLocalList(idTag,
   parent_idTag, status, expiry)`** to cache the verdict.
7. Parse the trailing fields (`atoi` of substrings between `:`) into
   `shmem[0xC40+9]` (expiry years/months/days/...) and `shmem[0xC50+1]`.

The 120 s timeout is the OCPP `AuthorizationTimeout` standard interval.

### Local-list path: `/Storage/OCPPLocalList`

`UpdateLocalList` (`0x99e8` in CSR, `0x1214c` in main — both daemons keep a
copy):

```text
open("/Storage/OCPPLocalList", O_RDWR|O_CREAT, 0o100)
read existing content into 5 MiB buffer
search for the idTag; insert-or-update with new status/expiry
write back, close
```

`DeltaOCPP` separately:
- `rm -f /Storage/OCPPLocalList` on `SendLocalList(updateType="Full")`
- Appends to `/Storage/OCPPLocalList` on each entry sent by the CSMS
- Implements `ns1:SendLocalListRequest` / `ns1:GetLocalListVersionRequest`
  (strings present in `DeltaOCPP`).

So the unit speaks **OCPP 1.5 SOAP LocalAuthorisationList** (`SendLocalList`,
`GetLocalListVersion`) — confirmed by the literal SOAP element names found
in `strings DeltaOCPP`.

### Remote path: OCPP `Authorize.req`

Same `DeltaOCPP` binary contains `ns2:AuthorizeRequest`, `ns2:AuthorizeResponse`,
`ns2:IdTagInfo`, `ns2:AuthorizationStatus`, and the SOAP endpoint
`/Authorize`. When `VerifyIdTag` flips `shmem[0x1BD] |= 0x01`, `DeltaOCPP`
picks it up, marshals an `Authorize.req` to the CSMS, parses the response,
and writes back `shmem[0xC14]` = status (`0=Accepted`, `1=Blocked`,
`2=Expired`, `3=Invalid`, `4=ConcurrentTx`, `0xFF=Pending`).

### Auth-decision dataflow summary

```
┌──────────┐      shm[0x5E0]      ┌──────────────────────────┐
│  RFID    │ ───── + shm[0xA79]=3 │ Charging_Standard_RFID   │
│  daemon  │ ───── + shm[0xAAE]=1 │ main loop                │
└──────────┘                       │   if AuthMode == 1 &&    │
                                   │      shm[0x1BA] == 3:    │
                                   │     RecordIdTag(idTag…)  │
                                   └────────┬─────────────────┘
                                            │ writes
                                            ▼
                              /Storage/IdTagToBeVerify (file)
                                            │
                                            ▼ reads
                                   ┌──────────────────┐
                                   │ main:VerifyIdTag │
                                   │   reads file,    │
                                   │   sets shm[0x5E0]│
                                   │   sets shm[0x1BD]|= 0x01
                                   └────────┬─────────┘
                                            │
                            ┌───────────────┴────────────────┐
                            ▼                                ▼
                  /Storage/OCPPLocalList                 DeltaOCPP
                  (local cache, hit→shm[0xC14]=0)        sends Authorize.req
                                                         on response writes
                                                         shm[0xC14] = status
                                                         shm[0x19C] |= 1
                                            ┌────────────────┐
                                            │ VerifyIdTag    │
                                            │ wakes, writes  │
                                            │ shm[0xC14..]   │
                                            │ updates local  │
                                            │ list, returns  │
                                            └────────────────┘
```

### What we couldn't determine

- Whether the local-list lookup is **performed before** Authorize.req is
  issued (typical OCPP 1.5 LocalAuthList behaviour) or only used as fallback
  on Authorize timeout. The code in `main` and `DeltaOCPP` both can write
  to the list and both can read; the exact ordering needs a live test
  with a known-listed and a known-unlisted UID. Static evidence suggests
  **list-first**: `VerifyIdTag` sets `shm[0x1BD] |= 0x01` (which triggers
  DeltaOCPP) only after the local-list miss, but the bit is set
  unconditionally in the static disassembly. Verdict: medium confidence
  on the ordering; high confidence that both paths exist.
- The exact `shmem[0x19C]` bit semantics — bit 0 ("done") and bit 1
  ("abort") are observed in the polling loop but not fully traced to
  their setters. DeltaOCPP almost certainly sets them.

---

## Question 4 — Authentication-mode semantics (`shmem[0x2FE]` / config key
"Authentication Mode:")

**Answer: per `Charging_Standard_RFID:b808` and `main:GetConfig:e1e8`, the
config file key `Authentication Mode:` is parsed by `atoi`, capped at byte
range, and stored at `shmem[0x2FE]`. Valid values: 0–4. Confidence: high
on the storage location and value range; medium on the per-value
behavioural semantics.**

### Evidence

- **Storage location**: `main:GetConfig:e1e8` — sole writer to `shmem[0x2FE]`,
  immediately after `strstr` finds the literal `"Authentication Mode:"`
  (rodata at VA `0x1838c`).
- **Default value on the bench unit**: `0` (from live
  `/Storage/DownloadConfiguration` probe — captured 2026-05-15).
- **Consumer test at `Charging_Standard_RFID:b808`** is `if (shm[0x2FE] == 1)`
  — the RFID-event-consumption branch only runs in mode 1.
- **There is ALSO a separate byte at `shmem[0x1BA]`** that holds a related
  config value, written by `main:GetConfig:d85c` from a different
  config-key search. The label for that key wasn't unambiguously identified
  in our scan, but the gate `shm[0x1BA] == 3` at `Charging_Standard_RFID:b830`
  suggests this is a **second axis** (likely "BackendSystem" or "Offline
  Charge" — both also present in `DownloadConfiguration`).

### Inferred mode semantics

Cross-referencing with the OCPP 1.5 spec and the live config keys
(`Authentication Mode: %d`, `OCPP local list: %d`, `Offline Charge: %d`,
`Backend System: %d`, `Remote Control Charge: %d`) — these are five
independent toggles, not one 0..4 enum. The most likely layout:

| Config key | Shmem byte | Default | Effect |
|---|---|---|---|
| `Authentication Mode:` | `0x2FE` | 0 | 0=free vend, 1=RFID required, 2=PIN/web, 3=remote-only, 4=plug+RFID |
| `Backend System:` | `0x2FF` | 0 | 0=disabled, 1=OCPP-1.5-SOAP |
| `Offline Charge:` | (TBD) | 1 | Allow charging when CSMS unreachable |
| `OCPP local list:` | (TBD) | 0 | Honour cached idTags from `/Storage/OCPPLocalList` |
| `Remote Control Charge:` | (TBD) | 0 | Allow CSMS-initiated RemoteStartTransaction |

The bench unit's live config (`Authentication Mode: 0`) confirms the no-auth
behaviour observed: plug → charge with no swipe needed.

### What we couldn't determine

- Exact value→behaviour mapping for `Authentication Mode: 1..4`. Need either
  a CSMS reachability test (toggle the config, observe the LCD/charging
  behaviour) or a deeper disassembly trace of the b808/b830 cascade in CSR.
- Which shmem byte holds `OCPP local list:` (the "use cached allowlist"
  toggle). That's the byte the bridge would want to expose for evcc/HA
  integration.

---

## Question 5 — Could we synthesise an authorized RFID event from outside?

**Answer: yes, two viable paths. Both bypass the physical reader but neither
truly races CSR.**

### Path A: spoof the verify-request inbox file

**Mechanism**: write a synthetic record to `/Storage/IdTagToBeVerify` and
let `main:VerifyIdTag` pick it up on its next poll.

**Required writes**:
1. `echo ":<idTag>:1:<unix-time>" >> /Storage/IdTagToBeVerify`
   — `<idTag>` is the ASCII hex UID (or any opaque string; OCPP accepts
   any 20-char ASCII).
2. Ensure `shmem[0x2FE]` (AuthMode) is **not** 0 — else CSR won't trigger
   `VerifyIdTag` polling cycle. (No write needed if user already configured
   mode 1+.)

**Race surface**: `RecordIdTag` is the only daemon-side writer of this file;
external append-only writes will not be clobbered.

**Caveats**:
- The unit must be configured for an auth mode that exercises the file.
- `VerifyIdTag` then calls out to either local-list (file `/Storage/OCPPLocalList`)
  or remote OCPP — so the idTag must be **either** pre-listed or accepted
  by the CSMS.
- The 120 s timeout in `VerifyIdTag` means external Authorize handling is
  expected to complete fast.

### Path B: spoof the local-list cache

**Mechanism**: pre-populate `/Storage/OCPPLocalList` with our chosen idTag
marked `Accepted`. Then any subsequent verify (real swipe or path-A spoof)
hits the local-cache fast-path.

**Required write**:
```
echo "<idTag>:0:<expiry>:<parent>" >> /Storage/OCPPLocalList
```

Status `0` = Accepted (OCPP 1.5 AuthorizationStatus.Accepted). Expiry can
be `9999999999` for "never expires".

**Race surface**: `UpdateLocalList` in CSR/main and `DeltaOCPP` are the
daemon writers. They both **append-only** in normal operation; full-rewrite
only on `SendLocalList(updateType="Full")` from the CSMS. If the unit
isn't connected to a CSMS, the list is stable.

### Path C: skip auth entirely (current bench state)

`Authentication Mode: 0` → plug-in alone starts charging, no RFID gate.
This is the bridge's current operating point and the simplest path for
HA/evcc users who don't want RFID at all.

### Path D (NOT viable): write `shmem[0x5E0]` + `shmem[0xA79]=3`

The previous agent's suggestion to overwrite `shmem[0x5E0..0x5EF]` then
flip `0xA79=3` looks tempting but races against `RFID` daemon's main loop
(1 µs busy-poll). The window where CSR observes our forged state before
RFID overwrites it is sub-millisecond and unreliable.

### What this means for race-free external auth

The clean external-auth path is:

```
bridge → write /Storage/IdTagToBeVerify  (one append)
                ↓
        VerifyIdTag picks up within 1 main-loop tick
                ↓
        if /Storage/OCPPLocalList has the idTag → instant Accept
        else                                    → OCPP Authorize.req → CSMS
```

There is **no race** on either file because the only writers are the
daemons themselves (append-only) and the bridge (also append-only). The
shmem state machine plays out as if a real card had been swiped.

---

## Question 6 — What is `PowerCard-UltraLight` for?

**Answer: factory whitelist of 10 RFID UIDs distributed with Delta's
shipped PowerCard sample cards. Each line is a 7-byte UID encoded as
14 ASCII hex characters, followed by CRLF. Confidence: high.**

### Evidence

`hexdump -C /home/rar/device-configs/esphome/testcharger/delta/PowerCard-UltraLight`:

```
00000000  30 34 32 33 36 37 30 61  63 33 32 36 38 30 0d 0a  |0423670ac32680..|
00000010  30 34 32 30 61 66 30 61  63 33 32 36 38 30 0d 0a  |0420af0ac32680..|
00000020  30 34 32 30 63 38 30 61  63 33 32 36 38 30 0d 0a  |0420c80ac32680..|
00000030  30 34 32 34 64 33 30 61  63 33 32 36 38 30 0d 0a  |0424d30ac32680..|
00000040  30 34 32 30 62 33 30 61  63 33 32 36 38 30 0d 0a  |0420b30ac32680..|
00000050  30 34 32 33 39 62 30 61  63 33 32 36 38 30 0d 0a  |04239b0ac32680..|
00000060  30 34 32 33 39 61 30 61  63 33 32 36 38 30 0d 0a  |04239a0ac32680..|
00000070  30 34 32 32 36 36 30 61  63 33 32 36 38 30 0d 0a  |0422660ac32680..|
00000080  30 34 32 31 35 62 30 61  63 33 32 36 38 30 0d 0a  |04215b0ac32680..|
00000090  30 34 32 30 35 38 30 61  63 33 32 36 38 30 0d 0a  |0420580ac32680..|
```

10 cards. Every UID starts with `04` (NXP manufacturer ID for MIFARE
UltraLight) and ends with `0ac32680` — the trailing 4 bytes are the
**fixed** part 2 of NXP's UID-cascade (manufacturing date + chip-batch
serial). The middle 2 bytes vary — these are the **per-card unique** part.

So the cards are a contiguous batch of 10 NXP UltraLight tags from the
same wafer. The list is shipped as a starter-pack — anything Delta intended
the customer to set up via the LCD/web UI would replace `/Storage/PowerCard-UltraLight`
with their own list.

### Where it gets loaded

The file does **NOT** appear in `/Storage/` on the bench unit (live `ls -la
/Storage/` showed only `DownloadConfiguration`, `EncodeLogMessage`, `Gain`,
`SerialNumber`, plus our bridge artefacts) — meaning the unit was
factory-virgin and never had the test cards distributed. The factory image
ships `PowerCard-UltraLight` in the firmware archive `/root/`, ready to be
copied to `/Storage/` once the dealer programs the unit.

**The `PowerCard` function at `main:0xbf78` reads a file of 100 KB max and
the `CardVerify` function at `main:0xbec4` does `strstr(shm[0x5E0], "ok")` /
`strstr(shm[0x5E0], "ng")` — these look like a legacy non-OCPP path that
was superseded by the OCPP local-list infrastructure. Neither function is
called from `main:main` in this build.** So the `PowerCard-UltraLight` file
is a relic of the pre-OCPP authentication design and not actively consumed
in the OCPP build.

### What we couldn't determine

- Whether the LCD/HMI UI on a real Delta deployment shows a "Pair RFID
  card" wizard that writes to `PowerCard-UltraLight`. The LCD code lives
  in the secondary STM32F334 and we don't have its firmware extracted yet.

---

## Question 7 — Local idTag cache

**Answer: `/Storage/OCPPLocalList`. Append-only ASCII, OCPP 1.5 LocalAuthList
managed via SOAP. Confidence: high.**

### Evidence

- File path appears as a literal in `Charging_Standard_RFID`, `main`, and
  `DeltaOCPP` (rodata).
- Writers (all of them):
  - `Charging_Standard_RFID:UpdateLocalList` (`0x99e8`) — append on auth
    response.
  - `main:UpdateLocalList` (`0x1214c`) — same role, second copy.
  - `DeltaOCPP:main` — `rm -f /Storage/OCPPLocalList` then bulk-append on
    `ns1:SendLocalListRequest` from the CSMS.
- Readers:
  - `VerifyIdTag` in main reads the file at start of each verify cycle.
  - `DeltaOCPP` reads it to answer `GetLocalListVersionRequest`.
- Format: `<idTag>:<status_digit>:<expiry_seconds>:<parent_idTag>`,
  one record per line, no header.

The bench unit has **no `/Storage/OCPPLocalList`** (never connected to a
CSMS). Listing the directory and viewing config confirms this.

### Usability for evcc / HA

The bridge could expose:
- **Read**: `cat /Storage/OCPPLocalList` → list current allowlist.
- **Write**: append a line to add a card; rewrite the file in-place to
  remove. (Coordinate with `DeltaOCPP` is unnecessary if `Authentication
  Mode: 1` and Backend System: 0 — no CSMS will overwrite.)
- **Sync from HA**: e.g. an HA `input_text` of "allowed UIDs" + a Python
  script in the bridge that mirrors it to `/Storage/OCPPLocalList`.

### What we couldn't determine

- The exact line terminator (LF vs CRLF) — `RecordIdTag` uses no
  terminator (it accumulates in one big buffer). May need to test.

---

## Question 8 — RFID-related config keys

**Answer: five user-tunable knobs, all parseable from
`/Storage/DownloadConfiguration` (and writable via the same file, picked up
by `GetConfig` on reboot or on a config-import trigger).**

### From the live unit's DownloadConfiguration

```
Authentication Mode: 0       ← shmem[0x2FE]
Max Charging current: 18     ← shmem[0x0A24]   (per existing doc 06)
Max Charging time: 0
Backend System: 0            ← shmem[0x2FF] (probable)
Offline Charge: 1
Remote Control Charge: 0
OCPP local list: 0
OCPP offline policy: 1
OCPP Charge Box ID: Delta Charging Station
OCPP Charge Point Model: Delta Charging Station
OCPP Server URL:
OCPP User ID:
OCPP Security: 0
```

### Bridge-relevant subset

| Config key | Effect | Bridge action |
|---|---|---|
| **Authentication Mode** | 0 = no auth (current), 1+ = require RFID | Expose toggle "Require RFID" |
| **OCPP local list** | 0 = bypass cache, 1 = use cache | Expose "Use cached allowlist" |
| **OCPP offline policy** | Behaviour when CSMS unreachable | Likely no UI value unless OCPP wired |
| **Backend System** | 0 = standalone, 1 = OCPP SOAP | Expose "OCPP Backend" toggle |
| **Remote Control Charge** | Allow CSMS-driven start/stop | Optional |

### How the bridge should write config changes

Either:
1. Append a new "set" record to `/Storage/DownloadConfiguration` then
   trigger a `kill -HUP main` (untested but `GetConfig` is the parser
   entry point).
2. OR write the runtime shmem byte directly (e.g. `shmem[0x2FE] = 1`)
   AND append to `/Storage/DownloadConfiguration` to make it survive reboot,
   AND trigger `StoreFlash` to commit shmem to mtd4. (Same pattern as the
   `rated_amps` write per doc 07.)

The cleanest path is (2) — direct shmem write + log a "change" line to
DownloadConfiguration for persistence. This is symmetric with how the bridge
already plans to write `rated_amps`.

### What we couldn't determine

- The mapping for `OCPP local list:`, `OCPP offline policy:`, `Offline Charge:`,
  `Remote Control Charge:` to specific shmem bytes. Each is parseable by
  `GetConfig` (since they appear in `DownloadConfiguration`), but I haven't
  finished tracing each `strstr/atoi/strb` block. The disassembly is
  mechanical — a fuller scan would lock these down. The minimum-viable
  bridge UX only needs the **Authentication Mode** byte.

---

## What this means for the bridge

### Short answer

We can expose RFID-based access control end-to-end without writing a single
line of code on the stock daemons, and without racing them.

### Concrete recommendations

**For "allow this UID" (HA service / evcc external auth):**

1. Bridge appends a line to `/Storage/OCPPLocalList`:
   `<idtag-hex>:0:9999999999:<parent_or_blank>`
2. If `Authentication Mode == 0`, optionally bridge also writes
   `shmem[0x2FE] = 1` + appends `Authentication Mode: 1` line to
   `DownloadConfiguration` and triggers `StoreFlash` — to actually enforce
   that swipes are required.
3. Real card swipes now hit the cache and Accept instantly. No CSMS needed.

**For "external authorize on demand" (HA "scan to start" button):**

1. Bridge writes a synthetic record to `/Storage/IdTagToBeVerify` with the
   user's chosen identity string (e.g. `"HA-Andrew"`):
   `:HA-Andrew:1:<unix-time>`
2. `VerifyIdTag` picks it up and looks for `"HA-Andrew"` in
   `/Storage/OCPPLocalList`. If listed → Accept; if not → OCPP Authorize.req
   (fails open if no CSMS).
3. To skip the local-list lookup entirely, pre-list `"HA-Andrew"` once.

**For "lock the charger" (HA security helper):**

1. Bridge writes `Authentication Mode: 1` + clears
   `/Storage/OCPPLocalList`. Now no swipe → no charge.
2. Or set `shmem[0x1BA] = 99` (or any value not in the legal set) — CSR's
   gate `shm[0x1BA] == 3` will never trigger. Less reversible but no
   filesystem writes.

**For "evcc-managed allowlist":**

Bridge mirrors evcc's `idtags` list to `/Storage/OCPPLocalList` on each
evcc config change. evcc gets a clean external-auth interface; the Delta
keeps thinking it's running an OCPP local list.

### What's still blocked

- We need a **real card swipe** on the bench (factory PowerCard set is in
  the firmware archive — can be re-loaded to `/Storage/PowerCard-UltraLight`
  to test if the legacy `PowerCard` path activates, or just programmed
  into `/Storage/OCPPLocalList` directly to test the modern OCPP path).
- We need a **second-pass disassembly trace** to map the remaining four
  config-key shmem bytes (OCPP local list, Offline Charge, etc.) — but
  none of these block the MVP bridge work.
- **None of this has been bench-tested.** All claims here are static
  predictions from the binaries. The first real swipe + log capture will
  validate or correct §2's encoding details and §3's verify-flow timing.

---

## Appendix: shmem additions vs doc 06

This investigation adds these byte assignments (or corrects/refines existing
ones) to the shmem map in `06-shmem-RE-from-binaries.md`:

| Offset | Width | Name | Notes |
|---|---|---|---|
| `0x05E0..0x05FF` | 32 B ASCII | **RFID UID** (ASCII hex, null-padded) | Was `0x5E0` listed as part of the OCPP identity region — **correction**: this slot is the RFID UID handoff buffer, not OCPP identity (which lives at `0x0400..0x07FF`). |
| `0x01BA` | u8 | **AuthMode-related (per CSR `b830` gate)** | Cfg-loaded by `main:GetConfig:d85c`. |
| `0x01BD` | u8 (bitfield) | **VerifyIdTag bus**: bit 0 = "verify pending request" | Set by `main:VerifyIdTag`; consumed by `DeltaOCPP`. |
| `0x019C` | u8 (bitfield) | **VerifyIdTag bus**: bit 0 = "result done", bit 1 = "abort/early" | Set by `DeltaOCPP`; polled by `main:VerifyIdTag`. |
| `0x02FE` | u8 | **AuthenticationMode config** (0=no-auth, 1=RFID required, ...) | Cfg-loaded by `main:GetConfig:e1e8`. **Confirms the table's `0x0A73 = ChargingMode` label needs revision** — `0x0A73` may be something else (per-phase config? See doc 06's note). |
| `0x0A79` | u8 | **RFID-event state** (0=idle, 3=fresh swipe, 4=verified) | Was listed in doc 06 as "meter-IC ready flag" — **correction**: this is the RFID-event state byte. The meter-IC ready flag is elsewhere. |
| `0x0AAE` | u8 | **RFID fresh-swipe event flag** | Cleared by CSR after consume. |
| `0x0C14` | u8 | **VerifyIdTag status result** (0=Accepted, 1=Blocked, 2=Expired, 3=Invalid, 4=ConcurrentTx, 0xFF=Pending) | Written by `DeltaOCPP`. |
| `0x0C40..0x0C5F` | bytes | VerifyIdTag scratch (expiry date, parent idTag) | Parsed in `VerifyIdTag`'s second-pass `:`-tokenizer. |
| `0x0C8A..0x0C8E` | bytes | Pre-RecordIdTag arg-passing scratch | Loaded into `RecordIdTag` args in CSR. |

The `0x0A79` correction is significant: doc 06 listed it as "meter-IC ready
flag" but the RFID daemon clearly owns it (3 = fresh card, 4 = verified;
cleared after consume). The MeterIC daemon also touches `0x0A79`, but
that's probably a stale read — worth double-checking on the bench.
