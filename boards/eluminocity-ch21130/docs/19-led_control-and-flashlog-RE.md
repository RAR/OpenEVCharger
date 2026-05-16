# LED_control + FlashLog — full static RE

**Date:** 2026-05-16
**Status:** RE complete; replacement candidates clearly scoped.

Two small charging-stack daemons we never fully decoded. Both are
**straightforward** by comparison to Pri_Comm (docs/18) and both are
worth replacing if the "flash our own stuff" goal is going to land
without leaving stock binaries we don't fully understand.

---

## Part 1: LED_control (13 KB)

### Architecture

11 named functions + main, debug-info present:

```
DiffTimeb           Green_IOCtrl         Green_Flash
Red_IOCtrl          Red_Flash            Green2_IOCtrl
Green2_Flash        Green2_Wifi          USB_Flash
Powerdtect          main
```

### How it actually drives the LEDs

Every LED operation goes through `system("echo {0|1} > /sys/class/gpio/gpioXX/value")` — no `/dev/mem`, no kernel driver, no shmem write. The IOCtrl helpers just `sprintf` the command string and shell out.

```c
void Green_IOCtrl(uint8_t on) {
    sprintf(buf, "echo %c > /sys/class/gpio/gpio55/value", on ? '1' : '0');
    system(buf);
}
```

GPIO mapping (from per-function disassembly):

| Function          | GPIO | Color/role               |
|-------------------|------|--------------------------|
| `Green_IOCtrl`    | 55   | Green LED (main)         |
| `Red_IOCtrl`      | 56   | Red LED                  |
| `Green2_IOCtrl`   | 57   | Green LED #2 (Wi-Fi/sec) |
| `Powerdtect`      | 82   | INPUT — read state, used as USB/aux-power detect |

`*_Flash` variants toggle the corresponding output once per second (1 sec on / 1 sec off = **0.5 Hz**, 50% duty), gated by a per-LED `last_toggle_time` timer + `current_state` cache (so multiple calls in the same second don't re-fire system()).

### State machine (the main loop)

Decoded from `main()` at 0x8b74:

```c
loop:
    ftime() update timers
    if shmem[0x0a72] || shmem[0x0a71]:    # override flags set
        goto firmware_update_path
    if shmem[0x0a07] == 5:                # PRI_STATE = fault
        goto fault_pattern
    
    switch (shmem[0x0a00]):               # USER_STATE
        case 0: Green_IOCtrl(0)           # off
        case 1: Green_IOCtrl(1)           # solid
        case 2: Green_Flash()             # blinking
    
    switch (shmem[0x0a01]):               # RED_LED
        case 0: Red_IOCtrl(0)
        case 1: Red_IOCtrl(1)
        case 2: Red_Flash()
    
    every 5 sec:
        if shmem[0x0a71] in (0, 0xff):    # "firmware update mode" 
            Green2_IOCtrl(1) + Green_IOCtrl(1)
            sleep 2
            wait for shmem[0x0a71] != 0
        else:
            USB_Flash()
    loop_back
```

**Override paths (the `goto`s above):** firmware-update or fault state.
When firmware update is in progress, both green LEDs go solid and the
state machine is paused. Fault paths use a similar approach (alternate
blink pattern).

### What we DON'T know

- **Green2_Wifi** function — exists, calls Green2_IOCtrl, hooked into the
  main loop somewhere (probably tied to a wlan0 status check). Not
  fully traced because we don't care about the Wi-Fi indicator for our
  use case.
- **Exact `Powerdtect` debounce logic** — reads gpio82 to detect USB/aux
  power presence; specific debounce thresholds unmeasured.

### Replaceability

**Trivial.** ~200-300 LoC `led` personality:

- Open the four GPIOs at startup (export + direction once via system())
- Poll shmem state every ~250 ms
- Run the state machine above; drive LEDs via sysfs writes

**Bonus value if we replace it:** we can drive LEDs from our own state
(MQTT-connected indicator, RFID scan ack flash, delta-bridge updating,
etc.) — stock has no concept of any of that. Whole new UX surface for
~half a session of work.

### GPIO contention concern

docs/14 §2 footnote: `Charging_Standard_RFID` (CSR) also writes
gpio55/56/57. From our static RE of LED_control, that's an additive
race — both daemons issue `echo X > .../gpioXX/value`, whichever
runs last wins. Most likely CSR uses them for transient signals (like
"RFID swipe acknowledged") that don't fight the LED_control steady
state. If we replace LED_control we should probably also have CSR
stop writing GPIOs — or coordinate via shmem.

---

## Part 2: FlashLog (11 KB)

### Architecture

Two functions + main:

```
StoreFlash    main
```

### What FlashLog actually does

**Persistence orchestrator for three things:**

1. **`/root/Energy`** — ASCII kWh accumulator (`echo "%s" > /root/Energy`)
2. **`/root/PassTime`** — uptime/session counter (`echo "%d" > /root/PassTime`)
3. **`/dev/mtdblock4`** — full 64 KiB shmem snapshot, written via StoreFlash()

### How the persistence works

**`/root/Energy`** — every ~60 sec while shmem[0x0a5f] == 1 (session
active flag), FlashLog reads ASCII bytes from **`shmem[0x5c0..0x5df]`**
(32-byte buffer) and runs `system("echo X.XX > /root/Energy")`.

**`/root/PassTime`** — same 60-sec cadence; reads u32 BE from
**`shmem[0x4e0..0x4e3]`** and runs `system("echo N > /root/PassTime")`.

**`/dev/mtdblock4`** — `StoreFlash(int mode, int len)` writes a 128 KiB
region with two redundant copies of the first 64 KiB of shmem:

```
[0x00000..0x0fff8] = shmem[0..0xfff8]
[0x0fffc..0x0ffff] = checksum (BE u32, sum of preceding bytes)
[0x10000..0x1ffff] = identical copy (redundancy)
```

Modes:
- `mode == -1`: full snapshot — memcpy 64 KiB of shmem into the buffer
- `mode == -2`: partial overlay — read existing mtd, then copy specific
  shmem fields (alarm bitmap at 0xa79, 32-byte string at 0x5e0, byte at
  0xa5f) over the existing snapshot
- `mode >= 0`: write `len` bytes from `shmem[mode]` over the same
  offset in the existing snapshot

Called on state transitions (charging-session start/end inferred from
shmem[0xa07] / shmem[0xa08] checks); NOT every 60 sec.

The write-side acquires a mutex at `shmem[0x01dd]` (the "flash-write-
in-progress" byte from docs/14 §4) with up to 5 sec wait, and ORs a
status bit into shmem[0x157+3].

### What boots from this

`/root/main` has a `FlashToShrMem` function (docs/14 §2). On boot it
reads `/dev/mtdblock4`, verifies the checksum, and copies the data
back into shmem. So the persistence chain is:

```
runtime:    [our daemons] -> shmem -> FlashLog (60s + on events) -> /dev/mtdblock4
                                                                  +-> /root/{Energy,PassTime}
boot:       /dev/mtdblock4 -> main:FlashToShrMem -> shmem -> [daemons start reading]
            /root/Energy   -> MeterIC_new (reads at boot) -> initial kWh counter
            /root/PassTime -> ??? (probably Charging_Standard_RFID — see matrix)
```

### What this means for our meter personality

**docs/16 §6's "deferred /root/Energy persistence" gap is BIGGER than
we thought** — but FIXABLE with a small change:

Our meter personality already correctly reads `/root/Energy` at boot
(initial kWh value, "100.06\n" in docs/13 trace). It doesn't WRITE
the running counter back. Stock's persistence chain expects the running
counter to be at **`shmem[0x5c0..]`** as ASCII — which our meter
personality doesn't populate.

**The fix is small** (~15 LoC in `src/meter.c`):

```c
/* In meter_publish_shmem, additionally: */
char buf[32];
double kwh = compute_kwh_from(r);    /* convert raw → float kWh */
int n = snprintf(buf, sizeof buf, "%.2f", kwh);
/* Write ASCII into shmem[0x5c0..0x5c0+n] + null. FlashLog
 * (stock, still running) picks this up every 60s and pushes to
 * /root/Energy via system(). */
for (int i = 0; i < n + 1 && i < 32; i++)
    shmem_write_u8(sm, 0x05c0 + i, (uint8_t)buf[i]);
```

**Verification:** after deploying, watch `mtime` on `/root/Energy` —
should tick every ~60s when meter personality is publishing values.

### Replaceability of FlashLog itself

**Moderate effort, ~300-400 LoC** for a `flashlog` personality:

- Reading 60 sec periodically and shelling out is trivial
- The StoreFlash() to /dev/mtdblock4 is structurally simple (single
  read → mem snapshot → checksum → write), but writing raw block
  devices needs care:
  - Erase before write (mtd) — would have to talk to /dev/mtd4 (char
    device) not /dev/mtdblock4 if we wanted full control
  - Checksum format = sum of 0..0xfff8 BE, stored at 0xfffc — match
    exactly so main's FlashToShrMem accepts it
  - Mutex protocol at shmem[0x01dd] — must coordinate with other
    daemons that touch flash (~6 writers per docs/14 §4)

Less attractive than `led` because there's no UX upside — it's pure
plumbing.

---

## Recommendations for "flash our own stuff"

Per your stated goal: **ship our own DcoFImage where we own as much of
/root as possible**. Current scorecard:

| Daemon                  | Status                          |
|-------------------------|---------------------------------|
| `RFID`                  | ✅ replaced (delta-bridge v0.6) |
| `MeterIC_new`           | ✅ replaced (meter personality, M7) |
| `Adc`                   | ✅ replaced (adc personality, M8)   |
| `LED_control`           | ⏭️ trivial to replace — recommend doing it |
| `FlashLog`              | ⏭️ moderate; one persistence fix (write shmem[0x5c0..]) lets us keep stock |
| `Pri_Comm`              | ❌ deferred (docs/18 — safety supervisor)   |
| `main`                  | ❌ too big (docs/14: HMI + contactor + Wi-Fi + USB FWUP + etc.) |
| `Charging_Standard_RFID`| ❌ too big + UART contention; needs Pri_Comm understanding first |
| `ErrorHandle`           | (SNMP trap emitter — replace only if we care about SNMP) |
| `RTC`                   | (i2c RTC chip — keep stock, works fine) |
| `DeltaOCPP`             | ❌ don't run (delta-bridge + evcc covers OCPP) |
| `snmpd`                 | upstream — unchanged                                       |

### Suggested next steps in priority order

1. **Add ~15-line kWh-string-write to meter personality** (close persistence gap; verify via /root/Energy mtime)
2. **Implement `led` personality** (~half session; trivial; opens UX surface for our own LED feedback)
3. **Decide on `flashlog` personality**: replace OR keep stock. If
   shipping our own image, "keep stock" is the lower-risk choice.
4. **Build a "stripped" DcoFImage** — remove DeltaOCPP, dead binaries
   from `/root` (ACFWMaker, ScenarioMaker, etc. that aren't referenced
   at runtime), include only daemons we trust
5. **Then we're at a point where the image is "ours"** — kernel + busybox
   + Wi-Fi/PPP/init scripts from stock (those work fine), our daemons
   for everything we replaced, stock for Pri_Comm + main + a few others
   we've explicitly decided to keep
