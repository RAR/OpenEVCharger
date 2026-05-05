# OpenEVCharger v1 — Safety-Core Firmware Design

**Date:** 2026-05-02
**Status:** Design approved, pending implementation plan
**Target hardware:** Rippleon ROC001 / NewEnergyCS ROC-family EVSE
**Target MCU:** GigaDevice GD32F205V (LQFP100, Cortex-M3 @ 120 MHz, 256 KB flash, 128 KB RAM)
**License:** GPL-3.0-only

## 1. Overview & scope

OpenEVCharger v1 replaces the stock Rippleon MCU firmware with a tightly-focused safety-and-charging core, modeled on OpenEVSE (https://github.com/OpenEVSE/open_evse) but written fresh in C against the GD32F20x vendor library and FreeRTOS. The MCU owns all safety-critical control: J1772 control-pilot generation and classification, contactor actuation, GFCI/RCD monitoring, ground-continuity check, NTC over-temperature, current limiting, and the relay-state-vs-commanded weld detector.

Non-safety features (Wi-Fi, BLE, cloud, OCPP, HA integration, RFID, LCD UI, scheduling) live on the FC41D Wi-Fi/BLE module, which drives the MCU through a binary TLV protocol over UART5. The MCU treats FC41D requests as advisory: it caps any requested current at the DIP1-configured hardware maximum and refuses to close the contactor without valid CP/CC/GFCI state regardless of what the FC41D asks.

### What v1 includes
- Full J1772 state machine (states A, B, C, E, F; refuses D)
- Boot self-test gate (relay readback, GFCI CAL exercise, ADC sanity, CRC of persisted config)
- Latched and self-clearing fault model
- Persistent storage on the W25Q64 SPI NOR (reformatted; stock firmware backed up first)
- WS2812 strip + buzzer + button user I/O
- Heartbeat LED on PD4
- IWDG watchdog
- TLV control protocol over UART5 to FC41D

### What v1 explicitly does NOT do
- Wi-Fi, BLE, cloud, OCPP, HA, MQTT
- Schedules / timed sessions (FC41D simulates by setting advertised amps to 0)
- RFID
- DGUS LCD UI
- Lock solenoid (SKU has none — only CP wired through to plug)
- DC charging / CCS / V2G
- UART DFU bootloader (SWD-only flashing in v1; bootloader is a separate spec)

### Future specs
- v2: UART DFU bootloader with signed updates
- v3: RFID auth on USART2 (bench-blocked currently)
- v4: DGUS LCD on USART1 (or skip, FC41D-driven instead)
- v5: All cloud/HA/OCPP work — done entirely on the FC41D, off the MCU

## 2. Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  GD32F205V (120 MHz Cortex-M3, 256 KB flash, 128 KB RAM)        │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │ FreeRTOS kernel (cooperative-priority preemption)       │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                                 │
│  Tasks (highest priority → lowest):                             │
│   ┌──────────────┐ ┌─────────┐ ┌─────────┐ ┌──────────────┐     │
│   │ safety_task  │ │io_task  │ │comms_   │ │persist_task  │     │
│   │ 50 Hz        │ │30 Hz    │ │task     │ │event-driven  │     │
│   │ owns relays  │ │buzzer/  │ │(FC41D   │ │(W25Q write)  │     │
│   │ + CP class   │ │WS2812/  │ │TLV)     │ │              │     │
│   │              │ │buttons) │ │         │ │              │     │
│   └─────┬────────┘ └────┬────┘ └────┬────┘ └────────┬─────┘     │
│         │ latched data  │           │               │           │
│  ┌──────▼───────────────▼───────────▼───────────────▼─────┐     │
│  │ HAL drivers (vendor lib + thin wrappers)               │     │
│  │  gpio · adc(scan) · adc(injected) · tim_pwm · spi3/    │     │
│  │  w25q · uart5/fc41d · uart1/lcd-stub · wdg · ws2812    │     │
│  └────────────────────────────────────────────────────────┘     │
│                                                                 │
│  ISRs (preempt all tasks):                                      │
│   - GFCI EXTI on dedicated input pin                            │
│   - ADC injected EOC (CP sample)                                │
│   - TIM trigger (1 kHz CP PWM rising edge)                      │
│   - UART5 RX FIFO (FC41D)                                       │
│   - SysTick (FreeRTOS scheduler)                                │
└─────────────────────────────────────────────────────────────────┘
       │                       │                       │
       ▼                       ▼                       ▼
   PE12 (DPDT contactor)   PE13 (CP PWM)       UART5 → FC41D
   PE0  (aux relay)        PA4 (CP read-back)   (TLV protocol)
```

### Tasks

| Task | Priority | Rate | Responsibility |
|---|---|---|---|
| `safety_task` | highest | 50 Hz (20 ms tick) | Read latched ISR state, classify J1772, run safety check matrix, decide & apply relay+PWM, kick watchdog. **Single owner of PE12, PE0, TIM1_CCR3.** |
| `io_task` | medium-high | 30 Hz | WS2812 frame composition, buzzer pattern engine, button scan from ADC results, heartbeat LED |
| `comms_task` | medium | event-driven | UART5 frame parse, command dispatch, async event publish |
| `persist_task` | low | event-driven | Drain a queue of W25Q write requests; lowest-priority so it never starves higher work |

### ISRs

| ISR | Action |
|---|---|
| GFCI EXTI | Set latched flag in safety state, set `last_fault = GFCI`. Don't actuate relay (that's safety_task's job — it sees the flag on next tick within 20 ms). |
| ADC injected EOC | Latch CP HIGH-phase sample at `cp_high_mv`. The companion LOW-phase sample uses a second injected slot triggered ~750 µs later. |
| TIM update (1 kHz CP rising edge) | Triggers ADC injected sample chain. |
| UART5 RX | Push received bytes into a FreeRTOS stream buffer for `comms_task` to consume. |
| SysTick | FreeRTOS scheduler tick. |

### Single-writer discipline

All relay GPIO writes (PE12, PE0) and all CP PWM duty changes (TIM1_CCR3) come from `safety_task` only. No ISR, no other task, ever writes those. This single-writer discipline removes a whole class of concurrency bugs from the most dangerous outputs in the system.

### Data flow per safety_task tick (20 ms)
1. Read latched ISR data: GFCI flag, latest CP HIGH+LOW samples, latest scan ADC values, relay-sense (PB12).
2. Classify J1772 state from CP HIGH sample (with hysteresis bands).
3. Run safety check matrix in priority order. First failing latched check raises a fault and overrides the rest.
4. Decide: target relay state + advertised PWM duty.
5. Apply: set TIM1_CCR3 (duty), set/clear PE12+PE0 via BSRR.
6. Update mutex-guarded shared `system_state_t` snapshot.
7. Kick IWDG.

## 3. J1772 control pilot state machine

The control pilot is generated as a 1 kHz PWM on PE13 (TIM1_CH3 alternate function) with ±12 V swing. PA4 reads CP back via a divider that maps ±12 V → 0–3.3 V.

The TIM1 update event triggers an ADC injected conversion ~400 µs after the PWM rising edge — well past the slew settle (high-drive op-amp on the CP output rises in <50 µs) and well before the next falling edge for a 1 kHz / 50% duty waveform. A second injected slot triggers ~750 µs after the rising edge (i.e. just after the falling edge) to capture the LOW-phase value used for the diode check.

### State classifier table (HIGH-phase voltage at CP, after divider→ADC→V_cp_mv reconstruction)

| ADC band | J1772 | Meaning | Contactor | PWM duty |
|---|---|---|---|---|
| > +10.5 V eq. | A | No vehicle | open | 0% (CP idle high) |
| +7.5 to +10.5 V | B | Vehicle plugged, not charging | open | advertised amps duty |
| +4.5 to +7.5 V | C | Charging, no vent | **closed** | advertised amps duty |
| +1.5 to +4.5 V | D | Charging, vent required | **OPEN** (we don't support D) | advertised amps duty |
| -1.5 to +1.5 V | E | Fault — no CP | open + latched fault | 0% |
| < -1.5 V | F | EVSE not ready (we drive this on fault) | open | output -12 V |

### Debouncing
Three consecutive same-band reads required to commit a state transition (60 ms at 50 Hz). The ±0.5 V dead zones around each boundary prevent chatter at noisy boundaries.

### Diode check
On the first transition from A → B, require `cp_low_mv ≤ -10 V` (the EV's diode pulled the LOW phase down). If LOW reads near 0 V, the EV's diode is missing/shorted → latched fault `DIODE_CHECK_FAIL`. This catches a defective or hostile cable claiming to be a vehicle.

### Advertised current → duty cycle (per J1772 / IEC 61851)
- For 6 ≤ A ≤ 51: `duty% = A × 0.6`
- For 51 < A ≤ 80: `duty% = A / 2.5 + 64`
- Effective advertised amps = `min(DIP1-config, FC41D-requested, hardware-rating)`
  - DIP1 closed → 40 A max, DIP1 open → 48 A max (verified in stock firmware disasm at flash 0x800fdd8 / 0x800fe30)
  - Hardware rating: 48 A (board has a 48 A contactor)

### CP regression
A C → B transition mid-session is treated as a transient pause: contactor opens immediately, but we allow re-progression to C. A C → A transition is unplug: contactor opens, session ends.

## 4. Safety supervisor check matrix

Every 20 ms tick of `safety_task`, evaluated in this order. First failing latched check raises a fault and the rest of the tick processes the fault rather than the normal state.

```
1.  GFCI                 [LATCHED]      EXTI flag from ISR (and initial-state poll)
2.  Relay weld           [LATCHED]      PB12 reads "closed" while PE12 commanded open ≥ 200 ms
3.  Relay-stuck-open     [LATCHED]      PB12 reads "open" while PE12 commanded closed ≥ 200 ms
4.  PE continuity loss   [LATCHED]      PC5 ADC out of expected band > 10 ticks
5.  CP=E (no pilot)      [LATCHED]      band E sustained 3 ticks
6.  Diode check fail     [LATCHED]      first state-B entry reads LOW-phase > -10 V
7.  Boot self-test fail  [LATCHED]      see § 4.1
8.  ADC out-of-range     [LATCHED]      any rank reads 0 or full-scale > 5 ticks (100 ms; broken sensor)
9.  Over-temperature     [SELF-CLEAR]   either NTC > 85 °C; recover < 70 °C
10. Hard over-current    [LATCHED]      CT reads > 1.20× advertised for > 5 s
11. Soft over-current    [SELF-CLEAR]   CT > 1.05× advertised for > 30 s → ramp duty −10%, log
12. CC out of range      [SELF-CLEAR]   PA7 outside 13/20/32/40/80 A bands
13. AC supply absent     [SELF-CLEAR]   PA2 reads no AC > 500 ms
14. CP regression        [SELF-CLEAR]   C→B mid-session, allow re-progress
```

### 4.1 Boot self-test sequence

Gates first contactor close. Adds 200–500 ms to first-charge readiness but catches dead components before a human or vehicle is in the loop.

1. Verify all ADC ranks return non-rail values within 100 ms of init.
2. Pulse PE3 (GFCI CAL) high for 50 ms; expect GFCI EXTI to fire. Clear flag. If no fire → fault `GFCI_SELF_TEST_FAIL`.
3. Command PE12 open, wait 100 ms, read PB12 → expect "open". If reads "closed" with no command → fault `RELAY_WELD_AT_BOOT`.
4. Command PE12 closed for ≤ 50 ms with CP held in state A (so no vehicle-side contactor closure consequence); read PB12 → expect "closed". If reads "open" → fault `RELAY_OPEN_AT_BOOT`. Then command open.
5. Verify CP voltage with TIM1 PWM at idle: PA4 should read state-A band.
6. CRC W25Q `boot_config` region; if fail, fall back to defaults + log.

If any step fails: enter state F (drive CP to −12 V), light WS2812 all red, buzzer triple-beep on 1 Hz cycle, refuse to close contactor regardless of any subsequent input. FC41D can read the fault code but cannot clear (power-cycle required for self-test failures).

### 4.2 Fault clearing model

- **GFCI trip** stays latched until power cycle. Matches J1772 / common-EVSE convention.
- **All other latched faults** can be cleared by FC41D `CLEAR_FAULT` TLV command. Fault history preserved in `event_log` regardless.
- **Self-clearing faults** clear automatically when the underlying condition removes (with hysteresis where applicable).
- **Soft warnings** never open the contactor; only logged + reported via TLV event.

### 4.3 Mutex on shared state

A single `xSemaphoreCreateMutex()` guards the published `system_state_t` snapshot. `safety_task` writes it (taking the mutex briefly at the end of its tick), other tasks read it via snapshot copy under the mutex. Faster than a queue at our rates and the only writer is one task. `xSemaphoreTake(timeout=10ms)` — if it ever fails to acquire (impossible by design, defensive only) we log and skip the tick rather than spin.

## 5. FC41D ↔ MCU TLV protocol

UART5 (PC12/PD2), 115200 8N1. Both directions speak the same frame format. CRC16-CCITT (poly 0x1021, init 0xFFFF) over LEN…PAYLOAD inclusive.

### Frame format

```
 0    1    2    3    4    5    6    ...    N-2  N-1
+----+----+----+----+----+----+----+   +----+----+----+
| A5 | 5A | LN | LN | CMD| SEQ| ... payload ... |CRCH|CRCL|
+----+----+----+----+----+----+----+   +----+----+----+
       SOF(2)  len(2,LE) cmd seq      payload      CRC16
```

- `A5 5A` start-of-frame (matches BLE A5 prefix from stock — keeps tooling muscle memory)
- `LEN` = u16 LE = bytes from `CMD` through end of payload (excluding CRC)
- `CMD` = u8 (high bit: 0x00–0x7F = request, 0x80–0xFF = response/event)
- `SEQ` = u8, request-response pairing; events use 0x00
- Max frame size: 64 bytes (8-byte header + CRC + ≤54-byte payload)

### FC41D → MCU requests

| CMD | Name | Payload | Notes |
|---|---|---|---|
| `0x01` | `PING` | none | Health check |
| `0x02` | `GET_STATE` | none | Response = full snapshot |
| `0x03` | `SET_ADVERTISED_AMPS` | u8 amps | Clamped to DIP1 + hw cap |
| `0x04` | `REQUEST_STOP` | u8 reason | Open contactor cleanly (ramp duty to 0% first) |
| `0x05` | `REQUEST_START_RESUME` | none | Exit user-paused state |
| `0x06` | `CLEAR_FAULT` | u32 fault_id | Non-GFCI latched faults only |
| `0x07` | `GET_FAULT_LOG` | u8 count | Response = up to N most-recent records |
| `0x08` | `GET_LIFETIME_KWH` | none | Response = u32 mWh |
| `0x09` | `WRITE_CALIBRATION` | struct cal | Persist new CT zero-offset etc. |
| `0x0A` | `SET_LED_OVERRIDE` | u8 mode + u8[3] rgb | FC41D-driven UI; auto-revert on state change |
| `0x0B` | `BUZZER_BEEP` | u16 ms | UI feedback (capped 500 ms; only in non-fault states) |
| `0x0C` | `GET_BUILD_INFO` | none | Response = git sha + version + build date |

### MCU → FC41D events (unsolicited)

| CMD | Name | Payload |
|---|---|---|
| `0x80` | `STATE_CHANGED` | new J1772 state byte |
| `0x81` | `FAULT_RAISED` | u32 fault_id, snapshot |
| `0x82` | `FAULT_CLEARED` | u32 fault_id |
| `0x83` | `SESSION_BEGAN` | timestamp |
| `0x84` | `SESSION_ENDED` | u32 mWh delivered, duration s |
| `0x85` | `BOOT_COMPLETE` | bool self_test_passed, last_fault_id |

### `system_state_t` snapshot

```c
enum j1772_state  { J1772_A = 'A', J1772_B = 'B', J1772_C = 'C', J1772_E = 'E', J1772_F = 'F' };
enum evse_state {
    EVSE_BOOT = 0,
    EVSE_SELF_TEST,
    EVSE_READY,
    EVSE_CHARGING,
    EVSE_USER_PAUSED,
    EVSE_COOLING_DOWN,
    EVSE_FAULT,
};

struct system_state {
    u8  j1772_state;         // J1772_A/B/C/E/F
    u8  evse_state;          // EVSE_*
    u8  contactor_commanded; // 0/1
    u8  contactor_sensed;    // 0/1
    i16 cp_high_mv;          // signed mV, last HIGH-phase sample
    i16 cp_low_mv;           // signed mV, last LOW-phase sample
    u8  advertised_amps;
    u8  pad0;
    u16 active_amps_x10;     // measured via CT
    u16 ntc1_dC;             // gun NTC #1 deci-°C
    u16 ntc2_dC;             // gun NTC #2 deci-°C
    u16 cc_max_amps;         // cable capacity
    u8  ac_present;
    u8  pad1;
    u32 last_fault_id;
    u32 session_mwh;
};
```

### Rate limiting

MCU rate-limits identical events to ≤ 1/100 ms to avoid flooding FC41D during a chattering edge. Boot sequence: MCU sends `BOOT_COMPLETE` exactly once after self-test; FC41D should not assume MCU is alive until then.

## 6. W25Q64 persistence

### Driver

Hardware SPI3 (PB3 SCK, PB4 MISO, PB5 MOSI) + PB6 GPIO-CS bit-banged via BSRR/BRR. Synchronous blocking ops only — no DMA. Driver:

```c
w25q_read(addr, buf, len);
w25q_erase_sector(addr);        // 4 KB-aligned
w25q_program(addr, buf, len);   // ≤ 256-byte page
w25q_chip_id() -> u32;          // verify presence at init
```

Owned exclusively by `persist_task`; other tasks queue write requests via a FreeRTOS queue. No other task touches SPI3.

### Memory map (8 MB total, sector = 4 KB)

```
0x000000 – 0x001FFF  ( 8 KB)   boot_config         (4 KB × 2 ping-pong, CRC32 per record)
0x002000 – 0x003FFF  ( 8 KB)   calibration         (4 KB × 2 ping-pong, CRC32 per record)
0x004000 – 0x043FFF  (256 KB)  event_log           (64 sectors, ring-buffered records)
0x044000 – 0x04BFFF  ( 32 KB)  session_log         ( 8 sectors, ring-buffered session totals)
0x04C000 – 0x04CFFF  (  4 KB)  boot_count + last_fault (single sector, ping-pong inside)
0x04D000 – 0x7FFFFF  (~7.7 MB) reserved (future: RFID idtags, OCPP cache, OTA staging)
```

### Records

All persisted structs are little-endian, packed (`__attribute__((packed))`), end with a CRC32 over the preceding bytes, and have a `version` byte at offset 0 for forward-compat.

```c
struct boot_config_record {        // 32 bytes total, ping-pong
    u8  version;                   // 1
    u8  pad0[3];
    u8  fc41d_advertised_amps;     // 0 = unset → fall back to DIP1
    u8  pad1[3];
    u8  reserved[16];
    u32 crc32;
};

struct calibration_record {        // 32 bytes total, ping-pong
    u8  version;                   // 1
    u8  pad0[3];
    i16 ct902_zero_offset;         // raw ADC counts to subtract from CT902 (PC0)
    i16 leakage_ct_zero_offset;    // same for PC1
    i16 cp_divider_trim_mv;        // additive correction in mV
    i16 ntc_pullup_trim_pct;       // ±N% adjustment to nominal pull-up
    u8  reserved[12];
    u32 crc32;
};
```

Ping-pong scheme: write candidate record to currently-erased slot, verify-read, then erase the other slot. Reader picks slot whose CRC validates and whose internal monotonic counter is highest. If both slots invalid → write defaults to slot A.

`event_log` — append-only ring of 32-byte records:
```c
struct event_record {                // 32 bytes
    u32 timestamp;
    u16 boot_count;
    u16 fault_id;
    u8  j1772_state;
    u8  evse_state;
    i16 cp_mv;                       // signed
    u16 cc_amps;
    u16 ntc1_dC;
    u16 ntc2_dC;
    u16 active_amps_x10;
    u8  reserved[8];
    u16 crc16;
};
```
4 KB sector = 128 records. 64 sectors = 8192 records total. **No head-pointer header sector** — at boot, scan the log to find the highest valid `boot_count` then highest `timestamp`; that's the head. Append to next slot; when sector fills, erase the next sector before writing into it. Sector with all 0xFF bytes is treated as empty (W25Q post-erase state). Cost: ~64 × 4 KB sector reads at boot ≈ 50 ms total at 12 MHz SPI — acceptable, replaces a class of header-corruption bugs.

`session_log` — 32-byte records:
```c
struct session_record {              // 32 bytes
    u32 start_ts;
    u32 end_ts;
    u32 mwh_delivered;
    u8  end_reason;
    u8  j1772_max_state_seen;
    u16 fault_count;
    u16 max_temp_dC;
    u8  reserved[10];
    u16 crc16;
};
```
~1024 sessions per 32 KB region. Same scan-on-boot head-discovery as event_log.

`boot_count + last_fault` — 16-byte struct + CRC, ping-pong inside one sector. `safety_task` updates `last_fault` synchronously on every fault raise (briefly stalls the safety loop, but it's the most important entry there is). Boot count increments only on cold boot.

### Wear

Worst case: 10 sessions/day × 365 days × 10 years = 36,500 writes spread across 8 MB. W25Q64 rated 100 K erase cycles per sector. Trivial margin; no FTL needed.

### First-boot sequence

1. Read JEDEC chip ID; mismatch → fail to boot.
2. Read each ping-pong region; pick newest valid CRC. Both slots invalid → write defaults to slot A.
3. Crash-loop detection: increment `boot_count`. If `now − last_boot_ts < 60 s` for 5 consecutive boots, enter safe-fail mode (refuse to charge until FC41D commands clear).

## 7. User I/O

### WS2812 strip on PA15 (TIM2_CH1)

PWM at ~800 kHz NRZ. DMA1 channel feeds `CCR1` from a frame buffer encoded as one byte per WS2812 bit (0 → ~30% duty, 1 → ~70% duty). 24 bits × N LEDs + 50 µs reset gap. Update rate ≤ 30 Hz. LED count: bench-determined during M2 (likely 4–8).

### State color/animation matrix

| EVSE state | Color | Animation |
|---|---|---|
| `BOOT_SELF_TEST` | white | sweep across strip 1×/s |
| `READY` (state A) | dim green | solid |
| `READY` (state B) | blue | solid |
| `CHARGING` (state C) | cyan | breathing 0.3 Hz, brightness ∝ active amps / advertised |
| `USER_PAUSED` | yellow | breathing 0.5 Hz |
| `COOLING_DOWN` | orange | breathing 1 Hz |
| `FAULT_LATCHED` (any) | red | flash 2 Hz |
| `FAULT_GFCI` | red | flash 5 Hz + extended buzzer pattern |
| `COMMS_DEGRADED` | base color, every 3rd LED magenta | overlay |

Brightness is gamma-corrected. Default = 60 % of max. FC41D `SET_LED_OVERRIDE` overrides base color/pattern; canonical pattern returns on next state change.

### Buzzer on PB2

Software-toggled GPIO, no PWM. `io_task` runs a small pattern engine.

| Event | Pattern |
|---|---|
| Boot self-test pass | one 100 ms beep |
| Boot self-test fail | three 100 ms beeps repeating every 2 s |
| Charging session start | one 200 ms beep |
| Charging session end (normal) | two 100 ms beeps |
| Fault raised (non-GFCI) | continuous 1 s on / 1 s off until ack |
| GFCI fault | continuous on (~5 s) then 1 s on / 1 s off |
| Button feedback | 30 ms beep |
| FC41D `BUZZER_BEEP` cmd | configurable 1–500 ms |

### Buttons

- **PC3** 3-button resistor ladder. ADC sampled at 20 Hz (part of regular scan). Top → 2.0 V, mid → 1.0 V, bot → 0 V, idle → ~3.3 V (or 0 V — circuit-dependent, bench-determine on bring-up). 3-consecutive-same-read debouncing.
- **PC9** single PCB button.

Default mappings (configurable later via FC41D):
- Top: start/resume charging (exit user-paused)
- Middle: stop charging (enter user-paused)
- Bottom: clear non-GFCI latched fault (after holding 3 s)
- PC9: boot mode selector at power-on; ignored during runtime

### Heartbeat LED on PD4

Toggled at 1 Hz by `io_task`. If `safety_task` hangs and watchdog fires, the heartbeat stops first — eyeballable diagnostic that the kernel is alive but supervisor is dead.

## 8. Watchdog

Use **IWDG** (independent, LSI-clocked at ~40 kHz) with 1.0 s timeout. LSI-clocked so it survives main-clock failures. `safety_task` is the only kicker, at the end of each 20 ms tick. If `safety_task` ever blocks > 1 s (mutex deadlock, infinite loop, stack corruption), the chip resets.

WWDG (window watchdog) is overkill at this scope and not used.

`DBGMCU` configured to halt IWDG when SWD-paused so debugger sessions don't reset the chip.

Boot reason logged: read `RCC->RSR` / `DBGMCU` at startup, persist `last_reset_cause` (POR, IWDG, software, debug, brownout). Crash-loop detection (§ 6) keys off this.

## 9. Bring-up milestones

Each milestone has a measurable success criterion. M0–M5 don't need AC power; M6 partially; M7+ need AC + a J1772 dummy or real EV.

```
M0: Toolchain bootstrap            [target: blink PD4 heartbeat at 1 Hz]
    - CMake + arm-none-eabi + vendor lib + linker script + startup
    - SWD flash via openocd works
    - vendor systick + clock tree at 120 MHz
    - SUCCESS = LED blinks, no FreeRTOS yet

M1: FreeRTOS + idle/safety tasks   [target: PD4 still blinks, from a task]
    - FreeRTOS submodule + vendor port
    - Task-driven heartbeat
    - IWDG armed, safety_task kicks
    - SUCCESS = vTaskList over an init-time UART log shows tasks

M2: GPIO + ADC scan + button       [target: button press logged via debug UART]
    - All pins configured per pin map
    - DMA scan ADC running (replicate stock 0x20002a80 layout)
    - 3-button ladder reading PC3
    - SUCCESS = pressing each button prints unique ID

M3: TIM1 CP PWM + injected ADC     [target: scope shows clean 1 kHz CP, PA4 injected sample matches]
    - TIM1_CH3 PWM at 1 kHz, configurable duty
    - TIM1 update event → ADC injected trigger
    - ISR latches CP HIGH and CP LOW samples
    - state classifier returns A/B/C/E/F per ADC band
    - SUCCESS = no vehicle → state A; 2.74 kΩ across CP↔PE → state B; 882 Ω → state C

M4: SPI3 + W25Q driver             [target: read JEDEC ID + round-trip a sector]
    - SPI3 up at 12-25 MHz
    - W25Q chip ID matches W25Q64
    - Erase + program + read 4 KB → bytes match
    - SUCCESS = round-trip a sector full of 0xA5/0x5A pattern

M5: Persistence layer              [target: boot_count increments across reboots]
    - boot_config + cal regions ping-pong
    - event_log + session_log append
    - reset reason persisted
    - SUCCESS = power cycle 5×, see boot_count = 5 in event log

M6: Safety supervisor + faults     [target: trips on simulated GFCI, weld]
    - Fault state machine + LED/buzzer integration
    - Boot self-test sequence
    - GFCI EXTI handler
    - Relay weld detection (manually short PB12 to GND with PE12 commanded open)
    - SUCCESS = each fault type can be provoked and reported correctly

M7: Relay control under load       [target: contactor closes/opens correctly per state]
    - safety_task decisions actuate PE12 + PE0
    - On AC power, with J1772 test load, close/open observed
    - SUCCESS = full A→B→C transition closes contactor; A→C transition refuses to close

M8: FC41D TLV protocol             [target: Python host can SET_ADVERTISED_AMPS, observe duty change]
    - UART5 RX/TX + framing + CRC
    - Command dispatch
    - Async events
    - SUCCESS = host script drives a full session: SET_ADVERTISED_AMPS=20, REQUEST_START, observe state events

M9: User I/O polish                [target: WS2812 patterns + buzzer match design]
    - WS2812 driver + state matrix
    - Buzzer pattern engine
    - LED override via FC41D
    - SUCCESS = all UI states visually validated

M10: Bench charging session        [target: full real-world EV charging cycle]
    - On a real EV, end-to-end session
    - Compare CT readings to clamp meter
    - Compare temperature to non-contact thermometer
    - Verify session_log entry created
```

## 10. Repo layout

```
OpenEVCharger/
├── README.md
├── LICENSE                     # GPLv3
├── CMakeLists.txt
├── docs/
│   ├── architecture.md         # this design + updates
│   ├── pin-map.md              # symlink/copy of rippleon/docs/mcu-re/pinout.md
│   ├── protocol.md             # TLV details
│   ├── safety.md               # fault taxonomy + self-test
│   ├── bring-up.md             # milestone procedures + bench observations
│   ├── attribution.md          # OpenEVSE references per module
│   └── superpowers/specs/
│       └── 2026-05-02-safety-core-v1-design.md  # this file
├── recovery/                   # stock backups (gitignored or LFS)
│   └── README.md               # restore procedure
├── src/
│   ├── main.c
│   ├── hal/                    # thin wrappers over GD32F20x_std_periph
│   │   ├── gpio.c/h
│   │   ├── adc_scan.c/h        # 11-channel DMA scan
│   │   ├── adc_injected.c/h    # CP sampling
│   │   ├── tim_pwm.c/h         # CP PWM + WS2812 PWM
│   │   ├── spi_w25q.c/h
│   │   ├── uart.c/h
│   │   ├── wdg.c/h
│   │   └── exti.c/h
│   ├── tasks/
│   │   ├── safety_task.c/h
│   │   ├── io_task.c/h
│   │   ├── comms_task.c/h
│   │   └── persist_task.c/h
│   ├── core/
│   │   ├── j1772.c/h           # state classifier + duty calc
│   │   ├── fault.c/h           # fault catalog + ack semantics
│   │   ├── self_test.c/h
│   │   ├── system_state.c/h    # mutex-guarded snapshot
│   │   └── pin_map.h           # const pin assignments — single source of truth
│   ├── ui/
│   │   ├── ws2812.c/h
│   │   ├── led_patterns.c/h
│   │   ├── buzzer.c/h
│   │   └── buttons.c/h
│   ├── proto/
│   │   ├── tlv.c/h             # frame parser/builder
│   │   ├── crc16.c/h
│   │   └── commands.c/h        # cmd dispatch
│   └── persist/
│       ├── pingpong.c/h
│       ├── event_log.c/h
│       └── session_log.c/h
├── tools/
│   ├── host_client.py          # FC41D-side reference TLV client
│   ├── flash.sh                # openocd one-shot
│   └── stock_backup.sh         # SWD dump before any flash
├── tests/                      # host-side unit tests for core/, proto/, persist/
│   └── unity/                  # test framework
├── third_party/
│   ├── FreeRTOS-Kernel/        # submodule
│   └── GD32F20x_Firmware_Library/  # vendored (vendor zip-distributes)
├── linker/
│   └── gd32f205vc.ld
└── .github/workflows/
    └── build.yml               # CI: build + size + host tests
```

## 11. Testing & validation

### Unit tests (host-built)

`j1772.c`, `fault.c`, `tlv.c`, `crc16.c`, `pingpong.c` are pure logic — no MCU peripherals. CMake target `host` compiles those translation units against a mock HAL and runs Unity tests. **Required passing for any PR touching `core/`, `proto/`, or `persist/`.** CI gates merges on this.

### HIL via SWD

A pytest harness drives `openocd` + `gdb` to halt the MCU, peek/poke RAM, inject synthetic ADC values into the safety_task input struct, and observe relay/PWM output. Same halt-poke technique already proven by `pin_probe.py`. `tools/hil/` hosts per-fault-type helpers. Expensive setup, priceless for safety regression tests.

### Bench validation

Each milestone has a SUCCESS criterion (§ 9). Document the actual scope/multimeter/clamp readings in `docs/bring-up.md` as we go. This is the documented physical-world correlation evidence.

### No fault injection on real EVs

Weld/over-current/GFCI fault tests use the HIL setup or contrived bench loads only. Never on a real vehicle.

## 12. Attribution & licensing

- `LICENSE`: full GPL-3.0-only text.
- `README.md`: "OpenEVCharger is a clean-room reimplementation of OpenEVSE-style EVSE firmware, targeted at the GigaDevice GD32F205V used in the Rippleon ROC001 / NewEnergyCS ROC-family hardware. The behavior of the J1772 state machine, fault model, and self-test sequence is modeled on OpenEVSE (https://github.com/OpenEVSE/open_evse) but no source is copied."
- `docs/attribution.md`: per-module "what we learned from upstream" notes (e.g. "j1772 state-band hysteresis values from `J1772EVSEController.cpp`").

## 13. Open items at v1 ship

These are acceptable to ship without and to address in v1.1:

- **PE0 destination unknown.** Drive it as a "may be wired to something safety-relevant" output, treat per the supervisor's relay-readback model. Update treatment if bench testing on AC reveals it's a CP-disconnect or similar.
- **PB14 high-frequency input.** Configure as input, log raw value to event log; integrate later if it correlates with anything useful.
- **PB7/PB8/PE2 board straps.** Read at boot, log to event_log, ignore until known purpose.
- **DIP2 effect.** Stock firmware sets `config_struct[0x362]` from PD12 read. We ignore DIP2 in v1; if its purpose surfaces, address in v1.1.

## 14. Pin map (canonical reference)

The full pin map lives at `rippleon/docs/mcu-re/pinout.md` and is the single source of truth. Summary of safety-critical assignments:

| Block | Pins |
|---|---|
| W25Q SPI3 | PB3 SCK, PB4 MISO, PB5 MOSI, PB6 GPIO-CS |
| FC41D control | PE1 supply-en, PD0 CEN, PD1 WAKEUP_IN, PC12/PD2 UART5 |
| J1772 CP PWM | PE13 (TIM1_CH3, full-remap) + ADCs PA4/PA7/PC5 |
| Current sense | PC0 CT902, PC1 leakage CT, PB9+PD15 gain bits to U11, PA2 AC-present |
| WS2812 LED strip | PA15 (TIM2_CH1) |
| Heartbeat LED | PD4 |
| Buzzer | PB2 |
| Relay-state SENSE | PB12 (DO NOT DRIVE) |
| GFCI CAL | PE3 (level-shifted+inverted to 5 V at GFCI side) |
| Main contactor | PE12 |
| Aux relay | PE0 |
| Buttons | PC3 (3-button ladder) + PC9 (single PCB button) |
| DIP1 (40/48 A) | PD13 |
| DIP2 (config) | PD12 |
| DIP3/4 | PD11/PD10 (read but unused in v1) |

`src/core/pin_map.h` is the firmware's single source of truth for pin assignments and must match the docs verbatim.
