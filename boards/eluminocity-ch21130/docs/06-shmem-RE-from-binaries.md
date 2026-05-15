# Delta EVMU30 — Shared-Memory Layout Recovered from Binaries

**Source**: Static reverse-engineering of unstripped, debug-symbol-bearing ARMv5
ELF binaries in `/home/rar/device-configs/esphome/testcharger/delta/`.

**Method**: For each priority daemon (`Pri_Comm`, `main`, `Adc`, `MeterIC_new`,
`ErrorHandle`) plus relevant secondaries (`Charging_Standard_RFID`,
`Charging_Standard`, `LED_control`, `Pri_Comm_cqc`, `NTC_tmp`):

1. Located `shmget(0x153E, 0x40000, 0o777)` and `shmat()` — confirmed all
   daemons use the same key/size, and `MeterSMPtr` (a BSS global) holds the
   attached base pointer.
2. Wrote a small ARM-instruction symbolic executor (`/tmp/find_shm_offsets2.py`
   + `/tmp/show_writes.py`) that finds every `ldr rN,[pc,#X]` that loads the
   address of `MeterSMPtr`, follows the pointer dereference, tracks adds, and
   reports every load/store against that base with the resolved offset, the
   width, and the calling function symbol.
3. Cross-referenced numeric offsets against neighbouring code, format strings
   (`strings -t x`), and the static dump (`dump/mtd4-sharemem.bin`).

**Status**: Predictions only — to be verified live on the next bench session.

---

## 1. Top-line predictions (offset → meaning)

Endianness: **multi-byte values are little-endian unless explicitly tagged BE**
(the 32-bit ARM ABI is LE). "BE" entries are byte-by-byte marshallers in the
producer code.

| Offset | Width | Name (proposed) | Conf. | Producer | Consumer | Evidence |
|---|---|---|---|---|---|---|
| `0x0000..0x0001` | u16 LE | **Vrms** (0.1 V units) | **high** | `MeterIC_new` | `Pri_Comm` | `MeterIC_new:main@0x9f54/9f70` stores raw×Vgain to shm[0..1] LE; `Pri_Comm:main@0xb394..b3bc` reads `(shm[1]<<8)\|shm[0]` into the `Vrms` u32 global |
| `0x0004..0x0005` | u16 LE | **Irms** (0.1 A units; raw/10) | **high** | `MeterIC_new` | `Pri_Comm` | `MeterIC_new:main@0xa050/a078` stores `value/10` to shm[4..5] LE; `Pri_Comm:main@0xb3c4..b3ec` reads as LE u16 into `Irms` |
| `0x000c..0x000f` | u32 LE | **Power** (raw/1000; likely watts) | **high** | `MeterIC_new` | (logging only) | `MeterIC_new:main@0xa158..a1e8` stores 4 bytes LE; scaling uses `0x10624dd3` ÷-by-1000 magic. Not read by Pri_Comm/main in our scan — exclusively for logging/SNMP/OCPP. |
| `0x0091..0x0093` | 3 × u8 | **System test flags** | low | `main:SystemTest` | `main:SystemTest`, `main:VerifyIdTag` | strb writes from `SystemTest` only |
| `0x00c4..0x00d9` | 22 × u8 | **HMI / status scratch** | low | `main:main` | (none observed) | sequence of 22 single-byte stores in main(), probably HMI buffer |
| `0x0100` | u8 | UPDATE_IP flag | **high** | `main:GetConfig` | `main:main` | matches `decode_sharemem.py` |
| `0x0101..0x010C` | 12 B | IP/netmask/gateway (LE 4-byte each) | **high** | `main:GetConfig` | `main:main`,`Ntpdate` | matches `decode_sharemem.py` (binary form); static dump confirms |
| `0x0138..0x0157` | 32 B | **per-daemon init/error bitmap** (NOT alarm bitmap) | **high** | all daemons | `CriticalError`, `Ntpdate`, `inet_aton`,... | every daemon ORs its own bit on init/runtime fail; specifically `byte 0x157 \|= 0x40` on shmget/shmat fail (universal); `byte 0x157 \|= 0x20` on FW-up malloc fail; `byte 0x139 \|= 0x4` in ScenarioUP, etc. **This is process-health, not fault-status.** |
| `0x0158..0x015B` | u32 BE | **SNMP trap manager IP** | **high** | `main:GetConfig` | `ErrorHandle:main` (→ `inet_ntoa`) | BE-marshalled in ErrorHandle@0x86e8..874c |
| `0x015C..0x015D` | u16 | SNMP trap port | high | `main:GetConfig` | (snmptrap) | matches decode |
| `0x0190..0x0193` | u32 (IP) | SNTP server IP | high | GetConfig | Ntpdate | matches decode |
| `0x019B..0x019C` | u8 each | scenario / HMI flags | low | main | main, SystemTest, VerifyIdTag | observed paired r/w |
| `0x01AE` | u8 | scenario flag | low | main, SystemTest | main, SystemTest | r/w |
| `0x01BA..0x01BD` | 4 × u8 | config bytes | low | GetConfig, main | main, StoreFlash | r/w |
| `0x01C4..0x01CE` | 11 × u8 | config bytes | low | main, GetConfig | main, GetConfig | r/w |
| `0x01C7..0x01CA` | u32 BE | **ChargeTime / session-time** | high | `main` | `MeterIC_new` | `MeterIC_new:main@0x9788..97f4` decodes BE u32 from shm[0x1c7..0x1ca] |
| `0x01D3..0x01D4` | u16 BE | **alarm-event sequence counter** (log line #) | **high** | `Pri_Comm:main` | `Pri_Comm:main` (next log entry) | `Pri_Comm:main@0xc1c8..c210` writes `(count>>8)→[0x1d3]` then `count&0xff→[0x1d4]` |
| `0x01D5..0x01DA` | 6 × u8 | RTC bytes | high | RTC daemon | main | matches decode |
| `0x01DD` | u8 | StoreFlash-busy lock | high | main:StoreFlash | main:StoreFlash | matches decode |
| `0x01E1..0x0202` | bytes | config flags | low | GetConfig | StoreFlash, DownloadConfig | r/w |
| `0x02FE..0x02FF` | 2 × u8 | config flags | low | GetConfig | main, StoreFlash | r/w |
| `0x0362..0x0365` | 4 × u8 | unused init scratch | low | main | (none) | written-once init pattern |
| `0x0400..0x07FF` | ASCII | OCPP/charge-point identity + cleartext WiFi PSK at `0x700` | **high** | `main:GetConfig` | all OCPP consumers | matches decode_sharemem.py (verified in static dump) |
| `0x07C2..0x07C5` | 4 × u8 | gateway-mirror (IP bytes) | high | GetConfig | (none observed) | matches decode |
| `0x0800` | u8 | HMI-cmd flag | low | main, HMIRecvCMD | main | observed |
| `0x0871..0x0899` | bytes | HMI scratch | low | main | (none) | bulk strb sequence |
| `0x0A00` | u8 | **Connector Green-LED / OCPP charging-state** | **high** | `Charging_Standard*` | `LED_control` | `Charging_Standard_RFID:main` writes 0/1/2; `LED_control:main@0x8cf0..8e08` reads → 0=off, 1=solid green (plug-in), 2=Green_Flash (charging). NOT the same as J1772 pilot state. **Live evidence "always 0" matches the unit not running Charging_Standard, or not progressing past auth.** |
| `0x0A01` | u8 | **Connector Red-LED / fault-indicator state** | **high** | `Charging_Standard*` | `LED_control` | mirrors `0xA00` but for the red ring; 1=solid, 2=Red_Flash. Live evidence "pulses on event bursts" matches transient red blinks during stops/aborts. |
| `0x0A07` | u8 | **Pri_Comm state-machine output** (cable+safety digested) | **high** | `Pri_Comm:main` | `Pri_Comm:main`, `LED_control` | values seen: `0,2,3,5`. `Pri_Comm` writes after consuming `Alarm` bits + STM32 telemetry. Live evidence "resting 0x03, pulses 0x02 during retries" matches a steady "connected + idle" state with momentary 2 transitions. **NOT a simple fault byte — it's the EVSE high-level state output.** Value `5` → fault, value `0` → init/no-power. |
| `0x0A08` | u8 | **J1772 pilot state from CP-voltage classifier** | **high** | `Adc:PilotState` (via `Adc:main@0x9f50..9f70`) | `Pri_Comm:main`, `Charging_Standard_RFID` | `Adc:PilotState` measures pilot voltage via SPEAr ADC, returns 0–5: `0=State A (12V, no plug)`, `1=B (9V, plug)`, `2=C (6V, charging)`, `3=D (3V, vent)`, `4=transient/unknown (voltage out of all defined ranges)`, `5=F (-12V fault)`. Live evidence "resting 4, pulses to {1,2,3}" matches no-plug with noisy ADC. **This is the byte to watch for plug-detection** — corrects the `decode_sharemem.py` label "heartbeat". |
| `0x0A0B` | u8 (bitfield) | **STM32 link-fault flags** | **high** | `Pri_Comm:UartSend`,`Pri_Comm:UartRecv` | (no consumers observed in scanned binaries) | `UartSend@0x8b80..8bec` does `OR 0x10` on TX timeout, `BIC 0x10` on TX success. `UartRecv@0x8d30..8ff0` same pattern. **Bit 0x10 = STM32 UART tx/rx timeout.** Resting value 0 = link healthy (every successful frame clears the bit), which matches the live "0 despite link alive" observation. **Existing label "STM32_LINK" is functionally correct as a heartbeat-style flag, but it's a fault bit on a multi-bit byte, not a connect/disconnect bool.** |
| `0x0A10` | u8 | **Pilot duty-cycle %** (clipped ≥10) | **high** | `Pri_Comm:OTPCheck` | `Pri_Comm:main`, `Charging_Standard_RFID`, `LED_control` | `OTPCheck@0x9148..91a0` computes `min(10, AmbTemp * scale)` and stores as u8. This is the **J1772 pilot PWM duty %** sent to STM32. Live evidence `0x32=50` for a 30 A configured ampacity (30A × 1.667 ≈ 50 %) is exactly correct. **Corrects `decode_sharemem.py` label "VRMS" — this is NOT voltage, it's pilot duty.** |
| `0x0A11` | u8 | per-config "max current limit" mirror | low | `main:GetConfig` | (none) | a config-write-only mirror |
| `0x0A16..0x0A17` | 2 × u8 | Charging_Standard scratch | low | `Charging_Standard_RFID:main` | (none) | observed writes |
| `0x0A24` | u8 | **Rated/configured ampacity (A)** | **high** | `main:GetConfig`, `main:main` | `Pri_Comm:OTPCheck`, `Pri_Comm:main`, `main:*`, `Charging_Standard*` | static dump `0x1e=30` matches "30 A rated"; multiple readers — this is the J1772 advertised current. **Corrects `decode_sharemem.py` label "IRMS" — this is NOT measured I, it's configured ampere setpoint.** |
| `0x0A25..0x0A28` | 4 × u8 | further ampacity / phase config | low | GetConfig | various | sequential reads |
| `0x0A29..0x0A2A` | 2 × u8 | network / config bytes | low | main, GetConfig | DownloadConfig | r/w |
| `0x0A2B..0x0A5E` | ASCII | **IP / netmask / gateway as ASCII strings** | **high** | GetConfig | (logging) | static dump confirms (`192.168.100.10\0\0\0255.255.255.0\0\0\0102.168.100.1`) — note the factory typo. matches `decode_sharemem.py`. |
| `0x0A5F` | u8 | scenario flag | low | (none observed) | main | read-only in main |
| `0x0A60` | u8 | OTP recovery flag | medium | `Pri_Comm:OTPCheck` | `Pri_Comm:main` | OTPCheck@0x91dc..91e0 sets to 1 when AmbTemp causes derate |
| `0x0A62` | u8 | **meter-IC connection error** | high | `MeterIC_new:main` | `MeterIC_new:main` | written `=3` on `strstr` match of `MeterRxbuffer` against an error fingerprint |
| `0x0A63` | u8 | firmware-upgrade / config-busy flag | medium | `Pri_Comm:main`, `main:GetPrimaryFW` | `Pri_Comm:main`, `main:main` | multiple state-transition writes |
| `0x0A68` | u8 | **AC-drop event flag** | high | `Charging_Standard_RFID:ACdrop_detect` | `Pri_Comm:main` (via Alarm bit 25), `MeterIC_new:main` | ACdrop_detect routine touches this |
| `0x0A69..0x0A6F` | 7 B (BE order) | **Meter IC serial number** | **high** | `MeterIC_new:main@0xb138..b1d0` | (logging) | byte-reversed copy from MeterRxbuffer[9],[8],[7],[6],[5],(skip 4),[3] |
| `0x0A70` | u8 | **WiFi AP-connection state** | high | `main:WiFiAPConnection` | `LED_control`, `main:main` | values 0/1 (down/up). LED_control reads to drive Green2_Wifi |
| `0x0A71` | u8 | **Network state** (likely wired/eth) | medium | `main:main` | `LED_control`, `main` | paired with 0xa72, values 0/1 |
| `0x0A72` | u8 | **PingGateway result** | medium | (writer not located in trace) | `main:main`, `LED_control` | read alongside 0xa71 |
| `0x0A73` | u8 | **Charging mode (e.g. phase config)** | high | `main:GetConfig`, `main:main` | `StoreFlash`, `GetConfig`, `DownloadConfig` | static dump = `2`; default config |
| `0x0A74..0x0A77` | u32 LE | **`Alarm` bitmap (32 fault bits)** ← **the real alarm bitmap** | **high** | `Pri_Comm:main@0xbfd8..c044` | (logged into events, broadcast to SNMP) | `Pri_Comm:main` marshals its local `Alarm` u32 (at `0x16184` in Pri_Comm BSS) as LE 4 bytes into `shm[0xa74..0xa77]`. **Bit→AlarmMessage mapping (from `AlarmMessage[]` table at Pri_Comm `0x15164`, stride `0x80`)**:<br>0=OVP, 1=OCP, 2=Ambient_OTP, 3=EMGSTOP, 4=RCD, 5=WELDING, 6=UVP, 7=RA_CPU, 8=RA_WATCHDOG, 9=RA_CLOCK, 10=RA_DATA, 11=RA_FLASH, 12=RA_RAM, 13=RA_INTERRUPT, 14=RA_TIMING, 15=RA_IO, 16=RA_ADC, 17=RCDTRIP, 18=GMI, 19=PILOTERROR, 20=INITIAL, 21=Ambient_NTC_fail, 22=Plug_OTP, 23=Plug_NTC_fail, 24=RCDLOCK, 25=AC_drop, 26=FW_upgrade_fail, 27=PILOTERROR_Negative, 28=Relay_driver_fault, 29=Pri_MCU_Lost, 30=WiFi_module_fail, 31=RFID_module_fail. **Corrects `decode_sharemem.py` claim that the alarm bitmap is at 0x138 — 0x138 is process-health, the real alarm bitmap is here.** |
| `0x0A78` | u8 (bit 0) | meter-IC sign / event bit | low | `MeterIC_new:main` | `MeterIC_new:main` | observed bic/orr by 1 |
| `0x0A79` | u8 | meter-IC ready flag | low | `MeterIC_new`, `Charging_Standard_RFID` | r/w | |
| `0x0AAC` | u8 | **config default = 60** (timer or %) | **high** | `main:main@0x15afc` | (config reads) | static dump shows `0x3c=60` — matches `last_imm=60` |
| `0x0AAD` | u8 | paired config flag | medium | `main`, `GetConfig` | several config readers | |
| `0x0AB0` | u8 | **meter-IC initialization complete** | medium | `MeterIC_new:main@0xa774` | `Charging_Standard_RFID:main` | written `=1` once setup loop finishes |
| `0x0BF1..0x0BF3` | 3 × u8 | per-phase current limits | medium | `main:main`, `GetConfig` | `StoreFlash`, `GetConfig`, `DownloadConfig` | observed in cluster with `0xbf2`,`0xbf3` |
| `0x0BF5` | u8 | **OTP-derating-active flag** | high | `Pri_Comm:OTPCheck@0x910c..9534` | `Pri_Comm:main`, `Pri_Comm:OTPCheck` | written `=1` when OTPCheck enters derating state |
| `0x0BF6` | u8 | **plug-OTP derating flag** | medium | (subtle path in Pri_Comm) | `Pri_Comm:main`, `main:WiFiAPConnection` | |
| `0x0BF7` | u8 | OTP secondary flag | low | (not seen written in priority five) | `Pri_Comm:main` | read-only here |
| `0x0C14`,`0x0CAB..0x0CAC` | u8 | **RFID idTag-verify scratch** | medium | `main:VerifyIdTag` | `main:VerifyIdTag` | |
| `0x2000..0x2001` | u8 | **MeterIC_new "started" sentinel** | high | `MeterIC_new:main@0xb714`, `Charging_Standard_RFID:main` | (none observed) | written `=1` when MeterIC init succeeds |
| `0x1FFFC` | u32 BE | **half-0 checksum trailer** | high | (FlashToShrMem) | FlashToShrMem | matches existing decode |
| `0x3FFFC` | u32 BE | half-1 checksum trailer | high | FlashToShrMem | FlashToShrMem | matches existing decode |
| `0x0FFFC..0xFFFF` | 4 × u8 | half-0 checksum input bytes | high | FlashToShrMem | FlashToShrMem | observed in FlashToShrMem reads |

Anything not in this table is either unscanned (secondary daemons we didn't
deep-dive) or unwritten in the priority five. **The `0x10000..0x1FFFB`
mirror and the `0x30000..0x3FFFB` half-1 region are byte-for-byte mirrors
of `0..0xFFFB`, per `decode_sharemem.py` § "Segment structure".**

---

## 2. Per-daemon writer/reader summary

### Pri_Comm (talks to STM32F334 over `/dev/ttyAMA1`)
- **Writes**:
  - `0x0A07` ← state-machine output (0/2/3/5)
  - `0x0A10` ← computed pilot-duty % (in `OTPCheck`, clamped ≥10)
  - `0x0A60` ← OTP-derate-active flag
  - `0x0A63` ← FW/state busy flag
  - `0x0A74..0x0A77` ← `Alarm` u32 LE (the real alarm bitmap)
  - `0x0A0B` ← STM32 link timeout flag bit 0x10 (set on UART timeout in
    UartSend/UartRecv, cleared on success)
  - `0x0AEC` ← internal counter (4-byte word write in OTPCheck)
  - `0x0BF5` ← OTP-derating-active flag
  - `0x01D3..0x01D4` ← alarm-event seq# (BE u16)
- **Reads**:
  - `0x00..0x05` ← Vrms (LE u16 from shm[0..1]) and Irms (LE u16 from shm[4..5])
  - `0x0A08` ← J1772 pilot state from Adc (used to drive its own state machine)
  - `0x0A24` ← rated ampacity (to compute pilot duty %)
  - `0x0A0B`, `0x0A63`, `0x0BF5..0x0BF7`, `0x01D3..0x01D4` — own state mirror

### main (top-level EVSE/OCPP/HMI orchestrator)
- **Writes**: massive — `0x0091..0x0093`, `0x00C4..0x00D9`, `0x00FA..0x010C`,
  `0x0138/0x0139/0x013B` (process-health bits), `0x0157` (global init bit),
  `0x0158..0x015B` (SNMP trap IP — via GetConfig), `0x01BA..0x01CE`,
  `0x01C7..0x01CA` (ChargeTime BE), `0x01DA/0x01DD/0x01DE`, `0x02FE..0x02FF`,
  `0x0362..0x0365`, `0x0400..0x07FF` (entire OCPP config block — via
  GetConfig), `0x07C2..0x07C5`, `0x0800`, `0x0871..0x0899`, `0x0A11`,
  `0x0A24`, `0x0A29..0x0A2A` (network ASCII), `0x0A63`, `0x0A70..0x0A77`
  (network state + Alarm — wait: 0xa74..0xa77 are written by Pri_Comm; main
  only touches 0xa70..0xa73), `0x0A73` (ChargingMode), `0x0AAC..0x0AAD`,
  `0x0BF1..0x0BF3`, `0x0BF6`, `0x0C14`, `0x0CAB..0x0CAC`.
- **Reads**: similarly broad. **Critically: only reads `0x0A08` (one place,
  at `main:main@0x16ea8`) — it does NOT write the pilot state.**

### Adc (CP-voltage classifier)
- **Writes**: `0x0A08` ← J1772 pilot state (in main loop, every iteration,
  from `PilotState()` return value).
- **Reads**: `0x0A08`, `0x0BF4`.
- No other shmem writes — this is a minimal daemon, just samples ADC and
  publishes the state code.

### MeterIC_new (meter chip UART)
- **Writes**: `0x0000..0x0001` (Vrms LE u16), `0x0004..0x0005` (Irms LE u16),
  `0x000C..0x000F` (Power LE u32), `0x01C7..0x01CA` (ChargeTime BE — but wait,
  this is read here, written by main), `0x0A62` (meter-error state),
  `0x0A68..0x0A6F` (meter serial bytes), `0x0A78` bit-0, `0x0A79`, `0x0AB0`,
  `0x2001`.
- **Reads**: `0x01C7..0x01CA` (ChargeTime BE from main), `0x0A62` own state,
  `0x0A68` AC-drop, `0x0A78`, `0x0A79`.

### ErrorHandle (SNMP trap forwarder)
- **Writes**: only `0x0157` bit 0x40 on shmget/shmat fail.
- **Reads**: `0x0158..0x015B` ← BE u32 → `inet_ntoa` → trap-destination URL.
- That's it. ErrorHandle does NOT process any per-alarm-bit logic in
  shmem; the alarms are processed by Pri_Comm internally and logged via
  the `EncodeLogMessage` shell pipe. ErrorHandle is just an SNMP relay.

### Charging_Standard_RFID (OCPP/RFID orchestrator)
- **Writes**: `0x0A00` (Green LED state 0/2), `0x0A01` (Red LED state),
  `0x0A10` (override pilot duty during transitions), `0x0A16..0x0A17`,
  `0x0A24` ← rated current setpoint, `0x0A60`, `0x0A62`, `0x0A79`, `0x0AAC`,
  `0x0C14`, `0x0CAB..0x0CAC`, `0x2000`, plus `0x0A68` from sub-routine
  `ACdrop_detect`.

### LED_control
- Pure reader. Reads `0x0A00`/`0x0A01` (LED commands), `0x0A07` (=5 →
  red fault flash), `0x0A70..0x0A72` (network status → green2 wifi LED).

### Pri_Comm_cqc, NTC_tmp
- Both compile-identical to `Pri_Comm`. Same shmem footprint.

---

## 3. Resolution of the puzzles

### Cable state / CP voltage byte (the BIG one)

**Two different bytes carry "connector state" semantics — they're produced
by different daemons and mean different things:**

- **`0x0A08`** = J1772 pilot-voltage classifier output (raw cable physics).
  Written by `Adc:PilotState` from analog pilot voltage measurements via
  `GetPilotVolt()`. Values 0=A, 1=B, 2=C, 3=D, 4=transient, 5=F.
  **This is the byte that reacts to plug-in events.**
- **`0x0A00`** = OCPP/RFID-authorised "user-facing" state. Written by
  `Charging_Standard_RFID:main` once the high-level transaction state
  decides "this is a charge". Values: 0=idle, 1=authorised/ready, 2=charging.
  **This only changes after RFID/OCPP authorisation — explains live "always 0".**
- **`0x0A07`** = Pri_Comm's digested state (cable + safety MCU + alarms).
  This is _between_ raw cable physics and OCPP state. Values 0/2/3/5.
  Live evidence "resting 3, pulses to 2" matches "ready to charge, briefly
  transitions during STM32 retries".

For a bridge meant to broadcast cable state to MQTT, the right byte to
watch is **`0x0A08`** (rawest, fastest, most diagnostic).

### Proximity (PP) byte
**Not directly visible in shmem.** Searching all priority binaries for PP /
proximity / plug-detect: no shmem byte is dedicated to PP. The PP signal is
sampled by STM32F334 over the safety-MCU UART, and Pri_Comm folds it into
the alarm bits (`PILOTERROR` bit 19, `PILOTERROR_Negative` bit 27) and into
the state-machine output at `0x0A07`. **There is no separate PP byte.**

### Real VRMS / IRMS (measured)
- **VRMS** (measured): `shm[0x0000..0x0001]` LE u16, units **0.1 V**.
  Range 0..6553.5 V. 230 V mains → stored as `2300`.
- **IRMS** (measured): `shm[0x0004..0x0005]` LE u16, units **0.1 A** (raw/10).
- **Power**: `shm[0x000C..0x000F]` LE u32, units = raw/1000 (likely watts, but
  needs live verification at known load).
- The bytes at `0x0A10` (50) and `0x0A24` (30) are **NOT** Vrms/Irms —
  they're pilot duty % and rated/configured ampacity respectively. The
  static dump and the live `0x0a24=0x1e` are both consistent with this.

### Alarm bitmap (where is it?)
- **NOT at `0x0138`** — that's a per-daemon process-health bitmap
  (init/runtime fail bits).
- **The real alarm bitmap is at `shm[0x0A74..0x0A77]`** as a 32-bit
  little-endian value, broadcast every Pri_Comm main-loop iteration from
  the Pri_Comm-local `Alarm` u32. The 32 bit-positions correspond to the
  32 alarm strings in Pri_Comm's `AlarmMessage[32]` table (mapping listed
  above in §1).

### `0x0A01`
**Red-LED state byte**, written by `Charging_Standard_RFID:main`, read by
`LED_control:main`. Values 0=off, 1=solid red, 2=red flash. The "pulses on
event burst" live observation matches red-flash episodes during transient
fault states. **Not a generic producer flag.**

### `0x0A08`
**EVSE state / J1772 pilot state code from `Adc:PilotState`.** See above —
this IS the cable-state byte, just named wrongly as "heartbeat". Resting 4
under noisy idle is a "pilot voltage in no-state zone" classification —
expected when no plug present and ADC samples drift.

### `0x0A0B`
**STM32-UART link fault bitmap, bit 0x10 = "Pri_Comm↔STM32 UART timeout"**.
Set on timeout in `UartSend`/`UartRecv`, cleared on every successful frame
RX/TX. Resting 0 = healthy. Other bits in this byte may carry other STM32
sub-faults — only bit 4 (0x10) was exercised in the disassembly we walked.

---

## 4. Corrections to `decode_sharemem.py`

| Existing label | Verdict | Correction |
|---|---|---|
| `OFF_CONNECTOR_STATE = 0x0a00` | **partially right** | Is the **Green-LED / OCPP user-state** byte (idle/auth/charging). Not the raw cable state; that's `0x0A08`. Live evidence "always 0" makes sense — unit not running Charging_Standard or no authorised session. |
| `(0x0a01 = unidentified)` | **NEW** | **Red-LED state byte** (0/1/2 = off/solid/flash). |
| `OFF_FAULT_FLAGS = 0x0a07` | **wrong label, right intent** | Is the **Pri_Comm digested state-machine output** (0/2/3/5). Not a flags byte — a small enum of EVSE meta-states. Live "resting 0x03 = UVP" interpretation was coincidence; `0x03` is just "state 3" in Pri_Comm's internal FSM. |
| `OFF_HEARTBEAT = 0x0a08` | **wrong** | Is the **J1772 pilot state code** from `Adc:PilotState`. Values 0-5 = A/B/C/D/transient/F. NOT a heartbeat counter. Live "rests at 4, pulses {1,2,3}" matches a noisy-no-plug classifier. |
| `OFF_STM32_LINK = 0x0a0b` | **partially right** | It's a **fault-bit byte**, specifically bit 0x10 = STM32 UART tx/rx timeout. "0 = link alive" interpretation is correct, but it's masking — the byte has spare bits. |
| `OFF_VRMS = 0x0a10` | **wrong** | Is **pilot duty cycle %** (0..100, clamped ≥10), written by Pri_Comm OTPCheck. Live `0x32=50` for 30 A rated. **Real Vrms is at `0x0000..0x0001` LE u16, 0.1 V resolution.** |
| `OFF_IRMS = 0x0a24` | **wrong** | Is **configured/rated ampacity in amps**, written by main:GetConfig. Live `0x1e=30` for 30 A unit. **Real Irms is at `0x0004..0x0005` LE u16, 0.1 A resolution.** |
| `OFF_ALARM_BITMAP = 0x0138 (32 B)` | **wrong** | `0x0138..0x0157` is **per-daemon process-health bitmap** (init / runtime / FW-upgrade error bits — not safety alarms). **Real alarm bitmap is at `0x0A74..0x0A77` LE u32**, with the 32 bit-positions mapped to `AlarmMessage[32]` (full mapping in §1). |
| `0x158 = SNMP trap mgr IP` | **right** | Confirmed: ErrorHandle decodes it BE → `inet_ntoa`. |
| `0x1d3/0x1d4 serial bytes` | **wrong** (label) | They're a **BE u16 alarm-event sequence counter** (incremented each time Pri_Comm logs an alarm). The unit serial is in `/Storage/SerialNumber`. |
| `0xbf1..0xbf6 limit cur` | **plausibly right** | Per-phase config bytes. Need live observation under varied config to confirm scaling. |
| `0x400+ OCPP config block` | **right** | Confirmed by static dump and main:GetConfig writes. |
| `0xa2b ASCII IP / 0xa3b netmask / 0xa4b gateway` | **right** | Confirmed. |

### Suggested new labels to add

```python
# Metering (measured) — written by MeterIC_new
OFF_VRMS_MEAS    = 0x0000  # LE u16, 0.1 V units
OFF_IRMS_MEAS    = 0x0004  # LE u16, 0.1 A units
OFF_POWER_MEAS   = 0x000c  # LE u32, units = raw/1000 (likely W)

# Connector / state
OFF_GREEN_LED    = 0x0a00  # u8  0/1/2 (Charging_Standard)
OFF_RED_LED      = 0x0a01  # u8  0/1/2 (Charging_Standard)
OFF_PRI_STATE    = 0x0a07  # u8  0/2/3/5 (Pri_Comm state machine output)
OFF_J1772_STATE  = 0x0a08  # u8  0..5 = A/B/C/D/transient/F (Adc:PilotState)
OFF_STM32_FAULT  = 0x0a0b  # u8  bit 0x10 = UART timeout
OFF_PILOT_DUTY   = 0x0a10  # u8  pilot PWM duty %
OFF_RATED_AMPS   = 0x0a24  # u8  configured max amps
OFF_WIFI_STATE   = 0x0a70  # u8  0=down, 1=up
OFF_NET_STATE    = 0x0a71  # u8  paired with 0x0a72
OFF_PING_RESULT  = 0x0a72  # u8  PingGateway result
OFF_CHRG_MODE    = 0x0a73  # u8  default 2
OFF_ALARM_BITMAP = 0x0a74  # LE u32, 32 alarm bits (see §1 mapping)
OFF_AC_DROP      = 0x0a68  # u8  AC drop flag
OFF_METER_SERIAL = 0x0a69  # 7B  BE-order meter chip serial
OFF_METER_READY  = 0x0ab0  # u8  meter init done
OFF_CHARGE_TIME  = 0x01c7  # BE u32, session seconds (4 B)

# Process health (NOT alarms)
OFF_PROCHEALTH   = 0x0138  # 32 B, per-daemon init/error bitfield
OFF_PROCHEALTH_GLOBAL = 0x0157  # u8  bit 0x40 = any daemon's shmem-init fail

# Logging
OFF_ALARM_SEQ    = 0x01d3  # BE u16 alarm-event counter
```

---

## 5. Residual uncertainty (what we did NOT pin down)

- **Power units at `0x000c..0x000f`**: the `÷1000` magic constant in
  MeterIC_new suggests raw-mW → watts, but the meter chip's native units
  weren't fully traced. Verify on bench under a known load (e.g. 1.5 kW
  kettle: expect `1500` ± gain at `0x000C..0x000F`).
- **`0x0BF1..0x0BF3`**: assumed "per-phase current limits" by proximity to
  `0x0BF5..0x0BF7` (OTP flags) and the GetConfig/StoreFlash r/w pattern,
  but exact semantics not confirmed.
- **`0x0A11`, `0x0A29..0x0A2A`**: GetConfig-only writes, not read by
  anyone in the priority five. Possibly stale config mirrors or used by
  secondary daemons (`snmpd`, `DeltaOCPP`) that we did not scan.
- **`0x0A07` value 4**: never written by `Pri_Comm` in the scanned paths,
  but checked-against (`if 0xa07 == 4 then …`). May be written by an
  init path we missed, or by `Charging_Standard*`.
- **`0x0A0B` bits other than 0x10**: only bit 4 was exercised in the
  UartSend/UartRecv paths we walked. Other bits may be in other Pri_Comm
  functions (e.g. UART open/close failure paths).
- **`shm[0x10000..0x1FFFB]` and `shm[0x30000..0x3FFFB]`**: per existing
  decoder, these are byte-for-byte mirrors of the lower 64 KiB. We did
  not re-validate that claim from the binaries (no daemon obviously
  writes the mirror; only `FlashToShrMem` reads `0xFFFC..0xFFFF` for the
  checksum). Trust existing decode for now.
- **Half-1 checksum field**: written by `FlashToShrMem` periodically. The
  exact accumulator algorithm is in `main:FlashToShrMem` but we did not
  fully decode it.
- **`shm[0x2000]`/`shm[0x2001]`**: only seen written as `=1` by MeterIC_new
  and Charging_Standard_RFID. Probably "subsystem ready" flags. Not
  consumed in the priority five.
- **Bit assignments within `0x0138..0x0157`** (process-health bitmap):
  only a few bits were traced (0x40 universal, 0x4/0x20 in FW upgrade, etc.).
  Full mapping would require walking each daemon's init-fail paths.
- **Live observation of `0x0a00` going to 2 during charging**: we predict
  this based on Charging_Standard's writes of `last_imm=2`, but the
  factory-fresh unit has never charged with auth, so it's untested.

These are all "low-priority, would-be-nice-to-confirm" items. The main
predictions above (Vrms/Irms/Power/state/alarm bitmap) are high-confidence
and ready to verify on the next bench session.
