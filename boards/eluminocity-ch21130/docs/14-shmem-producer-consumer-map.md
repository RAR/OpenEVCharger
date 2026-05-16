# Producer/consumer map of stock Delta shmem + replacement strategy

**Date:** 2026-05-16 (M5 follow-up to `docs/13`)
**Method:** Pure static analysis. No bench access required. Ran
`companion/tools/find_shmem_accesses.py --csv` against all 12 charging-stack
binaries in the rootfs dump, joined the per-binary CSVs by offset, produced
the producer→consumer matrix below.

**Why this exists:** before we can safely replace any stock daemon X, we
need to know **who reads what X writes** and **who writes what X reads**.
Without that, a clean-looking swap can race against a peer we didn't know
existed. The matrix gives us the answer for every byte in shmem the stock
charging stack touches.

**Coverage warning:** the scanner only recognises the GCC-4.6/`-O0/1`
register-chain patterns we observed in Adc/MeterIC_new — multi-step
`add #imm/add #imm` splits, fp-relative spills, and PLT-indirect base
loads are skipped. Expect ~10 % false negatives. Trace correlation is the
ground truth.

---

## 1. Boot orchestration (where each daemon comes from)

From `/etc/inittab` → `/etc/rc` → `/etc/funs`:

```
/etc/funs:
  /root/main &           ← spawns RFID, Pri_Comm, LED_control,
                            Charging_Standard_RFID, DeltaOCPP, FlashLog
                            (as children, not via /etc/funs)
  /sbin/watchdog ...
  /root/RTC &
  sleep 5
  /root/Adc &            ← NOT a child of main
  /root/MeterIC_new &    ← NOT a child of main
  /root/snmpd &
  /root/ErrorHandle &    ← NOT a child of main
```

Implication: **killing `main` orphans 6 children**; killing an `/etc/funs`
daemon doesn't auto-respawn (nobody supervises them). The session today
confirmed this: kill `Adc` + `MeterIC_new` → `main` doesn't relaunch them.

## 2. Daemon role summary (static-RE only)

| Daemon                  | Size  | Role                                                                 | Critical APIs                             |
|-------------------------|------:|----------------------------------------------------------------------|-------------------------------------------|
| `main`                  | 86 KB | Supervisor + HMI + contactor + connector lock + Wi-Fi + 3G + USB FWUP + factory reset + RFID auth flow | gpio78/82/68/44/80/85/52/83/63, can0, /sbin/ifconfig, /sbin/udhcpc, /UsbFlash/, /Storage/EncodeLogMessage |
| `Pri_Comm`              | 35 KB | STM32-safety-MCU link (docs/01, docs/11)                              | `/dev/ttyAMA1` 115200 SLIP                |
| `Charging_Standard_RFID`| 47 KB | J1772 pilot PWM generator + AC-drop detect + relay drive + GMI button | `/dev/spr320_pwm`, `/dev/ttyAMA1` (shared with Pri_Comm!), gpio32/34/44/54/55/56/57 |
| `MeterIC_new`           | 35 KB | Meter chip UART poller (docs/13)                                      | `/dev/ttyAMA2` 2400, `/Storage/{SerialNumber,Gain}`, `/root/Energy` |
| `Adc`                   | 24 KB | CP-sense classifier (docs/13)                                         | `/dev/adc0`                               |
| `LED_control`           | 13 KB | LED ring drive                                                        | gpio55/56/57 (overlaps `Charging_Standard_RFID`!) |
| `ErrorHandle`           | 22 KB | SNMP trap emitter, 20+ OIDs under `.1.3.6.1.4.1.6785.1.8.3.X`         | shells `/root/snmptrap`                   |
| `FlashLog`              | 11 KB | Periodic flush of shmem state to `/root/Energy`, `/root/PassTime`     | `/dev/mtdblock4`                          |
| `RTC`                   | 13 KB | RTC chip read/write                                                   | `/dev/i2c-0`                              |
| `RFID`                  |       | Replaced by `delta-bridge` (docs/10/12)                               | (was `/dev/ttyAMA4` + `/dev/spr320_pwm1`) |
| `DeltaOCPP`             | 734 KB| OCPP 1.6-JSON over WSS (massive); has `reboot -f` + `/backup/*` paths | OpenSSL, `/Storage/OCPPLocalList`, gpio80/85 |
| `snmpd`                 |       | Upstream net-snmp — unchanged, no RE                                  |                                           |

## 3. Single-writer / multi-reader shmem zones

These are the safe, well-bounded "ownership" zones — replacing the single
writer is a self-contained swap:

| Offset    | Size | Writer            | Readers                                               | Meaning                              |
|-----------|-----:|-------------------|-------------------------------------------------------|--------------------------------------|
| `0x000000`–`0x000005` | 1 B× | `MeterIC_new`         | `Pri_Comm`                                            | meter→STM32 V/I bytes (BE-packed)    |
| `0x00000c`–`0x00000f` | 1 B× | `MeterIC_new`         | (none observed)                                       | meter→? 32-bit power scaled /100     |
| `0x00015b`–`0x00017b` | 4 B× | `MeterIC_new` + `ErrorHandle` | (ErrorHandle reads too)                       | **40-byte telemetry block** (M1 prize) |
| `0x0001dd`            | 1 B  | **6 writers**         | **6 readers**                                         | "flash-write-in-progress" mutex flag |
| `0x000a07`            | 1 B  | `Pri_Comm`            | `main`, `Charging_Standard`, `Charging_Standard_RFID` | **STM32→Linux fault/pilot status**   |
| `0x000a08`            | 1 B  | `Adc`                 | 6 daemons                                             | **PilotState (CP class A/B/C/D/F)**  |
| `0x000a0b`            | 1 B  | `Pri_Comm`            | `Pri_Comm` (self)                                     | Pri_Comm's private state cache       |
| `0x000a10`            | 1 B  | `Charging_Standard{_RFID}` | self + `main`                                    | **configured ampacity (set_current)** |
| `0x000a11`            | 1 B  | `main`                | (none observed)                                       | main scratch                         |
| `0x000a17`            | 1 B  | `Charging_Standard{_RFID}` | `LED_control`                                    | LED state input                      |
| `0x000a00`–`0x000a01` | 1 B× | `Charging_Standard{_RFID}` | `LED_control`                                    | additional LED state inputs          |
| `0x000091`–`0x0000d9` | 1 B× | `main`                | (none observed)                                       | `main` private status zone           |
| `0x000100`–`0x00010d` | 1 B× | `main`                | `main`, `DeltaOCPP`                                   | **EVSE state → OCPP pipeline**       |

## 4. Multi-writer hot spots (race-risk zones)

These need careful handling — multiple daemons write the same byte:

| Offset    | Writers                                                    | Risk if we replace one writer                   |
|-----------|------------------------------------------------------------|-------------------------------------------------|
| `0x000157` | 12 daemons (all of them) — this byte is a global mutex     | Don't touch it from a replacement; just preserve     |
| `0x00015b`–`0x00017b` | `MeterIC_new` + `ErrorHandle`                  | ErrorHandle is a "if metering fails, here's zero" writer; replacement should preserve same fallback semantics |
| `0x000107` | `Charging_Standard`, `Charging_Standard_RFID`              | The two Charging variants are mutually exclusive (only one runs); fine |
| `0x0001dd` | 6 writers (any flash-touching daemon)                      | Standard "I'm using flash" lock — replacement must coordinate |

## 5. UART contention: `Pri_Comm` AND `Charging_Standard_RFID` both open `/dev/ttyAMA1`

Both binaries reference `/dev/ttyAMA1`. SPEAr3xx has exactly one ttyAMA1.
Possibilities (still untested):

1. They take turns via a custom mutex (probably via shmem byte 0x0a0b which only `Pri_Comm` touches but `Charging_Standard_RFID` might gate on).
2. One opens it but never reads (passive standby).
3. Both write; the STM32 firmware tolerates interleaved writes.

**Until this is resolved by trace, do not replace `Pri_Comm` without also
understanding `Charging_Standard_RFID`'s ttyAMA1 usage.** Next-session
shim target: trace `/root/Charging_Standard_RFID` (same shim, same
wrapper pattern, per-comm log path captures it automatically).

## 6. Meter-chip protocol completion

(Addendum to docs/13 §2.) Reread the trace with the cmd format fully
understood now:

```
  35 LL OO            = READ LL+1 bytes from register OO
  ca LL OO data×(LL+1) = WRITE LL+1 bytes to register OO
```

Init writes interpreted:
| Bytes                       | Decode                                    |
|-----------------------------|-------------------------------------------|
| `ca 00 00 05`               | reg 0x00 ← 1 byte `0x05`   (mode byte?)   |
| `ca 02 00 3b b7 1e`         | reg 0x00 ← 3 bytes `3b b7 1e`   (= 0x3bb71e BE = 3,914,526 — large cal const) |
| `ca 02 2c 00 00 00`         | reg 0x2c ← 3 bytes `00 00 00`   (clear)   |
| `ca 02 2c 00 00 08`         | reg 0x2c ← 3 bytes `00 00 08`   (set bit 3 of low byte) |
| `ca 01 02 44 80`            | reg 0x02 ← 2 bytes `44 80`      (= 0x4480 BE)   |

The cal const `0x3bb71e` doesn't obviously match Vgain/Igain/Wgain
(`342/557/3199`) by any simple formula — likely an oscillator trim or
sample-clock divisor, chip-specific. Confirms the chip is some non-Atmel
single-phase meter SoC; pending precise ID via the chip-ID query response
(`92 0e` ← may be a part-number-revision pair: 0x0e92 BE = 3730, or
0x920e LE = 37,390).

## 7. Replacement strategy — what to replace, what to keep, in what order

(Refines docs/13 §6 and the discussion under "monolith vs separate".)

**Keep stock (no replacement needed):**
- `RTC` — works; replacing risks RTC drift
- `snmpd` — upstream net-snmp, unchanged
- `wpa_supplicant`, `udhcpc`, `ppp` — standard packages

**Keep stock for now (optional later):**
- `LED_control` — replacing means owning the GPIO 55/56/57 LED state machine;
  trivial if we want it (small binary)
- `FlashLog` — periodic persistence of kWh + uptime; small, easy to replace
- `ErrorHandle` — SNMP trap emitter; only matters if we care about SNMP

**Replace now, in priority order:**
1. **`MeterIC_new`** — small protocol, single writer, low risk. Owning it
   means we own the 40-byte telemetry block at `0x015b..0x017e` and the
   meter chip's UART. Unblocks live V/I/P/E publishing via MQTT without
   guessing offsets.
2. **`Adc`** — small (24 KB), single output byte at `0x0a08`. Critical
   safety signal (J1772 state) so must implement the same classifier
   correctly. Bench-validate with a real plug.
3. **`Pri_Comm`** — known protocol (docs/01, docs/11), single writer of
   STM32→Linux status byte. **Block on understanding `Charging_Standard_RFID`'s
   ttyAMA1 use first** (§5).

**Replace selectively (large, complex):**
- `main` — supervisor + HMI + contactor + connector lock + Wi-Fi config +
  USB FWUP + factory reset + Wi-Fi/3G/VPN setup. 86 KB of stuff. Each
  feature is small in isolation but the BOM is huge. **Don't try to
  replace wholesale.** Pick specific features to take over (e.g., HA
  service `clear_faults` → our daemon writes the same shmem bytes that
  `main`'s critical-error reset does, without needing `main` itself).
- `DeltaOCPP` — 734 KB OpenSSL+JSON beast. We already have evcc / our own
  OCPP path. **Replace by simply not running it** — kill the spawn in main
  (or let it crash on startup, which seems to be the current bench state
  since DeltaOCPP wasn't in the ps list at docs/05).
- `Charging_Standard_RFID` — drives the J1772 pilot PWM. Owning this is
  the path to controlling charging current ourselves. But it also handles
  RFID auth flow → /Storage/IdTagToBeVerify → DeltaOCPP. Complicated;
  leave for last.

**Architecture decision (still recommended): hybrid.**
One `delta-bridge` binary, multi-personality via `argv[0]` or
`--personality=...`. Wrappers at `/root/<name>` dispatch each personality.
Same source tree, same MQTT/web/shmem code reused. See §"Monolith vs
separate processes" discussion in chat for full reasoning.

## 8. What's still missing (data we'd want)

1. **Read-side trace for `main`, `DeltaOCPP`, `Charging_Standard_RFID`.** We
   know what they READ in shmem statically; we don't know the cadence.
2. **240 V load test** to label the 9 u32 slots in the `0x015b..0x017e`
   block (docs/13 §6.1).
3. **Resolve the `Pri_Comm` ↔ `Charging_Standard_RFID` ttyAMA1 contention.**
   Shim either one to see whether ttyAMA1 traffic is sole-writer or shared.
4. **Static-RE pass through scanner improvements (deferred task 56).** The
   multi-step `add #imm` and fp-spill patterns would catch ~10 % more
   accesses we currently miss. Worth doing before we lean on the matrix
   as definitive.

## 9. Artifacts

- `tools/find_shmem_accesses.py` — the scanner (~200 LoC ARM symbolic exec)
- `/tmp/delta-matrix/matrix.csv` — full CSV (1,993 access rows) — not
  committed; regenerate via:
  ```bash
  for d in main Adc MeterIC_new Pri_Comm ErrorHandle Charging_Standard_RFID \
           LED_control FlashLog RTC RFID Charging_Standard DeltaOCPP; do
    tools/find_shmem_accesses.py rootfs/root/$d --csv
  done >| /tmp/matrix.csv
  ```
- `/tmp/delta-matrix/matrix-table.txt` — human-readable matrix (293
  unique offsets) — not committed; regenerate via
  `python3 /tmp/delta-matrix/build_matrix.py`.
