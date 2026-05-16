# Pri_Comm replacement — DEFERRED (safety scope)

**Date:** 2026-05-16
**Status:** **Not replaced.** Stock `/root/Pri_Comm` continues to run.
Decision-with-rationale below; reverse-engineering of Pri_Comm
captured here to avoid losing context for whoever picks this up.

The hybrid-personality plan landed `meter` (PR #17) and `adc` (PR #18).
This third planned personality — `pricomm` — looked similar in shape
before we cracked open the binary; the static-RE pass showed it is
materially different in kind, and the safety implications of getting
it wrong push it out of "ship a v1, iterate" territory.

---

## 1. What Pri_Comm actually is

Not a polling shuttle. It's the **EVSE safety supervisor in
userland** — the layer that turns the STM32's raw status bytes into
debounced, threshold-tested alarm signals that the rest of the Linux
stack reacts to.

### Symbols (from `arm-linux-gnueabi-nm /root/Pri_Comm`):

```
Vrms, Irms                    u32 each   — meter readings, sourced from shmem
AmbTemp, InletTemp            u16 each   — temperature sensors
OTPFlag, OTP_PLUG_Cnt         flags + counter
duty_original                            — original CP duty before derating
OCP_val                       u32        — overcurrent threshold
Alarm                                    — accumulated alarm bitmap
UVPTime, UVPTime_R, OVPTime, OVPTime_R   — undervoltage / overvoltage debounce timers
OTP_Chk_Start_time, OTP_end_time         — over-temp protection timers
OTP_PLUG_end_time                        — gun-plug over-temp timer
Derating_end_time                        — when to stop derating
PollingStartTime, PollingEndTime         — frame-cycle timing
CMD_Start_time, Acktime, chktime         — request/ack timing
StartTime, EndTime                       — session lifetime
TxDataBuf                                — outbound payload (so Linux DOES send data)
```

### Alarm catalogue (from strings):

```
OVP alarm / OVP alarm recovered
OCP alarm / OCP alarm recovered
Ambient OTP alarm / Ambient OTP alarm recovered
EMGSTOP alarm / EMGSTOP alarm recovered          (emergency stop button)
RCD alarm / RCD alarm recovered                  (residual current device — GFCI)
WELDING alarm / WELDING alarm recovered          (contactor welded shut)
UVP alarm / UVP alarm recovered
RA_CPU alarm                                     (Runtime-Access CPU fault)
RA_WATCHDOG alarm                                (Runtime-Access watchdog)
RA_CLOCK alarm                                   (Runtime-Access clock)
Pilot error voltage / Pilot recover voltage
```

### Code volume:

| Function | Approx dis lines | What it does (inferred from name + .bss touch) |
|----------|-----------------:|-------------------------------------------------|
| `main`     | ~2,800           | Main loop + every alarm's state machine inline  |
| `OTPCheck` | ~545             | Over-temperature derating logic                 |
| `UartSend` | ~290             | SLIP-frame TX                                   |
| `UartRecv` | ~410             | SLIP-frame RX + parse                           |
| `DiffTimeb`| ~70              | Helper for timer arithmetic                    |
| Total      | ~4,100           |                                                 |

Plus 60+ `ftime`/`time` calls in main → heavy time-based debouncing
on every alarm.

## 2. Why this isn't a "ship v1, iterate" candidate

For meter and adc, "wrong v1" means a wrong MQTT value or a
classification difference — annoying, not unsafe. Pri_Comm's outputs
gate the contactor:

- If our v1 **misreads** the STM32 fault byte, downstream daemons
  (`main`, `Charging_Standard`, `CSR`) act on stale safety state
- If our v1 **mis-applies the OCP threshold**, the charger may
  permit current beyond what the meter says is safe
- If our v1 **doesn't process WELDING/RCD alarms**, a welded
  contactor or ground fault won't be reflected in shmem and the
  user-facing layer won't know
- If our v1 **doesn't run the OTP derating curve**, the charger
  won't throttle pilot duty as temperature rises

The STM32 is the actual hardware safety boundary — it independently
trips contactors on hard faults. But the Linux side carries
significant safety responsibility too: setpoint enforcement, alarm
debouncing, derating, and user-visible state. Pri_Comm is the seam
between the two.

## 3. What it would take to do this properly

Multi-session sub-project — not a one-pass commit. Outline:

1. **Full reverse-engineering of `main` + `OTPCheck`** with per-block
   summary of state-machine logic; cross-reference each alarm path
   against a J1772/IEC 61851 safety reference
2. **Identify the exact STM32-response payload byte layout** — which
   byte position carries which sensor reading + which fault bits
3. **Re-implement each alarm with the same threshold + debounce**
   used by stock (we cannot guess these — they MUST match)
4. **Re-implement OTP derating curve** — duty-cycle reduction as
   function of `AmbTemp`/`InletTemp` and time
5. **Implement TxDataBuf outbound** — what does Linux send to STM32
   beyond the bare polling opcodes? CSR's `fc 83` frame already
   carries pilot status; Pri_Comm may layer set-current commands.
6. **Bench validation against stock side-by-side** — run our
   replacement on /dev/ttyAMA1 while stock Pri_Comm runs on a
   second tty (e.g., a USB-UART loopback), feed both the same STM32
   responses, compare every shmem write byte-for-byte over hours
   of various fault injection scenarios
7. **240 V mains, real plug, real fault injection** — verify
   OCP/UVP/OVP/OTP all fire correctly under realistic conditions

Optimistic estimate: 5-10 focused sessions.

## 4. Recommendation

**Keep stock `/root/Pri_Comm` running.** The hybrid-personality
architecture lets us replace daemons selectively — it's specifically
designed so we can decline to replace the ones whose risk/reward
doesn't justify it. Pri_Comm is one of those.

The personality dispatch in `delta-bridge` is unchanged — adding a
`pricomm` arm later is a small mechanical addition. Nothing about
deferring it costs us future optionality.

### What to do instead (concrete suggestions for next direction):

- **More direct safety integration without replacing Pri_Comm.** We
  can READ Pri_Comm's shmem outputs (alarm bitmap, fault state) from
  delta-bridge and surface them in MQTT/web UI for better visibility
  without owning them.
- **Implement the deferred meter + adc follow-ups** from docs/16/17:
  `/root/Energy` persistence in meter, voltage-class threshold tuning
  in adc (deferred to 240 V bench session).
- **240 V bench session** to ground-truth the meter's 40-byte
  telemetry block slot labels (docs/13 §6.1) and adc's per-class
  bands (docs/17 §9).
- **Move on to entirely different work.** delta-bridge has enough
  to do its job; the stock daemons we left in place are the ones
  stock built and tested.

## 5. Note on what changed about our matrix understanding

`docs/14 §3` listed `Pri_Comm` as a "single-writer safe-replacement
zone" for `shmem[0x0a07]` and `shmem[0x0a0b]`. That's structurally
true — only Pri_Comm writes those bytes — but it misjudged the
*semantic* complexity of producing the right byte value. Future
reads of docs/14 should treat single-writer status as a necessary-
but-not-sufficient signal for replacement readiness; complexity of
the writer matters too.
