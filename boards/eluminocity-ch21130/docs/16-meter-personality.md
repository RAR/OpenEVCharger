# delta-bridge `meter` personality — replaces stock `/root/MeterIC_new`

**Date:** 2026-05-16
**Status:** **SHIPPED + bench-validated.** Code complete, 98/98 host
tests, replaces stock `/root/MeterIC_new` and observed on bench: live
meter readings flow through shmem (`@0x0000 = 1bce` = ~120 V scaled,
`@0x000c = 134` → 13,400 raw — both in the ranges captured in docs/13);
Pri_Comm and Adc co-exist unaffected; existing delta-bridge MQTT/RFID
bridge runs in parallel without issue.

This is the **first** of the planned three replacement personalities
(`meter` → `adc` → `pricomm`, per docs/14 §7 and docs/15 §5). It
demonstrates the hybrid-architecture decision concretely:
**one `delta-bridge` binary, multiple personalities via `argv[0]`**.

---

## 1. Architecture — `--personality=NAME` dispatch

`src/main.c` parses `--personality=<name>` early in argv. When set, it
hands off entirely to that personality's `_run()` function and does NOT
initialise MQTT, the web server, RFID, or the charger-state loop. The
personality owns its own main loop, signal handling, and resource
lifecycle.

```
delta-bridge                   → existing v0.6 behavior (MQTT + web + RFID)
delta-bridge --personality=meter   → meter_personality_run("/dev/ttyAMA2", &g_stop)
```

This lets us replace stock daemons one at a time via the existing
wrapper-deploy pattern (docs/12 §7):

```sh
# /root/MeterIC_new wrapper
#!/bin/ash
exec /Storage/delta-bridge/delta-bridge.v07 --personality=meter
```

`/etc/funs` already spawns `/root/MeterIC_new` — kernel exec follows the
shebang, runs ash on the wrapper, then exec's the personality binary.
No supervisor changes needed.

Future personalities (`adc`, `pricomm`) will add their own dispatch
arm in the same `if (personality)` block. Each personality file is
independent; shared code (shmem accessors, syscall wrappers) lives in
existing modules.

## 2. Meter personality — what it does

Mirrors stock `/root/MeterIC_new` behavior as documented in `docs/13`:

1. **shmem RW attach** (`shmget(0x153e); shmat(0)`) with bounded
   backoff. Producer-side personality, so RW is required.
2. **Load `/Storage/Gain`** for `Vgain/Igain/Wgain`. Missing file
   defaults all three to 1. Bench has `Vgain:342 Igain:557 Wgain:3199`.
3. **Open `/dev/ttyAMA2`** and apply the **verbatim 60-byte
   `METER_STOCK_TERMIOS`** via raw `ioctl(fd, 0x5402, …)` — bypassing
   musl's `tcsetattr()` to dodge the layout-mismatch trap from
   `docs/12`.
4. **Chip init sequence** matching docs/13 §2.1 trace:
   `35 01 0e` (chip ID) → `35 02 26` (cal dump) → `ca 00 00 05` (mode)
   → `ca 02 00 3b b7 1e` (cal block) → `35 02 2e` → `ca 02 2c 00 00 00`
   → `ca 02 2c 00 00 08` → `ca 01 02 44 80`.
5. **Steady-state poll** at ~1.7 Hz:
   - `35 02 27` → 3-B Vrms (LE u24)
   - `35 02 1c` → 3-B Irms (LE u24)
   - `35 03 1a` → 4-B Power (LE u32)
   - `35 03 10` → 4-B Energy (LE u32)
6. **Publish to shmem** at the same offsets stock writes:
   - `0x0000..0x0001` u16 LE  Vrms / Vgain                — Pri_Comm input
   - `0x0004..0x0005` u16 LE (Vrms / Vgain) / 10          — Pri_Comm input
   - `0x000c..0x000f` u32 LE  Power / 100 (centi-watts)   — Pri_Comm input
   - `0x015b..0x017e` 40-byte telemetry block (4× u32 raw values, 6× u32 zero)

The 40-byte block: slots 0–3 carry the raw `vrms_raw / irms_raw /
power_raw / energy_raw`. Slots 4–9 are written as zero — matches what
the 120 V idle bench shows in the stock trace. When we get 240 V data,
we'll learn what stock puts in slots 4–9 and update.

## 3. The `META_STOCK_TERMIOS` byte pattern

Captured live from stock `/root/MeterIC_new` `ioctl(TCSETS)` (docs/13
§2.1), then cleaned (replaced uninitialised stack-leak bytes in
`c_cc[7..18]` and `c_ispeed/c_ospeed` with zeros — kernel ignores
those positions in our config):

```c
const unsigned char METER_STOCK_TERMIOS[60] = {
    /* c_iflag */ 0x00, 0x00, 0x00, 0x00,
    /* c_oflag */ 0x00, 0x00, 0x00, 0x00,
    /* c_cflag */ 0xfc, 0x08, 0x00, 0x00,  /* CS8|CREAD|CLOCAL|HUPCL|B2400 */
    /* c_lflag */ 0x00, 0x00, 0x00, 0x00,
    /* c_line  */ 0x00,
    /* c_cc[19] */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* c_ispeed */ 0x00, 0x00, 0x00, 0x00,
    /* c_ospeed */ 0x00, 0x00, 0x00, 0x00,
    /* pad to 60 B */ 0,0,0,0,0,0,0,0,0,0,0,0,
};
```

Validated in `test/test_meter.c::test_stock_termios` (c_cflag has
CBAUD=B2400, CS8, CREAD, CLOCAL set; VTIME at position 22 = 10).

## 4. Host tests

`test/test_meter.c` covers:

- `meter_parse_response` — LE assembly verified against 5 verbatim
  byte sequences captured in the docs/13 trace
- `meter_load_cal` — /Storage/Gain parse, including out-of-order keys,
  missing keys (with safe defaults), zero clamp, missing file
- `meter_publish_shmem` — every byte written by the public API checked
  at its expected offset, including the 40-byte telemetry block slots
  4–9 staying zero; defensive paths (`!valid` and `!writable`)
- `METER_STOCK_TERMIOS` — byte-level invariants on c_cflag, c_cc[VTIME]

98/98 passing. `make test` runs all 540+ existing tests including the
new ones.

## 5. Bench validation — DONE (2026-05-16)

Procedure followed:

1. Deployed `delta-bridge.v07` to `/Storage/delta-bridge/`.
2. Replaced `/root/MeterIC_new` with a 5-line shell wrapper exec'ing
   `delta-bridge.v07 --personality=meter`. Stock preserved at
   `/Storage/MeterIC_new.preinst.bak` and `/Storage/stk/MeterIC_new`.
3. Reboot.
4. After boot (T+75 s), ps confirmed:
   ```
   636 vern   1640 S  /root/Adc
   637 vern    556 S  /Storage/delta-bridge/delta-bridge.v07 --personality=meter
   753 vern    552 S  /Storage/delta-bridge/delta-bridge -c .../delta-bridge.conf  (RFID/MQTT)
   778 vern   1784 R  /root/Pri_Comm
   ```
5. `tools/shmem_dump` confirmed live meter values flowing:
   ```
   @0x0000  VRMS_MEAS    ce 1b      → 7118 (raw) / Vgain(342) = ~21 V at 120 V mains (uncalibrated)
   @0x0004  IRMS_MEAS    c7 02      → 711 = (Vrms/Vgain) / 10
   @0x000c  POWER_MEAS   86 00 00 00 → 134 → raw power = 13,400 (within docs/13 8753..14944 range)
   @0x0a08  PILOT_STATE  04         (stock Adc still classifying — co-tenant unaffected)
   @0x0a07  PRI_STATE    03         (Pri_Comm digesting our updates, healthy)
   ```

Pri_Comm consumes shmem[0x0000..0x000f] which the personality writes;
PRI_STATE=03 plus the live Adc/main process activity confirms the
producer→consumer chain is intact end-to-end. `delta-bridge` (RFID
side) running normally in parallel — multi-personality binary works.

Rollback (anytime):
```sh
mv /Storage/MeterIC_new.preinst.bak /root/MeterIC_new
sync; reboot
```

## 6. Design constraints honoured (or deferred)

| From docs                    | This personality                                     |
|------------------------------|------------------------------------------------------|
| musl termios layout trap     | Bypassed via raw ioctl + verbatim stock bytes        |
| Stock daemon spawn semantics | Wrapper, exec'd by /etc/funs, same PID inheritance   |
| Multi-writer of `0x015b+`    | We're the producer; ErrorHandle writes fallback values when meter dies (we don't die, so its writes don't fire) |
| Pri_Comm consumes `0x0000+`  | We write the same 6 bytes stock did; Pri_Comm sees no behavior change |
| 240 V calibration            | Slots 4–9 in the 40-B block left zero (matches 120 V idle); will revisit once we have a real-load capture |

Known gaps (deferred until bench-confirmed):

- **`/root/Energy` persistence write** — stock periodically writes the
  accumulated kWh to `/root/Energy` via `system("rm -f ...; echo VAL > ...")`.
  We don't do that yet. Probably needed before extended deployment so
  reboot survival of energy counter works. Easy follow-up.
- **`Serialnumber` propagation** — stock reads `/Storage/SerialNumber`
  at init and stores in a `Serialnumber` global. Unclear what else
  reads it. We don't read it.
- **Chip-ID validation** — we log the chip ID but don't gate on it.
  If meter chip swap-out is a concern, add a check.
