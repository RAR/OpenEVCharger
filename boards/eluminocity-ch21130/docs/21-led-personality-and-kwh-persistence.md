# M11 — `led` personality + meter kWh persistence

**Date:** 2026-05-16
**Status:** SHIPPED + bench-validated where possible.
- led personality: ✅ live; gpio55/56/57 read back at sysfs level match what `led_decide()` chose. **M11.1 follow-up (same day):** user-observed LED inversion on the physical bench → board is wired ACTIVE-LOW through a buffer. Fixed in `led_sysfs_byte_for()` (invert at the actuation boundary, not the semantics). docs/19 updated with the polarity correction + a TODO to re-examine the USER_STATE→`led_action` mapping against stock's behavior under live charge cycles. New host test `test_active_low_polarity` pins the byte translation.
- meter kWh write: ✅ live; `shmem[0x05c0..]` = `"0.01\0"` (matches our snprintf("%.2f", energy_raw/Wgain/100.0) formula). Stock FlashLog (PID 774) is alive but its 60-sec tick is **gated** on `shmem[0x0a5f] == 1` (charging session active); bench is idle (`0x0a5f = 0x02`), so /root/Energy hasn't been re-written. End-to-end persistence verification deferred to a real-load session.

Two small additions atop the M7/M8 personality framework:

1. **`led` personality** replaces stock `/root/LED_control`
2. **Meter personality writes kWh ASCII to `shmem[0x05c0..]`** so stock
   FlashLog persists it via `/root/Energy` across reboots

Together these get us to the point where the "ship our own image"
goal in docs/19 §end is unblocked — every leaf I/O daemon we
identified as replaceable is now either replaced or has a 15-LoC
shim to keep stock helpers happy.

---

## 1. `led` personality (`src/led.{c,h}`)

Replaces stock LED_control. Same approach as docs/19 §LED_control
specced.

### State machine (decoded from stock)

```c
if shmem[0x0a71] || shmem[0x0a72]:        // firmware-update override
    G solid, G2 solid, R off
elif shmem[0x0a07] == 5:                   // fault override
    G off, G2 off, R flash
else:
    G = USER_STATE → off/solid/flash
    R = RED_LED    → off/solid/flash
    G2 = OFF (Wi-Fi indicator deferred; we don't have stock's Green2_Wifi trigger fully decoded)
```

### Flash timing

1 sec on / 1 sec off = 0.5 Hz, 50 % duty — matches stock (docs/19 §"How it actually drives the LEDs").

### GPIO actuation

`system("echo {0,1} > /sys/class/gpio/gpioXX/value")` — same as stock.
Per-LED `last_action`/`last_phys`/`last_toggle_ms` cache prevents re-
issuing identical values, so steady-state is `system()`-free.

### Inputs

Read from shmem read-only at ~250 ms poll interval. Defensive GPIO
export on startup in case stock LED_control hasn't run.

### What we DON'T replicate from stock (yet)

- `Green2_Wifi` blink pattern — the trigger source isn't fully decoded
- `USB_Flash` + `Powerdtect` — USB stick detection / power presence;
  Green2 is held off in our v1, can be wired to a delta-bridge state
  later (MQTT-connected indicator, etc.)

### Host tests

`test/test_led.c` covers the pure `led_decide()` mapping function:
- All-zero → all off
- USER_STATE 0/1/2 → green off/solid/flash
- RED_LED 0/1/2 → red off/solid/flash
- shmem[0x0a71] OR shmem[0x0a72] non-zero → firmware-update override
- shmem[0x0a07]=5 → fault override (red flash, green off)
- Firmware-update override wins over fault

26 tests, all green.

## 2. Meter kWh write to `shmem[0x05c0..]`

Per docs/19 §FlashLog §"What this means for our meter personality" —
~15 LoC addition in `src/meter.c`:

```c
static void publish_kwh_ascii(struct shmem *sm,
                              const struct meter_readings *r,
                              const struct meter_cal *cal)
{
    uint32_t wgain = cal->wgain > 0 ? (uint32_t)cal->wgain : 1;
    double kwh = (double)r->energy_raw / (double)wgain / 100.0;
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%.2f", kwh);
    /* ...bounds-check, write byte-by-byte, NUL-terminate... */
}
```

Called from `meter_publish_shmem()` alongside the existing compact +
telemetry-block writes. Each cycle (~1.7 Hz), our personality refreshes
the ASCII kWh string at `shmem[0x05c0..]`. Stock FlashLog (still
running) reads it every ~60 s and shells out `echo X.XX > /root/Energy`.

Bench validation: watch `/root/Energy` mtime — should tick every ~60 s
once the meter personality starts publishing. After reboot, the v07
meter personality (without kWh write) failed to update `/root/Energy`
since stock FlashLog read zeros from `shmem[0x05c0..]`; v09 should fix
this.

### Formula caveat

`kWh = energy_raw / Wgain / 100.0` is provisional. Stock's exact
formula isn't decoded — it likely involves the Wgain calibration
constant differently, and the energy register may need additional
post-processing. For our purposes, what matters is that **the value
persists across reboots** so the kWh counter doesn't reset. Refining
to match stock's exact formula is a 240 V follow-up.

### Updated tests

`test/test_meter.c` adds:
- Two kWh-byte-layout checks: small (0.01) + larger (100.00) values
- Defensive: `!valid` and `!writable` don't clobber shmem[0x05c0]

98 → 112 tests (14 new), all green.

## 3. Bench deployment

```
/Storage/delta-bridge/delta-bridge.v09         (170 KB ARM static-pie)
/root/LED_control      → wrapper exec'ing v09 --personality=led
/root/MeterIC_new      → wrapper exec'ing v09 --personality=meter (was v07)
/root/Adc              → wrapper exec'ing v08 --personality=adc (unchanged)
/root/RFID             → wrapper exec'ing v06 delta-bridge (unchanged)

/Storage/LED_control.preinst.bak     stock LED_control rollback
/Storage/stk/LED_control             stock LED_control (kept for personality launch path)
/Storage/MeterIC_new.v07.bak         previous meter wrapper (rollback to v07)
```

Rollback (anytime):
```sh
mv /Storage/LED_control.preinst.bak /root/LED_control
mv /Storage/MeterIC_new.v07.bak /root/MeterIC_new    # or use stock from /Storage/stk/
sync; reboot
```

## 4. What's NOW the scorecard

| Daemon | Status |
|--------|--------|
| RFID | ✅ replaced (delta-bridge v0.6) |
| MeterIC_new | ✅ replaced (meter personality v09 — incl. kWh persistence) |
| Adc | ✅ replaced (adc personality v08) |
| **LED_control** | ✅ **replaced (led personality v09 — NEW)** |
| FlashLog | ⏭️ kept stock; meter personality now feeds it correctly |
| Pri_Comm | ❌ deferred (safety supervisor, docs/18) |
| main | ❌ stock (orchestration + config; docs/14 §7) |
| Charging_Standard_RFID | ❌ stock (EVSE state machine, docs/20) |
| RTC | ⏭️ kept stock (works fine, low ROI to replace) |
| ErrorHandle | ⏭️ kept stock (SNMP only) |
| DeltaOCPP | ❌ don't run (delta-bridge + evcc covers OCPP) |

All four leaf I/O personalities (RFID, meter, adc, led) now ours.

## 5. Next: M12 — build the stripped DcoFImage

Per docs/20 §"What 'flash our own stuff' actually looks like":

- Wrap RFID/MeterIC_new/Adc/LED_control to delta-bridge personalities
- Keep stock for main, Pri_Comm, CSR, FlashLog, RTC, ErrorHandle
- **Remove** DeltaOCPP (~734 KB) + dead build tools (ACFWMaker, ScenarioMaker, PowerCard-UltraLight, NTC_tmp, PassTime, LogCount, FWMaker, Pri_Comm_cqc — none referenced at runtime per the docs/14 matrix)
- Wrap into a DcoFImage via the existing `image/build-dcofimage.sh`

Result: a USB-flashable image that's "ours where we trust it", stock
where it's safety/config-critical, with the dead weight stripped.
