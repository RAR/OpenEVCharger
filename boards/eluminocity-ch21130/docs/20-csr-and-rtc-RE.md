# Charging_Standard_RFID + RTC — full static RE

**Date:** 2026-05-16
**Status:** RE complete. Headline finding: **no Linux daemon drives a
mains contactor GPIO at runtime.** All Linux-side "contactor" function
names (`SwitchD1D2`, `ConnectorLock`, `Relay_IOComm`) are **dead
code** — defined but never called. Safety actuation is on the STM32
side; Linux's role in charging permission is the J1772 pilot PWM
duty cycle.

This updates the architecture picture from docs/14 / docs/19
substantially.

---

## Part 1: RTC (13 KB)

Trivially simple I2C-based time-sync daemon.

### Wire interface

- `/dev/i2c-0` opened in `InitI2C()`
- `ioctl(fd, I2C_SLAVE=0x703, 0x51)` — RTC chip at **I2C address 0x51**
  (likely a DS1339-class chip)

### Behavior

```
init:  shmget + shmat
       InitI2C() → fd
       FT_GetRTC(fd)              # read RTC chip → buffer
       date -s "%d-%d-%d %d:%d:%d"   # system() to set Linux date
       hwclock -w / hwclock -s
       Log "System Start" to /Storage/EncodeLogMessage

loop:  sleep 1
       if shmem[0x01d5] == 1:     # "please write RTC back" trigger
           time() + gmtime() → tm struct
           clear shmem[0x01d5]
           SetRTC_Time(fd)        # write to chip via I2C
```

### Replaceability

Trivial (~150 LoC for an `rtc` personality). **Value: very low** —
stock works, no UX gain. Keep stock for the "flash our own stuff"
build.

---

## Part 2: Charging_Standard_RFID (47 KB, formerly "the big mystery")

### Function inventory

| Function       | Size (lines) | Role                                          |
|----------------|--------------|-----------------------------------------------|
| `main`         | 6,516        | EVSE main loop + state machine                |
| `RecordIdTag`  | 477          | Session-start logging (OCPP-style)            |
| `UpdateLocalList` | 379       | OCPP local-list parser + cache               |
| `GMI_Disable_Check` | 250    | GMI button safety logic                       |
| `ACW_UART_Send` | 193          | SLIP-frame TX to STM32 (the `fc 83` family)   |
| `FlashToShrMem` | 111          | Load shmem from `/dev/mtdblock4` at boot      |
| `PWM_Init`     | 68           | J1772 PWM device setup                        |
| `Pri_Comm_init`| 62           | CSR's own copy — opens `/dev/ttyAMA1`        |
| `ACdrop_detect`| 60           | Reads gpio34 (AC-line presence sense)         |
| `PWM_0_IOCtrl` | 53           | Force PWM to 0% (state F/fault)               |
| `Relay_IOComm` | 43           | DEAD CODE — writes gpio32 (never called)      |
| `ACDrop_IOCtrl`| 43           | gpio32 echo of AC presence (called by detect) |
| `Green_IOCtrl`/`Red_IOCtrl`/`Green2_IOCtrl` | 43 each | LED helpers (dupes of LED_control)    |
| `GMI_Button`   |              | GMI button input read                         |
| `PWM_On`       |              | Set pilot duty (called 14+ times in main)     |
| `DiffTimeb`    |              | Time-diff helper                              |

### .bss state variables (the state machine)

```
RS1_SendMax       Send-buffer cap
nSendSize         Pending send bytes
nRecvSize         Pending recv bytes
ACFlag            AC-line present flag
G2_status         Green2 LED state
GMI_Set_Step      GMI button-press state machine step
GMI_Set_Timeout_Flag
fd_gmi            GPIO fd for GMI button
fd_ac             GPIO fd for AC-drop input
Max_Charge_Time   Configured max session time
Duty_Cycle        Current J1772 pilot duty %
EndTime, EndTime2, EndTime3, StartTime, StartTime2, StartTime3   timers
TxDataBuf         Outbound SLIP payload (52B per docs/15 size pattern)
Pri_fd            CSR's own /dev/ttyAMA1 fd
Relay_start_time, Relay_end_time   "relay" timing (likely a command-frame timer, not actual relay)
GMI_start_time, GMI_end_time       GMI button timing
TxBuffer          Outbound full SLIP frame buffer
TempBuf           Generic scratch
MeterSMPtr        shmem base ptr
RxTime, TxTime    UART RX/TX timestamps
```

### What CSR actually controls

| Device                   | Path/GPIO              | Function called from main             |
|--------------------------|------------------------|---------------------------------------|
| **J1772 pilot PWM**      | `/dev/spr320_pwm`      | `PWM_On(fd, duty_ns)` — 14+ sites    |
| **GMI button** (input)   | `/sys/class/gpio/gpio54/value` | `GMI_Button()`                |
| **AC-drop sense** (input)| `/sys/class/gpio/gpio34/value` | `ACdrop_detect()`             |
| **AC-drop echo** (output)| `/sys/class/gpio/gpio32/value` | `ACDrop_IOCtrl()` from detect |
| **Button output**(?)     | `/sys/class/gpio/gpio44/value` | output direction set, value writes |
| **STM32 link** (out)     | `/dev/ttyAMA1` 9600 baud | `ACW_UART_Send()` — 14 distinct frames |
| **Stock LED helpers**    | gpio55/56/57           | Dupes of LED_control — likely dead duplicates |
| **`Relay_IOComm`**       | gpio32                 | **DEAD CODE — never called**         |

### The PWM format (J1772 pilot)

`PWM_On(fd, duty_ns)` writes 8 bytes to `/dev/spr320_pwm`:

```
[u32 LE period_ns][u32 LE duty_ns]
```

Period is hardcoded `0x000f4240 = 1,000,000 ns = 1 ms = 1 kHz` (J1772
spec). Duty argument controls the on-time:
- `duty_ns = 0` (0%) → CP held LOW (state F)
- `duty_ns = period` (100%) → CP held HIGH (state A, no plug detected)
- Intermediate duties encode permitted current per J1772 Table A.5

With 14+ call sites in main, this is THE charging-permission mechanism.
There's no separate contactor relay being switched — vehicle reads
pilot, decides current draw, vehicle's contactor closes.

### Where the contactor actually IS (open question)

The whole `Relay_IOComm`/`SwitchD1D2`/`ConnectorLock` dead-code chain
suggests an earlier design that had Linux-driven contactor pins. The
production design appears to have moved this to **STM32-side** via the
SLIP command frames CSR sends (the `c0 fc 83 …` family — see
docs/15). CSR's 14 distinct `ACW_UART_Send` call sites likely include
"close-contactor" / "open-contactor" / "set-current" commands. We
captured the all-zero idle frame in docs/15 but never saw a live
charging-state frame to confirm payload semantics.

**Possible alternative**: there's no separately-controlled contactor at
all on this AC mini — the EVSE relies on pilot PWM + vehicle's
contactor + a hardware-only safety circuit on the STM32 side that
hard-drops mains on fault detection. For a 30A residential charger
this is plausible.

Confirming would require either: 240 V + plug-in trace of CSR's
ACW_UART_Send frames OR a schematic of the Delta EVMU30. Not blocking
for our use case.

### RFID auth flow (UpdateLocalList + RecordIdTag)

When `/root/RFID` (or our v0.6 replacement) detects a card, it writes
the UID to `/Storage/IdTagToBeVerify`. CSR polls that file. Logic:

1. Read UID from `/Storage/IdTagToBeVerify`
2. Look up in cached `/Storage/OCPPLocalList` (managed by
   `UpdateLocalList` — keeps in-memory copy synced with the file)
3. If accepted: start charging session, set Duty_Cycle accordingly,
   `RecordIdTag` (logs to /Storage/EncodeLogMessage)
4. If rejected: pilot stays at 100% (no current permitted)

### Cadence and lifetime

CSR's main loop runs continuously, polling at ~10-100 ms intervals
(many sleep() and usleep() calls; exact cadence varies by state).
Spawned by `/root/main &` early in /etc/funs.

## Architecture (updated from docs/14)

| Component                  | Role                                                                |
|----------------------------|---------------------------------------------------------------------|
| **STM32**                  | Analog sensor hub + alarm detector + (likely) contactor/safety actuator |
| **Pri_Comm** (Linux)       | STM32 read-side only: receives alarms/measurements, posts to shmem  |
| **Charging_Standard_RFID** (CSR) | **EVSE state machine**: pilot PWM duty, RFID auth, AC-drop, GMI button, sends commands to STM32 |
| **main** (Linux)           | Supervisor + config + HMI + Wi-Fi/PPP/SNMP/VPN + USB FW + RFID local-list cache management; no direct charging control |
| **LED_control**            | Reads shmem state → drives gpio55/56/57 LEDs                        |
| **MeterIC_new / Adc / RFID / FlashLog / RTC / ErrorHandle** | Peripheral driver leaves     |
| **DeltaOCPP**              | OCPP backend client (not running on bench; replaced by delta-bridge+evcc) |

## Replaceability scorecard updated

| Daemon                  | Status                          |
|-------------------------|---------------------------------|
| `RFID`                  | ✅ replaced (delta-bridge v0.6) |
| `MeterIC_new`           | ✅ replaced (meter personality, M7) |
| `Adc`                   | ✅ replaced (adc personality, M8)   |
| `LED_control`           | ⏭️ trivial — ~200 LoC; UX upside     |
| `FlashLog`              | ⏭️ moderate — keep stock + add kWh write to meter personality (docs/19) |
| `RTC`                   | ⏭️ trivial; keep stock (no value)     |
| `ErrorHandle`           | (SNMP only — replace if you care) |
| `Charging_Standard_RFID`| ❌ **complex** — 6,516-line main, OCPP local list, STM32 command frames we don't fully understand |
| `Pri_Comm`              | ❌ deferred (docs/18 — safety supervisor) |
| `main`                  | ❌ too big + config/Wi-Fi/USB-FW; mostly orchestration  |
| `DeltaOCPP`             | ❌ don't run (delta-bridge + evcc) |

## What "flash our own stuff" actually looks like

Realistic image structure:

```
/root/RFID              → wrapper exec'ing delta-bridge (v0.6 RFID + MQTT + web)
/root/MeterIC_new       → wrapper exec'ing delta-bridge --personality=meter
/root/Adc               → wrapper exec'ing delta-bridge --personality=adc
/root/LED_control       → (optional) wrapper exec'ing delta-bridge --personality=led
/root/Pri_Comm          → stock (safety supervisor)
/root/main              → stock (config + supervisor + HMI + W-Fi)
/root/Charging_Standard_RFID → stock (EVSE state machine)
/root/FlashLog          → stock (persistence; meter personality publishes kWh ASCII to shmem[0x5c0])
/root/RTC               → stock
/root/ErrorHandle       → stock (or removed if no SNMP)
/root/DeltaOCPP         → REMOVED from image (delta-bridge + evcc covers OCPP)
/root/ACFWMaker /root/ScenarioMaker /root/PowerCard-UltraLight /root/NTC_tmp /root/PassTime /root/LogCount /root/Energy /root/FWMaker /root/Pri_Comm_cqc → REMOVED (build-time tools, not needed at runtime)
```

The result: Linux userland trimmed by ~750 KB (DeltaOCPP + dead build
tools), our delta-bridge providing modern UX + MQTT + HA + RFID + meter
+ ADC behavior, stock providing the safety-critical + configuration
paths.

## Open questions / things we could chase

1. **The 13-byte ACW_UART_Send payload semantics** — capture during
   live charging on 240 V; decode each byte's role in the SLIP frame
2. **Whether there's a Linux-side contactor we haven't found** — exhaustive
   `bl` analysis or hardware schematic review
3. **Pri_Comm safety supervisor** (docs/18 — deferred)
4. **What 0x107..0x10d shmem cluster encodes** (docs/14 §3 noted main↔OCPP pipeline)
