# MeterIC_new + Adc — LD_PRELOAD syscall trace + static-RE shmem write map

**Date:** 2026-05-16
**Method:** Same `uart_trace.so` LD_PRELOAD shim from `docs/10`/`docs/11`,
extended to auto-derive log path from `/proc/self/comm`. Both daemons
wrapped simultaneously via `/Storage/stk/<name>` + thin `/root/<name>` shell
wrappers; reboot starts both under the shim in their normal supervised slot.

**Raw evidence:**
- `companion/test/data/meteric-trace-stock-2026-05-16.log` — 285 KB, 3065 paired
  WRITE→READ cycles plus init (~5 min capture).
- `companion/test/data/adc-trace-stock-2026-05-16.log.gz` — 1.6 MB → 193 KB
  gz, 41,737 single-byte reads (~5 min capture, ~128 Hz sample rate).
  `gunzip -k` to read.

**Companion artefact:** `companion/tools/find_shmem_accesses.py` —  tiny ARM
symbolic-executor that scans an ELF for `*(g_shm_base) + off` store patterns
and dumps the full write map. Used inline below.

This doc together with `docs/01 §"Shmem layout"`/`docs/05 §"Bytes that DO
move"` advances **Phase 2a** (shmem offset rewrite) without needing 240 V
mains — most of what we cared about lives in known-source files now, even if
the *values* still need ground-truth on a live load.

---

## 1. What runs and how

`/etc/funs` (init) `&`-spawns `/root/main`, which in turn forks `/root/Adc`,
`/root/MeterIC_new`, `/root/Pri_Comm`, etc. `MeterIC_new` polls the meter chip
over `/dev/ttyAMA2` at 2400 8N1; `Adc` polls `/dev/adc0` at ~128 Hz; both
write into the SysV shmem segment at key `0x153E`, 256 KiB, attached by every
charging daemon.

Bench was on 120 V, latched UVP fault, no plug — so the trace captures
**steady-state idle**, no real-world load values. Layout discovery: full.
Value calibration: ground-truth pending 240 V.

## 2. MeterIC_new — meter-chip UART protocol

### 2.1 Init phase syscalls (one-shot, in order)

```
[21.072] open("/Storage/SerialNumber", O_RDONLY)  → "JCF164800030WE\n"
[21.078] open("/Storage/Gain",       O_RDONLY)    → "Vgain:342\nIgain:557\nWgain:3199\n\n"
[21.101] open("/root/Energy",        O_RDONLY)    → "100.06\n"
[21.125] open("/dev/ttyAMA2",        O_RDWR|O_NOCTTY)
[21.126] ioctl(fd, TCSETS, &t)
         t = c_iflag=0  c_oflag=0  c_cflag=0x08fc  c_lflag=0  VTIME=10  VMIN=0
         CBAUD → 0x0c = B2400, CS8|CREAD|CLOCAL set.
[21.128] write fd << 35 01 0e  →  read fd >> 92 0e         (chip ID query)
[21.154] write fd << 35 02 26  →  read fd >> cb 59 26      (cal-reg dump)
[21.212] write fd << ca 00 00 05                            (chip "go" cmd)
[21.272] write fd << ca 02 00 3b b7 1e                      (write 4-B cal @ reg 0x02)
[21.503] write fd << 35 02 2e                               (then 3-B read — unknown reg)
[21.692] write fd << ca 02 2c 00 00 00                      (clear reg 0x2c)
[21.782] write fd << ca 02 2c 00 00 08                      (set bit 3 of reg 0x2c)
[21.940] write fd << ca 01 02 44 80                         (write 2-B cal @ reg 0x02)
[22.002] — steady-state polling begins —
```

**Inferences:**
- The meter chip is interrogated with two opcodes: `35 LL OO` (read LL+1 bytes
  from register `OO`), and `ca LL OO ...` (write LL+1 bytes to register `OO`).
- All UART traffic is binary, no framing or checksum visible. The chip is
  almost certainly an SD3004 / V9203 / RN8302-class single-phase meter SoC.
- `JCF164800030WE` = meter-chip serial; persisted under `/Storage/SerialNumber`.
- `Vgain/Igain/Wgain = 342/557/3199` = per-unit calibration, persisted under
  `/Storage/Gain`. **These values are written ONCE in factory cal** — the daemon
  reads them at boot; never re-writes during steady-state.
- `/root/Energy = 100.06` = persisted kWh accumulator. Daemon reads at boot,
  updates at run (writes not yet observed in trace — likely on shutdown via the
  `system("rm -f /root/Energy; echo VAL > /root/Energy")` pair seen in strings).

### 2.2 Steady-state poll cycle

A 4-register cycle, repeated every ~600 ms (≈ 1.7 Hz):

| Cmd        | Resp size | Decode (LE)            | Range observed @ 120 V, no load |
|------------|----------:|------------------------|---------------------------------|
| `35 02 27` | 3 B       | u24 Vrms (raw counts)  | 2,449,045 .. 2,513,355 (0.7%)  |
| `35 02 1c` | 3 B       | u24 Irms (raw counts)  | 3,645,246 .. 3,670,010 (0.7%)  |
| `35 03 1a` | 4 B       | u32 P/S* (raw)         |     8,753 ..     14,944 (50%)  |
| `35 03 10` | 4 B       | u32 E/Q* (raw)         |     1,910 ..      3,280 (50%)  |

`*` Active vs apparent vs reactive — can't disambiguate without a real load.
The first two cluster tight (Vrms is real ~120V via some scale; Irms is 0A
sensor offset). The last two show 50 % range because they are P/Q-like terms
that swing on noise when current ≈ 0.

**Provisional Vrms calibration:** `Vrms_volts ≈ raw / Vgain / 60` gives ~119.9
for `raw = 2,460,000`, `Vgain = 342`. Confirm on 240 V split-phase.

### 2.3 Cycle period & UART rate

- Per-cycle: 4× `write(3B)` + 4× `read(3-4B)` + per-syscall RTT.
- 600 ms cycle, 2400 baud → ~28 bytes / cycle ≈ 117 ms wire time, balance is
  syscall + kernel-side wait for response.

## 3. Adc — `/dev/adc0` CP-sense byte stream

### 3.1 Init phase

```
[20.978] open("/dev/adc0", O_RDWR)                   → fd 3
[20.982] ioctl(fd, 0x40104102, struct of 16 B at SP)  → 0  (channel config)
[20.983] ioctl(fd, 0x40104102, struct of 16 B at SP)  → 0  (channel config #2)
[21.001] — steady-state polling begins —
```

`0x40104102` = `_IOW('A', 2, 16-byte struct)` — driver-private "set up channel"
ioctl. Called twice with two different arg pointers, likely configuring two
ADC channels (CP-sense × 2 redundant taps, per Delta's pattern of
double-instrumenting safety inputs).

### 3.2 Steady-state read pattern

41,737 `read(fd, buf, 1)` calls — **one byte per syscall**, at ~128 Hz. Of
those, 1.9 % returned `-EAGAIN` (no sample ready), the rest returned exactly
1 byte.

ADC value histogram (bench idle, CP at −11.9 V):

```
  0x5c (92)   262   ********
  0x5e (94) 13842   ************************************************************
  0x61 (97) 18690   ************************************************************
  0x63 (99)  6931   ************************************************************
  0x65 (101)  230   *******
  0x68 (104)    1
  0xe8 (232)    3
  0xeb (235)  829   ***************************
  0xed (237)  153   *****
  0xf0 (240)   15
```

Bimodal — **97% LOW cluster** (94..99, centered at ~97) plus a **2.4% HIGH
cluster** (235..240, centered at ~236). This matches the expected CP-sense
behavior: when the pilot generator is OFF (UVP fault), CP rests near one
rail; brief transitions/noise create the small HIGH cluster.

On 240 V with the pilot generator active, expect a clean bimodal split at
the ±12 V rails (with a 50% duty cycle when no plug, then a state-dependent
duty in B/C). Bench in UVP can't yet exercise this.

### 3.3 Internal classifier

Static analysis of the Adc binary identifies these globals (debug-info
unstripped, see `tools/find_shmem_accesses.py 12dc8`):

```
State           PV_State       PV_State_Prev   PV_P_Cnt    PV_N_Cnt
volt_negtive    Alarm_Flag     Buf_error
Buf_9V          Buf_6V         Buf_12V         Buf_N_12V   SysTime
MeterSMPtr      PilotState
```

The classifier accumulates samples into voltage-bucket buffers (`Buf_6V`,
`Buf_9V`, `Buf_12V`, `Buf_N_12V`) and a positive/negative counter pair
(`PV_P_Cnt` / `PV_N_Cnt`), then maps the bucket distribution to a J1772
pilot state (A=12V, B=9V, C=6V, F=−12V, etc.).

The single output to shared memory is **`shmem[0x0a08] = PilotState`** — one
byte, the EVSE pilot-state classification.

## 4. Shmem write maps — what each daemon owns

Static scan via `tools/find_shmem_accesses.py`:

### 4.1 Adc → shmem

| Offset    | Size | Source function   | Meaning                                       |
|-----------|-----:|-------------------|-----------------------------------------------|
| `0x0157`  | 1 B  | `main` (5 sites)  | shared status byte (also written by `main`'s peers — Pri_Comm scratchpad?) |
| `0x0a08`  | 1 B  | `PilotState`      | **J1772 pilot state: A/B/C/D/E/F bucket**     |

That's *it*. The whole Adc daemon writes 2 bytes into shmem. Everything else
is private state.

### 4.2 MeterIC_new → shmem

| Offset block     | Sizes  | Source                | Provisional meaning                                                 |
|------------------|-------:|-----------------------|---------------------------------------------------------------------|
| `0x0000`/`0x01`  | 2× 1 B | `main` 0x9f50‑0x9f70  | BE-packed u16 — meter reading "channel 1" raw (Vrms low units)      |
| `0x0004`/`0x05`  | 2× 1 B | `main` 0xa050‑0xa078  | BE-packed u16 — channel 1 / 10 (Vrms in deci-volts)                |
| `0x000c‑0x000f`  | 4× 1 B | `main` 0xa150‑0xa1c0  | BE-packed u32 — channel reading / 100 (P in centi-watts or kWh×100) |
| `0x0157`         | 1+4 B  | `main` 0x93ec / 0x93f8| flag byte AND start of 40-B telemetry table                         |
| `0x015b‑0x017b`  | 9× 4 B | `main` 0x9404‑0x9464  | **40-byte meter telemetry table** (9 u32 readings)                  |
| `0x01c7‑0x01ca`  | 4× 1 B | `main` 0x977c‑0x97e8  | 4 status counters / probe states                                    |
| `0x01d5`         | 1 B    | `main` 0xb248          | latched flag                                                        |
| `0x01dd`         | 1 B    | `StoreFlash` + `main`  | "writing flash" / "flash error" flag                                |
| `0x0a62`, `0x0a68..0x0a6f`, `0x0a78`, `0x0ab0` | 1 B each | `main` (multiple) | EVSE-area scratch (alongside `0x0a08` PilotState) — needs trace replay |
| `0x002001`       | 1 B    | `main` (debug?)        | far-region single byte — likely a debug counter, low confidence     |

The **40-byte telemetry table at `0x0157..0x017e`** is the M1 prize: it's
where Vrms/Irms/P/E/S/Q/PF/freq/etc. land in consumer-friendly format. Now
known *by location*; needs **one** live capture to label each u32 slot
(easiest: shmem-snapshot diff alongside known polled values from a
shimmed Pri_Comm + MeterIC_new on a live load).

### 4.3 Cross-reference to inherited (wrong) `shmem_offsets.h`

| Symbol in v0.5            | Old offset | Verdict                                          | New best guess                                |
|---------------------------|-----------:|--------------------------------------------------|-----------------------------------------------|
| `OFF_VRMS`                | `0x0a10`   | Wrong (= pilot duty)                             | Likely **`0x0157+k`** in the 40-B table       |
| `OFF_IRMS`                | `0x0a24`   | Wrong (= configured ampacity)                    | Likely **`0x0157+k`** in the 40-B table       |
| `OFF_POWER`               | `0x0a18`?  | Wrong (was guess)                                | Likely **`0x0157+k`** in the 40-B table       |
| `OFF_ENERGY`              | `0x0a20`?  | Wrong (was guess)                                | Likely **`0x0157+k`** in the 40-B table (or 0x000c-0x000f scratch)   |
| `OFF_CONNECTOR_STATE`     | `0x0a00`   | Wrong (always 0)                                 | **`0x0a08`** (`Adc.PilotState`)               |
| `OFF_FAULT_FLAGS`         | `0x0a07`   | Plausible — pulses to 0x02 during retries        | Keep; corroborate vs `0x01dd` from MeterIC_new|
| `OFF_HEARTBEAT`           | `0x0a08`   | **Wrong** — that's PilotState, not heartbeat     | Look elsewhere (Pri_Comm region 0x0a0a..0x0a0f); needs trace        |
| `OFF_STM32_LINK`          | `0x0a0b`   | Wrong (static 0)                                 | Probably wholly unowned; needs trace          |

## 5. What it took to get the trace

- **Per-process log path**: Extended `tools/uart_trace.c` to read
  `/proc/self/comm` and write to `/Storage/trace/<comm>.log`. Two daemons
  trace in parallel without colliding (one shim binary, two log files).
- **Wrapper-deploy pattern** (same as docs/12): stock binaries copied to
  `/Storage/stk/{Adc,MeterIC_new}`, replaced with thin `#!/bin/ash` wrappers
  exec'ing `LD_PRELOAD=/Storage/uart_trace.so /Storage/stk/<name> "$@"`.
- **Stop sequence**: `mv` stocks back, `kill` wrapped PIDs. `/root/main` does
  **not** auto-respawn the children — manual `/root/Adc &; /root/MeterIC_new &`
  from the serial shell was needed. (Future: prefer reboot for cleaner
  recovery, or write a respawn loop.)
- **/Storage budget**: ~14 KB/s combined trace growth ⇒ 11 MB free ⇒ ~13 min
  before disk full. 5 min was enough; snapshot via `tftp -p` back to dev.

## 6. Open questions / next traces

1. **Label the 9× u32 slots at `0x0157..0x017e`**. Easiest: 240V on, plug in,
   correlate live values with the shimmed `/root/Pri_Comm` `35 02 27` (Vrms),
   `35 02 1c` (Irms), `35 03 1a`, `35 03 10` readings.
2. **Identify register 0x2e** (one-shot init read of 3 bytes). Probably a
   chip-status / sample-clock divisor.
3. **Decode `ca 02 00 3b b7 1e`** — the calibration block written at init.
   Looks like four parameters; might encode V/I gain in a different unit
   from `/Storage/Gain`.
4. **Pri_Comm corollary**: trace `/root/Pri_Comm` AGAIN with the new per-
   comm log path enabled — confirms whether `0x0a08` (PilotState) is read by
   Pri_Comm before being shipped to the STM32, or if Pri_Comm writes its own
   PilotState elsewhere.
5. **`main` daemon (PID 618)** is the biggest unmapped surface — `/root/main`
   ties everything together. Next worthwhile shim target. Will reveal whom
   reads what from shmem, completing the producer→consumer graph.

## 7. Bench state at end of session

- `/root/{Adc, MeterIC_new}` = stock binaries restored, md5s match factory.
  Wrappers preserved at `/Storage/{Adc,MeterIC_new}.wrap.bak` for redeploy.
- `/Storage/stk/{Adc,MeterIC_new}` = stocks (always present, source of truth).
- `/Storage/uart_trace.so` = v3 shim with per-comm log path
  (`md5 b42eecae4ba701f7e312eb732f52d5b0`).
- `/Storage/uart_trace.so.prev` = v2 (the docs/12 shim, kept as rollback).
- `/Storage/trace/{Adc,MeterIC_new}.{log,snap,snap2}` = ~3 MB of captures.
  Snapshotted to `companion/test/data/{adc,meteric}-trace-stock-2026-05-16.log`.
- `delta-bridge`, RFID, MQTT all unaffected.
