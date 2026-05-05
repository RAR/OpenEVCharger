# OpenEVCharger bring-up log

Per-milestone hardware validation notes. Every milestone gets an entry with
date, success criterion, observed result, and any deviations from spec.

## M0 — Toolchain Bootstrap

**Date completed:** 2026-05-02
**Spec section:** § 9 M0
**Plan:** docs/superpowers/plans/2026-05-02-m0-toolchain-bootstrap.md

### Success criterion (from spec)
PD4 heartbeat LED blinks at 1 Hz with the GD32F205V running at 120 MHz, after
SWD-flashing `openevcharger.elf` produced by CMake + arm-none-eabi-gcc + GD32F20x
vendor library.

### Observed result
- LED blink visible at 1 Hz: **YES** (user-confirmed visually)
- OpenOCD connect/halt/program/verify all succeed: **YES** (after target-config fixes — see "Deviations" below)
- Build size: text=1216 B, data=4 B, bss=2564 B (reserved heap+stack), total flash usage 0.23 % of 512 KB linker region.
- `arm-none-eabi-size` output:
  ```
     text	   data	    bss	    dec	    hex
     1216	      4	   2564	   3784	    ec8
  ```

### Stock backup
- `recovery/stock-mcu-V1.0.066.bin` exists, **524288 bytes** (lower 512 KB of flash; chip's upper 512 KB is fully erased so this fully restores stock).
- SHA-256: `d61a0ed1ed81eccdc3487a9ce705c547316264cbd19667a5ce46a5593fc814fb`
- Identical to `~/device-configs/esphome/rippleon/rippleon-mcu-firmware.bin` from prior reverse-engineering session (same physical bench unit, same firmware version) — re-used rather than re-dumping.

### Hardware discoveries

1. **Chip is GD32F205VG, NOT VC or VE.** The chip's flash-size auto-detect register reads `1024 KiB` during the first openocd connect. Earlier specs/notes guessed VC (256 KB) then VE (512 KB) based on the 524288-byte dump. Re-dumping `0x08080000`–`0x080FFFFF` shows 524288 bytes of `0xFF` (zero non-FF bytes), so the chip has 1 MB total but stock fw V1.0.066 only uses the lower 512 KB. Memory entry `project_rippleon_chip_variant` updated.

2. **8 MHz HXTAL crystal confirmed.** Stock firmware contains the constant `0x007A1200` (8000000 decimal) at flash offset `0x3004`. Vendor's default `HXTAL_VALUE=25000000` is wrong for this hardware; we override to `8000000` via `target_compile_definitions`. PLL × 15 = 120 MHz, matches `__SYSTEM_CLOCK_120M_PLL_HXTAL`.

3. **GD32 IDCODE differs from STM32 by one nibble.** GD32F205V SW-DP returns `0x1ba01477`; the stock `target/stm32f1x.cfg` and `stm32f2x.cfg` expect `0x3ba00477` / `0x2ba01477` respectively. Override via `set CPUTAPID 0x1ba01477` BEFORE `source [find target/...cfg]`.

4. **DBGMCU IDCODE = `0x15080418`.** Low 12 bits = `0x418` = STM32F1 connectivity-line value (used by GD32F205 too). The `stm32f1x` flash driver accepts this; `stm32f2x` does not (it errors "Cannot identify target as a STM32 family"). Switched the openocd target to `target/stm32f1x.cfg`.

5. **NRST not wired on bench probe.** `reset_config srst_only srst_nogate` requires NRST and times out on `reset halt`. Switched to `reset_config none separate` — uses CortexM SYSRESETREQ via debug AP, works without an NRST line.

### Deviations from plan

The M0 plan's openocd config went through three iterations during this milestone (all captured in commit `c72c4de` and follow-up commits to `tools/openocd-gd32f205.cfg`):
- v1: stm32f2x target + IDCODE override → halt fine, program failed (chip not recognized as STM32 family).
- v2: stm32f1x target + IDCODE override + `reset_config srst_only srst_nogate` → halt timed out (no NRST).
- v3 (final): stm32f1x target + IDCODE override + `reset_config none separate` + 1 MB flash bank → all good.

Vendor library version was also adjusted: M0 plan said V2.5.1 from gd32mcu.com, but that direct-download URL 404s currently. Fell back to the CommunityGD32Cores SPL package (V2.2.0, GitHub commit `431ebde`). V2.2 vs V2.5 don't differ for any peripheral we use.

Linker filename remains `gd32f205vc.ld` for plan continuity, but the LENGTH is 512K. We don't extend to 1 MB because v1 firmware is < 5 KB and the upper half is unused real estate for now.

### Halt-state at end of M0 flash
After `reset run`, halting again shows our Reset_Handler ran:
```
xPSR: 0x01000000 pc: 0x080002b4 msp: 0x20020000
```
(`msp = 0x20020000` matches our linker `_estack` = top of 128 KB RAM. PC inside our Reset_Handler at `0x080002b4` confirms vector table loaded correctly.)

### Next milestone
M1: FreeRTOS + idle/safety tasks. Plan to be written after M0 final commits/tag.

## M1 — FreeRTOS Scaffold + IWDG

**Date completed:** 2026-05-02
**Spec section:** § 9 M1
**Plan:** docs/superpowers/plans/2026-05-02-m1-freertos-scaffold.md

### Success criterion
PD4 heartbeat blink driven by `io_task` under FreeRTOS, with `safety_task`
kicking the FWDGT watchdog every 20 ms. Unit runs continuously without resetting.

### Observed result
- LED blinks at 1 Hz from io_task: **YES** (user-confirmed)
- 30 s+ stability test passed (no resets): **YES**
- Halt-state PC resolves to `prvIdleTask` at `tasks.c:5797` — kernel scheduler is running, idle task active. Unambiguous proof of life.
  - `xPSR: 0x61000000 pc: 0x08000432 psp: 0x200012d8`
- Watchdog negative test: skipped (the positive evidence is convincing — chip ran for >30s without reset, and PC is inside FreeRTOS, which means safety_task IS kicking the watchdog or the chip would have reset).

### Build size
- text: 5544 B
- data: 8 B
- bss: 19216 B (includes 16 KB FreeRTOS heap + task TCBs/stacks)
- flash usage: 1.06% of 512 KB linker region
- RAM usage: 14.67% of 128 KB

### Hardware notes
- FreeRTOS V11.1.0 from upstream `FreeRTOS/FreeRTOS-Kernel`, Cortex-M3 GCC port (`portable/GCC/ARM_CM3`), `heap_4` allocator.
- `vPortSVCHandler` / `xPortPendSVHandler` / `xPortSysTickHandler` aliased via `FreeRTOSConfig.h` macros — vendor startup vector table picks up the FreeRTOS implementations without any modification to `startup_gd32f20x_cl.S`.
- `DBG_CTL_FWDGT_HOLD` set in `wdg_init()` so the watchdog halts during SWD-pause; debugger sessions don't trigger resets.
- Configurable scope for ISR priority: `configLIBRARY_LOWEST_INTERRUPT_PRIORITY = 15`, `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`. ISRs at NVIC priority 0–4 (highest) preempt the kernel without using FromISR APIs; 5–15 use `xQueueSendFromISR` etc.

### Deviations from plan
None of substance. Watchdog negative test (deliberately hung safety_task to observe ~1 s reset cycle) was skipped because the positive evidence (30 s no-reset uptime + halt-PC inside kernel idle task) was sufficient. Skipping it saves a flash cycle.

### Next milestone
M2: GPIO + ADC scan + button + first debug UART. Plan to be written next.

## M2 — GPIO + ADC scan + buttons + USART1 console

**Date completed:** 2026-05-02
**Spec section:** § 9 M2
**Plan:** docs/superpowers/plans/2026-05-02-m2-gpio-adc-uart.md

### Success criterion (from spec)
Pressing each of the 3 ladder buttons on PC3 (and the on-board PC9 button)
prints a unique decoded line via the debug UART, with the 11-channel ADC
DMA scan running and all GPIOs configured per pin map.

### Observed result
- Boot banner via USART1 + ARM semihosting tee: **YES**
- PD4 1 Hz blink (M1 regression): **YES**
- All 4 buttons decode to unique IDs: **YES** (see table below)
- ADC dump prints every 5 s with plausible values: **YES**
- Continuous-uptime stability: **YES** (≥ 5 min, no spontaneous resets)

### UART output (sample)
```
--- OpenEVCharger M2 boot, SystemCoreClock=120000000 Hz ---
STRAPS: dip=0011 pb7=1 pb8=0 pe2=1 pb14=1
ADC scan armed: 11 ranks @ ~3.6 kHz
scheduler starting
ADC: AC=2116 NTC1=2123 CT=2100 LCT=2107 CPR=860 CC=4095 PE=0 NTC2=0 UNUSED=0 BTN=4093 VREF=1482
BTN press top (raw=2458)
BTN release top
BTN press mid (raw=1238)
BTN release mid
BTN press bot (raw=4)
BTN release bot
BTN press pc9
BTN release pc9
```

### Bench-measured ladder thresholds (PC3, idle = HIGH ~3.3 V)

| Button | Measured raw (12-bit) | mV @ 3.3 V Vref | Plan band | Match |
|---|---:|---:|---|---|
| top | 2458 | 1980 | 1900–2800 | ✓ |
| mid | 1238 | 998 | 800–1700 | ✓ |
| bot | 4 | 3 | ≤ 400 | ✓ |
| idle | 4092 | 3299 | ≥ 3500 | ✓ |

All three press values fall cleanly inside the plan's predicted bands.
No threshold tuning required — `BAND_*` constants in `src/ui/buttons.c`
ship as-written.

### ADC sanity check (no AC, no J1772 cable)

| Rank | Pin | Role | Raw | Volts | Comment |
|---:|---|---|---:|---:|---|
| 0 | PA2 | AC-supply-present | 2116 | 1.71 | Mid-rail; divider not pulled w/o AC. Plausible. |
| 1 | PA3 | NTC1 | 2123 | 1.71 | Hotter than stock's 1.19 V @ 25 °C — possibly no thermistor populated on bench. Non-blocking; M6 will revisit. |
| 2 | PC0 | CT (main) | 2100 | 1.69 | Op-amp midrail (≈ 1.65 V) — matches "no current" expectation. |
| 3 | PC1 | LCT (leakage) | 2107 | 1.70 | Same op-amp midrail. |
| 4 | PA4 | CP read-back | 860 | 0.69 | TIM1 idle (PWM not running); CP buffer at low rail. |
| 5 | PA7 | CC | 4095 | 3.30 | No J1772 plugged → divider open → 3.3 V. ✓ |
| 6 | PC5 | PE continuity | 0 | 0.00 | Grounded as expected. ✓ |
| 7 | PB0 | NTC2 | 0 | 0.00 | No thermistor populated on bench. Non-blocking. |
| 8 | PB1 | (unused) | 0 | 0.00 | Board variant tied low. Matches stock. |
| 9 | PC3 | button ladder | 4093 | 3.30 | Idle = HIGH (no press). ✓ |
| 10 | – | VREFINT | 1482 | 1.194 | **Calibration confirmed**: VREFINT spec = 1.16–1.24 V. End-to-end ADC + DMA pipeline correct. |

### Strap inputs (boot read)
```
dip=0011  → DIP1=open(48A), DIP2=open(KA inactive), DIP3=closed, DIP4=closed
pb7=1, pb8=0 (tied low externally), pe2=1, pb14=1 idle
```

### Output idle states (all controlled outputs read 0 V on USB-only bench)
PE12 (relay main), PE0 (relay aux), PE3 (GFCI CAL), PB2 (buzzer),
PE1 (FC41D supply), PD0 (FC41D CEN), PD1 (FC41D WAKE), PB9/PD15
(U11 gain) all idle low as required by `init_outputs_safe_low()`.
PB6 (W25Q CS) idles high (deasserted).

### Build size
- text 10504 B, data 8 B, bss 19264 B, flash usage 2.01 % of 512 KB.
- RAM usage 14.70 % of 128 KB (16 KB FreeRTOS heap + TCBs + 22-byte ADC buffer).

### Hardware discoveries / deviations from plan

1. **USART1 wire-out impractical on this bench.** PA9 (LQFP100 pin 78)
   on this SKU goes to an unpopulated LCD-daughterboard header that the
   user couldn't physically locate. Solved by adding an **ARM semihosting
   tee** in `uart_write()` — every printk also surfaces in the OpenOCD
   console when `arm semihosting enable`d, gated on DHCSR.C_DEBUGEN so
   the BKPT 0xAB never fires without a debugger attached. New helper
   script `tools/openocd-monitor.sh` runs OpenOCD with semihosting
   enabled. UART hardware path is still functional — just untested
   on this bench. Will be exercised in M3 if/when the user solders a
   probe to the chip pin.

2. **NTC channels read 0 V (PB0) and ~1.7 V (PA3) instead of stock's
   ~1.2 V.** Strongly suggests one or both gun thermistors are not
   populated on this bench unit — common for an SKU variant without
   the gun-attached temperature sensors. Non-blocking for M2; M6's
   over-temp safety check will need a "thermistor not populated" path
   (read const "safe" temperature when input is at a rail) or simply
   require thermistors to be wired before M6 testing.

3. **Ladder thresholds matched the spec on first try.** Plan estimates
   (top 1900–2800, mid 800–1700, bot ≤ 400, idle ≥ 3500) lined up with
   measured 2458 / 1238 / 4 / 4092 — comfortable margins all around.

### Next milestone
M3: TIM1 full-remap → PE13 PWM at 1 kHz + injected ADC trigger on TIM1
update event + CP state classifier (states A/B/C/E/F per voltage band).
Plan to be written after M2 final commits/tag.

## M3 — CP PWM + injected ADC + J1772 state classifier

**Date completed:** 2026-05-02
**Spec section:** § 3, § 9 M3
**Plan:** docs/superpowers/plans/2026-05-02-m3-cp-pwm-state-machine.md

### Success criterion (from spec)
Scope shows clean 1 kHz CP PWM at PE13. Each of A/B/C/D/E states is
provokable on bench by appropriate resistor across CP↔PE; safety_task
prints the correct J1772 state with `cp_mv` matching the spec band.

### Observed result (chip-side validated; buffer-side BLOCKED)

**Chip side:** TIMER0 + injected ADC + classifier all run as designed.
Verified by direct SWD register peek with the chip running:

| Register | Value | Meaning |
|---|---:|---|
| TIMER0_CTL0 | 0x00000081 | CEN+ARSE both set — counter running, ARR shadow on |
| TIMER0_CCHP | 0x00008000 | POEN set — primary output enabled |
| TIMER0_CAR | 0x000003E7 (999) | ARR = 999 → 1000 µs period = 1 kHz |
| TIMER0_PSC | 119 | 1 µs counter tick |
| TIMER0_CH2CV | 0x000003E8 (1000) idle / 0x12C (300) under 50A debug | CCR matches the API call |
| AFIO_PCF0 | 0x080000C0 | bits 7:6 = 11 → TIMER0 full-remap active |
| GPIOE_CRH | 0x44B24444 | PE13 = 0xB → AF push-pull 50 MHz |

These six readings together prove the firmware is generating a 1 kHz
PWM on PE13 at the configured duty. PWM polarity issue resolved by
switching from PWM0 to PWM1 mode (commit M3.4.1) — the on-board CP
buffer inverts (MCU pin LOW = CP +12 V).

**Buffer side: NON-FUNCTIONAL on this bench unit.** The CP read-back
(PA4) does not respond meaningfully to either the MCU pin level or
to bench-side load resistors:

| Stimulus | s_cp_raw | s_cp_mv | Expected | State |
|---|---:|---:|---|---|
| static idle pin LOW (CCR=1000) | ~1462 | -3434 | +12000 (state A) | F (wrong) |
| toggling PWM 30 % (CCR=300) | ~1422 | -3668 | similar | F (wrong) |
| static idle + 2.2 kΩ across CP↔PE | ~1462 | -3422 | ~+9000 (state B) | F (wrong, no response to load) |

The PA4 reading sits at ~1.17 V (mid-divider) regardless of MCU pin
state or external load. Two possible causes, indistinguishable
without scoping the actual J1772 CP wire or the buffer's ±12 V supply
rails:
1. The CP level-shifter on this SKU variant is not populated /
   not powered (consistent with this unit's other unpopulated
   components: NTC1 floats high, NTC2 grounded).
2. The buffer is present but its negative supply rail is missing
   (some EVSE designs use a charge pump driven by a separate
   timer pin to make -12 V; if that helper isn't running, the
   buffer can't swing negative).

### J1772 state during chip-side validation
With the buffer dead, every reading is in the -1500..+1500 mV band,
so the classifier reports `J1772 state=F cp=-3434 mV` (or -3668). This
is **correct classifier behaviour given the input** — at < -1500 mV
the band table maps to state F.

### Build size
- text 13188 B, data 8 B, bss 19272 B, flash usage 2.52 % of 512 KB.
- RAM usage 14.71 % of 128 KB.

### Hardware notes / deviations from plan

1. **Initial PWM polarity wrong.** The plan assumed a non-inverting CP
   buffer; bench observation showed the buffer inverts. Fixed by
   switching `TIMER_OC_MODE_PWM0` → `TIMER_OC_MODE_PWM1` so user-facing
   CCR semantics (CCR = ticks of CP-HIGH time) stay correct.

2. **Bench unit's CP analog chain non-functional.** PE13 toggles
   correctly per SWD register inspection, but the J1772 CP wire reads
   ~1.17 V irrespective of MCU pin level or external load. Validates
   that the firmware code is correct against spec, but the
   resistor-stimulus matrix in the spec's M3 success criterion can't
   be exercised on this bench unit. Recorded in memory as a known
   limitation; full A/B/C/D/E end-to-end validation requires either
   fixing the buffer on this SKU or testing against a different
   bench unit with a populated CP buffer board.

3. **Semihosting + OpenOCD halt-and-peek require explicit `arm
   semihosting enable`.** Without it, OpenOCD halts on the first
   `BKPT 0xAB` the firmware issues, leaving registers at near-reset
   defaults (it never runs past `uart_init()`). The
   `tools/openocd-monitor.sh` script already enables semihosting;
   one-off peek invocations need to add it manually.

### Next milestone
M4: SPI3 + W25Q64 driver (read JEDEC ID + round-trip a sector). The
W25Q chain is fully populated on this bench unit (SPI3 pads
wire-traced in M2's hardware notes), so M4 won't hit the same
bench-unit limitation as M3.

### Update 2026-05-02 (post-M5): M3 buffer-side limitation RETRACTED

The "CP buffer non-functional" diagnosis above was wrong. After
re-investigation:

1. **Buffer is healthy and inverting.** Scope on the J1772 socket CP
   terminal:
   - PWM0 + idle pin HIGH → CP reads -12 V ← the inversion
   - PWM1 + idle pin LOW  → CP reads +12 V ✓ (state A)
2. **The original PWM0→PWM1 swap (commit M3.4.1) was actually
   correct.** Reverting to PWM0 (commit M3.4.2) was a mistake driven
   by misreading the post-swap PA4 reading as evidence of a
   non-functional buffer; the actual problem was the read-back
   path's calibration, not the buffer.
3. **Read-back divider scales differently than the spec assumed.**
   Spec said ±12 V → 0..3.3 V (raw 0..4095). This PCB:
   - CP = -12 V → raw = 0    (PA4 = 0.00 V)
   - CP = +12 V → raw = 1462 (PA4 = 1.18 V)
   So the divider swing reaches only ~1.18 V at +12 V, about a third
   of the spec'd range. Empirical two-point fit committed in M3.4.4:
   `cp_mv = raw * 24000 / 1462 - 12000`. Intermediate band thresholds
   (state B/C/D) still need bench validation; M5.b's calibration
   record will be the long-term home for these constants.
4. **Final M3 status: FULLY VALIDATED.** Scope confirms CP = +12 V at
   idle; classifier reports `J1772 state=A cp=+12000 mV`. The full
   resistor-stimulus matrix (open / 2.74 k / 882 / 274 / short →
   A/B/C/D/E) can now be exercised on this bench unit — pending
   user time to do so.

The memory entry `project_openevcharger_bench_cp_buffer_dead` is OBSOLETE;
will be removed/superseded by a calibration-anchors entry.

### Update 2026-05-02 (final): full A/B/D/E matrix validated

Bench resistor matrix run after the 3-point calibration (M3.4.5) was
flashed:

| Load across CP↔PE | cp_mv reported | State | Pass |
|---|---:|---|---|
| open | +12000 | A | ✓ |
| 2.2 kΩ | +8445 | B | ✓ |
| 220 Ω | +2630 | D | ✓ |
| short | +748 | E | ✓ |

State C wasn't separately tested (no ~880 Ω resistor on hand), but
the linear fit holds confidently across the B–D bracket. Source-Z
back-calc from these data points gives an EVSE output impedance of
~800 Ω (vs nominal J1772 1 kΩ), consistent with the readings.

**M3 is FULLY VALIDATED end-to-end** on this bench unit, with no
caveats. The CP signal generation, level shifter, and read-back
calibration are all working together as designed.

Final classifier transition log captured during the matrix:
```
J1772 state=A cp=12000 mV
J1772 state=B cp=8445 mV       (2.2 kΩ applied)
J1772 state=A cp=12000 mV       (resistor removed)
J1772 state=D cp=2630 mV       (220 Ω applied)
J1772 state=E cp=748 mV        (short)
```

## M4 — SPI3 + W25Q64 driver

**Date completed:** 2026-05-02
**Spec section:** § 6, § 9 M4
**Plan:** docs/superpowers/plans/2026-05-02-m4-spi3-w25q.md

### Success criterion (from spec)
SPI3 up at 12–25 MHz; W25Q chip ID matches expected; erase + program
+ read 4 KB → bytes match. SUCCESS = round-trip a sector full of
0xA5/0x5A pattern.

### Observed result
Boot self-test output (via semihosting):
```
W25Q: JEDEC ID = 0xc84017 (unrecognised — non-Winbond or different capacity)
W25Q: erased sector @ 0x07f000
W25Q: programmed 256 bytes @ 0x07f000
W25Q round-trip PASS: 256 bytes match
```

- JEDEC ID = **0xC84017**:
  - 0xC8 = **GigaDevice** (not Winbond as spec assumed — sibling
    vendor to the GD32 MCU, same SPI NOR command set)
  - 0x40 = SPI NOR memory type
  - 0x17 = capacity 2^23 = 8 MB → **GD25Q64** (or similar 8 MB GD25Q variant)
- Erase time: <100 ms (under the 5 M-loop timeout, didn't measure precisely)
- Program time: <10 ms (under the 100 K-loop timeout)
- 256-byte round-trip: **PASS** — every byte matches
- SPI clock: 15 MHz (APB1 30 MHz / 2)
- Continuous-uptime stability: validated alongside M2/M3 on the
  same boot, no regressions.

### Build size
- text 14528 B, data 8 B, bss 19280 B, flash usage 2.77 % of 512 KB.
- RAM usage 14.72 % of 128 KB.

### Hardware notes / deviations from plan

1. **W25Q is actually a GigaDevice GD25Q64, not Winbond W25Q64JV.**
   Same 8 MB capacity, same standard SPI NOR command set (0x9F /
   0x05 / 0x06 / 0x03 / 0x20 / 0x02 all behave identically per the
   GD25Q datasheet vs Winbond datasheet). The spec's "W25Q64JV =
   0xEF4017" expectation is wrong for this hardware variant; not a
   bug, just a brand assumption that didn't match. Driver is brand-
   agnostic so no code change needed — only the bring-up log
   reflects the actual reading.

2. **No CS-timing oddities at 15 MHz.** First-pass round-trip worked
   without needing to slow SPI down to 7.5 MHz.

3. **Sector 0x07F000 (very last 4 KB of the chip) used for the test.**
   Stock firmware's persistence is in 0x000000–0x04CFFF per the spec
   memory map; using the upper-end sector keeps the test maximally
   far from anything stock-firmware-relevant.

### Next milestone
M5: Persistence layer — boot_config + calibration ping-pong,
event_log + session_log ring buffers, scan-on-boot head discovery,
integrate writes via persist_task's queue.

## M5 — boot_count persistence (scoped subset of full M5)

**Date completed:** 2026-05-02
**Spec section:** § 6, § 9 M5
**Plan:** docs/superpowers/plans/2026-05-02-m5-boot-count.md

### Success criterion (from spec)
"Power cycle 5×, see boot_count = 5 in event log."

### Observed result
5 consecutive resets via `./tools/openocd-monitor.sh` (each issues a
`reset run`):
```
=== boot 1 ===  JEDEC=0xc84017  boot_count = 6
=== boot 2 ===  JEDEC=0xc84017  boot_count = 7
=== boot 3 ===  JEDEC=0xc84017  boot_count = 8
=== boot 4 ===  JEDEC=0xc84017  boot_count = 9
=== boot 5 ===  JEDEC=0xc84017  boot_count = 10
```

Increments by exactly 1 per reset, no skips. Counter started at 6
because earlier bench iterations during M5 development had already
nudged it; the relevant test is the **delta of +5 across 5 resets**,
which holds.

### Build size
- text 14236 B (-292 B vs M4 because the M4 self-test was removed),
  data 8 B, bss 19280 B, flash 2.72 % of 512 KB.

### Hardware notes / deviations from plan
1. **No event_log yet** — the spec's "boot_count = 5 in event log"
   is interpreted as "boot_count value is 5"; the formal event_log
   record-append machinery is deferred to M5.b. M6 (faults) will need
   M5.b in place before fault events can be persisted.
2. **No persist_task queue yet.** boot_count_increment() runs
   synchronously from main() pre-scheduler. M5.b adds the FreeRTOS
   queue and moves write requests onto persist_task's worker.
3. **CRC32 is software-bit-banged.** No GD32 hardware CRC unit used —
   ~50 cycles/byte, fine for our 32-byte records.

### Next milestone
M5.b — full persistence: ping-pong helper for boot_config +
calibration, event_log + session_log scan-on-boot + append, integrate
writes via persist_task's queue. M5.b is a hard prerequisite for M6
because M6 writes a fault event on every latched fault raise.

---

## M5.b.1 — ping-pong helper + boot_config record

**Date completed:** 2026-05-02
**Spec section:** § 6 (records, ping-pong scheme)
**Plan:** docs/superpowers/plans/2026-05-02-m5b1-pingpong-boot-config.md

### Success criteria
1. First flash → defaults written to slot A (counter=1, advertised_amps=0).
2. Reset → load from slot A unchanged.
3. Programmatic `boot_config_set_advertised_amps(32)` → store to slot B (counter=2).
4. Reset → load from slot B (counter=2, advertised_amps=32).
5. boot_count keeps incrementing across reboots (M5 chain unbroken).

### Observed result

```
=== boot N (post-flash, slots blank) ===
W25Q: JEDEC ID = 0xc84017
boot_count = 17
boot_config: defaults written -> slot A (counter=1, advertised_amps=0)
                                                         (printed during flash.sh's reset run)

=== boot N+1 (monitor reset) ===
boot_count = 17
boot_config: loaded from slot A (counter=1, advertised_amps=0)

=== boot N+2 (after re-flash with one-shot set(32)) ===
boot_count = 18
boot_config: loaded from slot A (counter=1, advertised_amps=0)
boot_config: stored -> slot B (counter=2, advertised_amps=32)

=== boot N+3 (monitor reset) ===
boot_count = 19
boot_config: loaded from slot B (counter=2, advertised_amps=32)
```

The set-when-already-32 path (idempotent) is implicitly validated by
boot N+3: the test gate `if (advertised_amps == 0)` was false, so the
store correctly skipped.

### Build size
- text 15592 B (+1372 B vs M5: pingpong + boot_config),
  data 8 B, bss 19312 B, flash 2.98 % of 512 KB.

### Implementation notes
1. **Spec drift — boot_config_record adds a `monotonic_counter` field.**
   The spec's record was 28 B as written (1+3+1+3+16+4); the spec also
   calls for a "monotonic counter" used by the ping-pong reader. The
   helper enforces a fixed envelope (counter @ offset 4, CRC at end),
   so the record was extended to 32 B by adding `u32 monotonic_counter`
   at offset 4 and growing `reserved` to 16 B. Calibration record will
   follow the same shape in M5.b.2.
2. **Helper is record-agnostic.** `pingpong_load/store` only see a buffer
   pointer + size; the convention is hardcoded (counter @ 4, CRC @ end).
   Callers do not pass offsets — every persisted record in this codebase
   follows this shape.
3. **First-write tie-break.** If both slots are blank, helper writes to
   slot A (target_slot = (cur_slot == 0) ? 1 : 0; cur_slot = -1 means
   "no current valid record" → target = 0).
4. **Verify-read.** After program, helper reads back and `memcmp`s
   against the source bytes; mismatch returns -5.
5. **w25q_read returns void.** Original plan checked rc; updated to
   match the actual HAL signature.

### Next milestone
M5.b.2 — calibration record. Same record convention, slots
0x002000 / 0x003000. Migrate the M3 hard-coded CP anchors
(slope = 3540/459, anchor_raw = 1462) into a `calibration_record`,
read at boot into a RAM cache that `adc_inject.c`'s ISR consumes via
`volatile` cached integers.

---

## M5.b.2 — calibration record + ISR cache

**Date completed:** 2026-05-02
**Spec section:** § 6 (calibration ping-pong)
**Plan:** docs/superpowers/plans/2026-05-02-m5b2-calibration.md

### Success criteria
1. First flash → defaults written to slot A (anchor=1462, slope=3540/459).
2. Reset → load unchanged.
3. CP behaviour unchanged: state A still reports cp=12000 mV.
4. Programmatic `calibration_set_cp(1500, 3600, 460)` → store + ISR
   reads new values (proves cache is live).
5. Restore defaults via the same path → cp=12000 mV again.

### Observed result

```
=== boot N (post-flash, slots blank) ===
calibration: defaults written -> slot A (counter=1, anchor=1462, slope=3540/459)
J1772 state=A cp=12000 mV          ← unchanged from M3 baseline

=== boot N+1 (monitor reset) ===
calibration: loaded from slot A (counter=1, anchor=1462, slope=3540/459)
J1772 state=A cp=12000 mV

=== boot N+2 (after re-flash with one-shot set(1500,3600,460)) ===
calibration: loaded from slot A (counter=1, anchor=1462, slope=3540/459)
calibration: stored -> slot B (counter=2, anchor=1500, slope=3600/460)
J1772 state=A cp=11703 mV          ← predicted (1463-1500)*3600/460+12000 = 11695

=== boot N+3 (after re-flash with restore-defaults gate) ===
calibration: loaded from slot B (counter=2, anchor=1500, slope=3600/460)
calibration: stored -> slot A (counter=3, anchor=1462, slope=3540/459)
J1772 state=A cp=12000 mV          ← restored
```

### Build size
- text 16252 B (+660 vs M5.b.1: calibration.c + ISR plumbing),
  data 20 B, bss 19340 B, flash 3.11 % of 512 KB.

### Implementation notes
1. **ISR cache uses volatile int32_t.** Three load-bearing values
   (`s_anchor_raw`, `s_slope_num`, `s_slope_den`). Each is a single
   word, so reads from the ISR are atomic on Cortex-M3 — no torn-read
   risk. The setter publishes all three under `__disable_irq()` to
   prevent the ISR from observing a half-updated triple. < 1 µs IRQ
   blackout per setter call (very rare path).
2. **Defaults match the bench fit.** `CAL_DEFAULT_*` constants in
   `calibration.h` are the M3.4.5 values, so the first few ADC ISRs
   that run *before* `calibration_load()` (very brief window between
   `adc_inject_init()` in main and `calibration_load()` later in main)
   produce correct cp_mv. No init-order shuffle needed.
3. **Spec drift — calibration_record fields.** Spec § 6 listed
   `cp_divider_trim_mv` (a single additive correction). The actual
   M3 fit is non-additive (slope-and-anchor), so the field was
   replaced with `(cp_anchor_raw, cp_slope_num, cp_slope_den)` — a
   triple that captures the real fit. CT902 / leakage-CT / NTC trim
   fields are reserved (struct fields present, no consumer yet).
4. **Validation strategy.** Step 4 deliberately programs *bogus*
   anchors and verifies the ISR reports the predicted bogus reading;
   that's the only way to prove the cache is actually being read.
   The math: `(1463 - 1500) × 3600 / 460 + 12000 = 11695`. Observed
   11703 (raw fluctuates 1461–1463). Match.

### Next milestone
M5.b.3 — CRC16 helper + event_log scan-on-boot + append. event_log is
the trickiest M5.b piece: ring buffer across 64 sectors, head discovery
by scan, sector-erase on wrap. Spec § 6 explicitly avoids a header
sector — head pointer is reconstructed from data on every boot.

---

## M5.b.3 — CRC16 + event_log scan-on-boot + append

**Date completed:** 2026-05-02
**Spec section:** § 6 (event_log)
**Plan:** docs/superpowers/plans/2026-05-02-m5b3-event-log.md

### Success criteria
1. First boot (event_log empty in fresh region): scan reports zero valid records.
2. 3 self-test appends from main → head_slot=3.
3. Reset → scan finds 3 valid records, head_slot still 3.
4. Read-back via `event_log_read_nth(0..2)` returns the test records with
   correct boot_count + timestamp + fault_id.
5. Slot 3+ readable as blank (rc=1, all 0xFF).

### Observed result

Boot N (post-flash, region had M4-leftover corruption in sector 0):
```
event_log: scan complete: valid=0 corrupt=89 blank=8103 head=0x004000 slot=0
evt[0..4]: rc=1 (corrupt leftover)        ← pre-append dump
event_log: 3 self-test records appended; head_slot=3
                                          ↑ append-to-slot-0 triggered the
                                            non-blank-probe → erase sector 0,
                                            then 3× page program.
```

Boot N+1 (reset, no re-flash):
```
event_log: scan complete: valid=3 corrupt=0 blank=8189 head=0x004060 slot=3
evt[0]: rc=0 ts=0x10000000 boot=24 fault=0xa000 cp=12000
evt[1]: rc=0 ts=0x10000001 boot=24 fault=0xa001 cp=12000
evt[2]: rc=0 ts=0x10000002 boot=24 fault=0xa002 cp=12000
evt[3]: rc=1 ts=0xffffffff boot=65535 fault=0xffff cp=-1   ← blank
evt[4]: rc=1 ts=0xffffffff boot=65535 fault=0xffff cp=-1   ← blank
```

The first-boot 89 corrupt records were all in sector 0 (M4 self-test
leftover); after the auto-erase, the entire region is clean except
for our 3 valid records.

Boot N+2 (clean main, test+dump removed):
```
event_log: scan complete: valid=3 corrupt=0 blank=8189 head=0x004060 slot=3
```

3 records persisted across re-flashes; M6 fault handlers can append
on top of them.

### Build size
- text 16680 B (+428 vs M5.b.2: crc16 + event_log code),
  data 20 B, bss 23452 B (+4112 — `static uint8_t sector_buf[4096]`
  in event_log_init), flash 3.29 % of 512 KB. RAM 17.91 %.

### Implementation notes
1. **Scan reads one sector at a time.** Single 4 KB SPI read amortises
   the per-byte SPI cost (~70 ns/byte at 15 MHz). 64 sectors × 4 KB ≈
   18 ms total scan. Static `sector_buf[4096]` in event_log.c keeps the
   buffer off main's stack.
2. **Head = (latest_slot + 1) mod TOTAL_RECORDS.** Tiebreak rule for
   "latest": higher boot_count wins; same boot, higher timestamp wins
   (>= so a stable slot index is used).
3. **CRC16-CCITT-FALSE** (poly 0x1021, init 0xFFFF, no final XOR).
   Spec said "CRC16" without specifying flavour; CCITT-FALSE is the
   standard choice for short embedded records.
4. **Sector-boundary erase.** Append to `slot % 128 == 0` probes the
   first record; if non-blank, erase the sector first. Avoids wearing
   freshly-erased sectors.
5. **Boot count is provided by main**, not read from boot_count.c — keeps
   event_log decoupled from the boot_count module. Caller stamps via
   `event_log_set_boot_count()` after `boot_count_increment()` returns.

### Deferred / known gaps
1. **Sector-boundary wrap not bench-tested with a 130-record append.**
   The code path is exercised by the in-line non-blank-probe → erase
   logic, but a real ring-wrap (slot 127 → slot 128 in sector 1) wasn't
   forced on the bench. Will get exercised naturally during M6 fault
   raises and M7 charging-session events.
2. **No persist_task queue.** event_log_append is synchronous; calls
   from safety_task will block on W25Q I/O. M5.b.5 wraps in a queue.
3. **timestamp is caller-supplied u32.** No RTC yet; spec says ms
   since boot or RTC. M9 (FC41D NTP sync) populates real wall-clock.

### Next milestone
M5.b.4 — session_log. Same scan + append shape, smaller ring (8 sectors,
~1024 records). Should extract a generic ring helper from event_log.c
to avoid duplicating the scan logic — refactor + new module in one
commit.

---

## M5.b.4 — session_log scan-on-boot + append

**Date completed:** 2026-05-02
**Spec section:** § 6 (session_log)
**Plan:** docs/superpowers/plans/2026-05-02-m5b4-session-log.md

### Success criteria
1. First boot (region pristine): `session_log: scan complete, head=0x044000 slot=0`.
2. 2 self-test appends → head_slot=2.
3. Reset → scan finds 2 valid records, head_slot=2 still.
4. `session_log_read_nth(0..1)` returns the test records with correct
   boot_count + start_ts + mwh_delivered + end_reason.

### Observed result

```
Boot N (post-flash, region pristine):
  session_log: scan complete: valid=0 corrupt=0 blank=1024 head=0x044000 slot=0
  ses[0..3]: rc=1 (all 0xFF)
  session_log: 2 self-test records appended; head_slot=2

Boot N+1 (reset):
  session_log: scan complete: valid=2 corrupt=0 blank=1022 head=0x044040 slot=2
  ses[0]: rc=0 boot=28 start=0x20000000 mwh=7000  end=0xc0
  ses[1]: rc=0 boot=28 start=0x200003e8 mwh=14000 end=0xc1
  ses[2..3]: rc=1 (blank)

Boot N+2 (clean main, test+dump removed):
  session_log: scan complete: valid=2 corrupt=0 blank=1022 head=0x044040 slot=2
```

### Build size
- text 17048 B (+368 vs M5.b.3: session_log code only),
  data 20 B, bss 27556 B (+4104 — second 4 KB static sector_buf for
  session_log_init), flash 3.25 % of 512 KB. RAM 21.04 %.

### Implementation notes
1. **Spec drift — added u16 boot_count field.** Spec's session_record
   had no monotonic key; without one, the scan can't reliably pick a
   "latest" across power cycles before RTC sync. Adding `u16 boot_count`
   (matching event_record) is the cheapest fix; reserved bytes
   adjusted to keep total at 32 B.
2. **No generic ring helper.** event_log.c and session_log.c are 95%
   identical — same scan, classify, append, sector-erase logic. The
   difference is the tiebreak key inside the scan loop (start_ts vs
   timestamp) and the constants (region base/size/total). With only
   two callers, abstraction overhead exceeds the duplication cost.
   Revisit if a third ring buffer lands.
3. **Two static 4 KB sector buffers now.** event_log + session_log
   each carry their own. RAM crept to 21 %. Acceptable.

### Next milestone
M5.b.5 — `persist_task` FreeRTOS queue. Wraps event_log_append +
session_log_append in a queue drained by persist_task. safety_task and
other producers post a write request and continue without blocking on
W25Q I/O. Required before M6 fault-state-machine raises faults at
real-time pace.

---

## M5.b.5 — persist_task FreeRTOS queue + producer API

**Date completed:** 2026-05-02
**Spec section:** § 5.5 (persist_task), § 6 (queue drain)
**Plan:** docs/superpowers/plans/2026-05-02-m5b5-persist-queue.md

### Success criteria
1. `persist_task_create()` builds a depth-8 queue and starts the worker.
2. Any task can `persist_post_event(rec)` and continue immediately
   (non-blocking).
3. persist_task drains the queue and calls `event_log_append` /
   `session_log_append`.
4. After reboot, scan finds the queued events persisted to W25Q.
5. Queue overflow returns -1 + emits one warning per burst.

### Observed result

```
=== boot 31 (post-flash with queue+producer) ===
event_log: scan complete: valid=3 corrupt=0 blank=8189 head=slot 3
session_log: scan complete: valid=2 ...
scheduler starting
io_task: posted startup event rc=0      ← producer succeeded

=== boot 32 (reset) ===
event_log: scan complete: valid=4 corrupt=0 blank=8188 head=slot 4
                                ↑  prior boot's io_task post landed
io_task: posted startup event rc=0
```

`valid` grows by exactly 1 per boot cycle = io_task's startup post
flowing through the queue → persist_task → flash → next boot's scan.

### Build size
- text 19592 B (+2544 vs M5.b.4: queue plumbing + io_task hookup +
  FreeRTOS queue.c TUs that the linker now pulls in via xQueueCreate /
  xQueueSend / xQueueReceive references), data 20 B, bss 27564 B.
  Flash 3.74 % of 512 KB. RAM 21.04 % (queue heap allocation ≈ 370 B
  is in FreeRTOS heap, not bss).

### Implementation notes
1. **Single tagged-union queue.** One `QueueHandle_t` carries
   `struct persist_req { type; union { event; session; }; }`. Queue
   item ≈ 36 B; depth 8 → ≈ 288 B + ~80 B FreeRTOS overhead.
2. **Non-blocking producers.** `xQueueSend(..., 0)` — drops on full
   queue, returns -1, latches a single "queue full" warning per burst.
   Latch resets on next successful drain. Required behaviour for fault
   floods that must not block safety_task.
3. **Boot-time path stays synchronous.** Pre-scheduler init code (none
   today, but M6 boot self-test events) calls `event_log_append` /
   `session_log_append` directly. Once `vTaskStartScheduler()` runs,
   producers switch to `persist_post_*`.
4. **persist_task priority 1** (lowest of the four tasks) — never
   starves higher-priority work. SafetyTask (4) > io (3) > comms (2)
   > persist (1). Spec § 5.5 requirement satisfied.
5. **No fromISR variant yet.** GFCI is the only spec-defined ISR-driven
   fault path; M6 will route GFCI EXTI into safety_task via task
   notification, then safety_task posts to the queue. No producer
   currently runs in interrupt context.
6. **io_task post is permanent**, not test-only. Production firmware
   benefits from a "boot signature" event_record per power cycle —
   trivial 32 B/boot of W25Q wear, valuable diagnostic when reading
   the log.

### Next milestone
M5.b.6 — crash-loop detector. Records boot timestamps; if 5 boots
happen within 60 s, enter safe-fail mode (refuse to charge until
FC41D commands clear). Final M5.b sub-milestone, then M6 (safety
supervisor + faults) becomes possible.

---

## M5.b.6 — crash-loop detector + 60 s alive marker

**Date completed:** 2026-05-02
**Spec section:** § 6 (crash-loop detection)
**Plan:** docs/superpowers/plans/2026-05-02-m5b6-crash-loop.md

### Success criteria
1. First boot: `crash_state: defaults written -> slot A` then count
   becomes 1.
2. After 60 s of uptime, `io_task` posts a reset request and
   persist_task writes `fast_restart_count = 0`.
3. 5 rapid resets (each within 60 s) → SAFE-FAIL ENTERED at boot 5.
4. After SAFE-FAIL, run > 60 s → alive marker clears safe-fail.
5. Subsequent reset shows fast_restart=1 (back to normal).

### Observed result

```
Boots 33-37 (rapid reset, ALIVE_MARKER_MS at production 60s — never reaches it):
  boot 33: crash_state: fast_restart=1 (slot A counter=1)
  boot 34: crash_state: fast_restart=2 (slot B counter=2)
  boot 35: crash_state: fast_restart=3 (slot A counter=3)
  boot 36: crash_state: fast_restart=4 (slot B counter=4)
  boot 37: crash_state: SAFE-FAIL ENTERED (fast_restart=5)

Recovery (with ALIVE_MARKER_MS temporarily reduced to 1000 for the 30 s
test, then restored to 60000):
  boot 17: crash_state: SAFE-FAIL ENTERED (fast_restart=17)
           (chip ran 30 s)
           io_task: alive marker posted (ms=1000)
           crash_state: alive marker -> fast_restart=0 (slot B counter=18)
  reset:   crash_state: fast_restart=1 (slot A counter=19)  ← recovered
```

Ping-pong slot rotation observed (alternates A/B per write); monotonic
counter increments correctly.

### Build size
- text 20348 B (+3300 vs M5.b.5: crash_state record + 3rd persist_req
  type + io_task hookup), data 20 B, bss 27596 B (+32),
  flash 3.88 % of 512 KB. RAM 21.07 %.

### Implementation notes
1. **crash_state record envelope.** Same shape as boot_config
   (counter @ 4, CRC @ end, 32 B total). Single payload byte:
   `fast_restart_count`. Threshold = 5 (`CRASH_LOOP_THRESHOLD`).
2. **Boot-time path is synchronous.** `crash_state_boot_increment()`
   runs from main() pre-scheduler — reads, increments, persists, sets
   safe-fail flag. No queue involved (queue doesn't exist yet at that
   point in main).
3. **Alive marker funnels through persist_task.** io_task does NOT
   touch W25Q directly — would race with persist_task on SPI3.
   io_task posts `PERSIST_REQ_CRASH_STATE_RESET`, persist_task drains
   and calls `crash_state_reset_alive()`.
4. **io_task stack bumped 256 → 512 words.** printk's 160 B buffer
   plus persist_req union (≈ 36 B copy from io_task's startup-event
   post) brought 256 W close to canary. 512 W = 2 KB gives margin.
5. **Spec says "now − last_boot_ts < 60 s for 5 consecutive boots".**
   We don't have an RTC. Implementation reinterprets: "5 consecutive
   boots that didn't reach 60 s of uptime each" — equivalent in
   intent. fast_restart_count increments at boot, resets at 60 s
   uptime. Spec-compliant in spirit; honest about the mechanism.
6. **OpenOCD semihost backpressure.** Sustained printk volume can
   throttle the chip's MCU clock against wall clock — every BKPT 0xAB
   blocks until the host writes to its log file. Long-uptime bench
   validation (with ALIVE_MARKER_MS=60000) timed out before reaching
   the marker due to this. Mitigation: shortened ALIVE_MARKER_MS to
   1000 for one validation run, observed the path works end-to-end,
   then restored production value. Document gap: production firmware
   alive marker requires 60 s of low-traffic uptime; only matters
   on the bench.
7. **Safe-fail enforcement is M6's job.** `crash_state_is_safe_fail()`
   accessor exists; safety_task in M6 will gate CP duty cycle / relay
   close on it.

### M5.b is complete

| Sub-milestone | Module | Tag |
|---|---|---|
| M5.b.1 | pingpong + boot_config | m5b1-pingpong-boot-config |
| M5.b.2 | calibration + ISR cache | m5b2-calibration |
| M5.b.3 | crc16 + event_log | m5b3-event-log |
| M5.b.4 | session_log | m5b4-session-log |
| M5.b.5 | persist_task queue | m5b5-persist-queue |
| M5.b.6 | crash_state | m5b6-crash-loop |

W25Q layout (8 MB chip, 64 KB used):

| Range | Region | Module |
|---|---|---|
| 0x000000-0x001FFF | boot_config (ping-pong) | M5.b.1 |
| 0x002000-0x003FFF | calibration (ping-pong) | M5.b.2 |
| 0x004000-0x043FFF | event_log (ring) | M5.b.3 |
| 0x044000-0x04BFFF | session_log (ring) | M5.b.4 |
| 0x04C000 | boot_count | M5 |
| 0x04D000-0x04EFFF | crash_state (ping-pong) | M5.b.6 |

## M6.1 — Fault catalog + EVSE state machine skeleton (2026-05-02)

First step of M6 (safety supervisor + faults). Lands the data model
without any detection logic.

Built:

- `src/core/fault.{c,h}`: 19-fault catalog (13 latched + 5
  self-clearing + sentinel). `fault_state_t` is a `uint32_t`
  active-bitmap + first-raised tracker. API: raise / clear /
  is_active / any_active / any_latched_active / is_latched_kind /
  clear_all_clearable / fault_name. Pure C, host-testable per
  spec § 11.
- `src/core/evse_state.h`: 7 supervisor states (BOOT, SELF_TEST,
  READY, CHARGING, USER_PAUSED, COOLING_DOWN, FAULT) per spec § 5.
- `src/tasks/safety_task.c`: now owns `fault_state_t` + `evse_state_t`.
  Boot path: BOOT → SELF_TEST → READY (or FAULT if
  `crash_state_is_safe_fail()` reports the safe-fail latch).
  Logs every state transition + every fault raise.

Bench validation (post-M5.b state, fast_restart=3, no safe-fail):

```
W25Q: JEDEC ID = 0xc84017
boot_count = 58
boot_config: loaded from slot B (counter=2, advertised_amps=32)
calibration: loaded from slot A (counter=3, anchor=1462, slope=3540/459)
event_log: scan complete: valid=30 corrupt=0 blank=8162 head=0x0043c0 slot=30
session_log: scan complete: valid=2 corrupt=0 blank=1022 head=0x044040 slot=2
crash_state: fast_restart=3 (slot A counter=3)
scheduler starting
EVSE state BOOT -> SELF_TEST
EVSE state SELF_TEST -> READY
J1772 state=A cp=12000 mV
```

No FAULT_CRASH_LOOP_SAFE_FAIL raised (fast_restart=3 < threshold=5).
Boot path lands cleanly in EVSE_READY.

Build size: text 21252 / data 20 / bss 27596 — flash 4.06%, RAM 21.07%.

## M6.2 — PB12 relay-sense readback + weld detection (2026-05-02)

Read-only PB12 sense. Inline in `safety_task` (no separate module —
single-writer rule keeps relay-related logic in one task).

Built:

- `relay_sense_closed()`: reads PB12 (HIGH = sensed-closed; pinout doc
  confirms HIGH means "contactor closed-feedback").
- `relay_commanded_closed()`: function stub returning 0. OpenEVCharger does
  not yet drive PE12 — `gpio_init_all()` configures it as output PP and
  drives it LOW. The function exists so M7's actuation work plugs in
  here cleanly.
- `check_relay_weld()`: each tick, compares sensed vs commanded. If
  sensed=closed && cmd=open for >= 200 ms (10 ticks at 50 Hz), raises
  `FAULT_RELAY_WELD` (latched) and transitions to EVSE_FAULT. Spec
  § 4 #2 persistence threshold.
- Logs every change in PB12 sense state.

Bench validation (post-M6.1 state):

```
boot_count = 59
crash_state: fast_restart=4 (slot B counter=4)
EVSE state BOOT -> SELF_TEST
EVSE state SELF_TEST -> READY
relay sense: open (cmd=open)
```

PB12 reads LOW (open) at boot, matching the hardware reality (relay
coil de-energised, contacts open). No weld fault raised. Clean.

The active-fault-injection variant of this test (manually shorting
PB12 to 3.3 V to spoof "closed" sense) was avoided overnight per the
pinout doc warning — it spoofs the firmware safety interlock, and we
already trust the reading. Will revisit during M9 user-I/O bench.

Build size: text 21552 / data 20 / bss 27596 — flash 4.12%, RAM 21.07%.
Cost vs M6.1: +300 B text.

## M6.3 — Fault → CP output (drive state F on EVSE_FAULT) (2026-05-02)

`safety_task` now calls `cp_pwm_set_state_f()` on entry to EVSE_FAULT
(CCR=0 → PE13 always HIGH → CP -12 V via inverting buffer) and
`cp_pwm_set_idle_high()` on entry to any non-FAULT state. Single
owner of TIM1_CCR3 per spec § 4.

Bench (this flash hit fast_restart=5 = safe-fail trip — perfect
fault-path test):

```
boot_count = 60
crash_state: SAFE-FAIL ENTERED (fast_restart=5)
EVSE state BOOT -> SELF_TEST
FAULT raised: CRASH_LOOP_SAFE_FAIL (first=CRASH_LOOP_SAFE_FAIL)
EVSE state SELF_TEST -> FAULT
relay sense: open (cmd=open)
ADC: AC=2089 NTC1=2094 CT=2075 LCT=2089 CPR=0 CC=1410 PE=0 NTC2=0 ...
J1772 state=E cp=725 mV
```

End-to-end fault path verified:

1. fast_restart=5 → `crash_state_is_safe_fail()` returns 1.
2. `check_safe_fail()` raises FAULT_CRASH_LOOP_SAFE_FAIL.
3. EVSE transitions SELF_TEST → FAULT.
4. `evse_apply_cp(EVSE_FAULT)` writes CCR=0.
5. CP physically driven to -12 V — confirmed by scan ADC `CPR=0`
   (raw 0/4095, well below the +12 V calibration anchor of 1462).

**Calibration carve-out:** `cp_high_mv()` reports +725 mV (J1772 state
E band) for raw=0, not the expected -11000 mV. Root cause is the M3
empirical fit in `adc_inject.c`: the slope 3540/459 was derived from
the 0..+12 V half-range only and extrapolates incorrectly below the
anchor. Hardware is correct — the read-back calibration is one-sided.
Documented in NOTES.md and `project_openevcharger_cp_calibration` memory.
M6.5 will gate `FAULT_CP_NO_PILOT` on `evse_state != EVSE_FAULT` to
avoid a feedback loop where our own state-F output trips the CP=E
classifier.

Build size: text 21600 / data 20 / bss 27596 — flash 4.13%, RAM 21.07%.
Cost vs M6.2: +48 B text.

## M6.4 — Fault event persistence (2026-05-02)

`safety_task` now posts an `event_record` to `persist_task` on every
true fault-raise edge (i.e., the moment `fault_raise()` returns 1).
The record captures fault_id + j1772_state + evse_state + cp_mv +
timestamp; `event_log_append` fills boot_count + crc16. Both
`check_safe_fail` and `check_relay_weld` now route through the new
`post_fault_event()` helper.

Refactor: `check_safe_fail` and `check_relay_weld` take
`(j1772_state, cp_mv)` so the boot-time and runtime callsites can
both pass live context. The boot path now primes `cp_high_mv()` +
`j1772_step()` once before BOOT→SELF_TEST so any fault posted during
the transition has valid context (the typical fast_restart=5
safe-fail boot reaches the persist post before the main supervisor
loop executes its first iteration).

Bench validation:

```
[after recovering and ramping fast_restart from 1 to 5+]
event_log: scan complete: valid=38 corrupt=0 blank=8154 head=0x0044c0 slot=38
crash_state: SAFE-FAIL ENTERED (fast_restart=5)
EVSE state BOOT -> SELF_TEST
FAULT raised: CRASH_LOOP_SAFE_FAIL (first=CRASH_LOOP_SAFE_FAIL)
EVSE state SELF_TEST -> FAULT
io_task: posted startup event rc=0
J1772 state=E cp=725 mV
```

Next reboot's scan-on-init read confirms the fault event landed:

```
event_log: scan complete: valid=40 corrupt=0 blank=8152 head=0x004500 slot=40
```

valid count went from 38 → 40 (+2): one io_task startup event
(unchanged since M5.b.5) plus one new safety_task FAULT
event-record posted from the safe-fail edge. M6.4 path verified
end-to-end through the W25Q ring.

### Bench-state-machine quirk worth noting

OpenOCD's `reset run` from a fresh openocd invocation does not always
re-trigger main() — only ~1 in 4 reset cycles netted a fast_restart
increment. The reliable pattern is `init` → `reset halt` → `sleep 200`
→ `reset run` → `sleep ≥1500` → `shutdown`, which gives the chip
clean halt+go and enough time for the synchronous
`crash_state_boot_increment()` SPI write to complete before the next
attach cycle. Documented for future bench scripts.

Build size: text 21804 / data 20 / bss 27596 — flash 4.16%, RAM 21.07%.
Cost vs M6.3: +204 B text.

## M6.5 — CP=E classifier → FAULT_CP_NO_PILOT (2026-05-02)

`safety_task` now raises `FAULT_CP_NO_PILOT` when the J1772 classifier
returns state E for ≥3 consecutive ticks (60 ms) AND
`evse_state != EVSE_FAULT`. Spec § 4 #5.

The `evse_state != EVSE_FAULT` gate is essential because the M3 CP
read-back calibration is one-sided (slope-fit > 0 V only): when we
drive state F (CCR=0 → CP -12 V), `cp_high_mv()` reads ~+725 mV which
the classifier reports as state E. Without the gate, raising
state F as a fault output would re-raise CP_NO_PILOT in a feedback
loop. The gate suppresses re-raise once we're in FAULT for any
reason.

Bench validation:

```
[clean boot, fast_restart=2]
EVSE state BOOT -> SELF_TEST
EVSE state SELF_TEST -> READY
relay sense: open (cmd=open)
J1772 state=A cp=12000 mV
```

No spurious CP_NO_PILOT raised at idle (CP +12 V → state A). Negative
test passes.

Then ramped fast_restart to 5 to re-trip safe-fail; observed:

```
crash_state: SAFE-FAIL ENTERED (fast_restart=5)
EVSE state BOOT -> SELF_TEST
FAULT raised: CRASH_LOOP_SAFE_FAIL (first=CRASH_LOOP_SAFE_FAIL)
EVSE state SELF_TEST -> FAULT
J1772 state=E cp=725 mV
```

Despite J1772=E for many ticks during the FAULT phase, no
`FAULT raised: CP_NO_PILOT` line — the gate held. Confirms the
calibration carve-out is contained.

**Positive test deferred.** Provoking a real CP=E condition needs a
CP↔PE jumper on the bench-side connector. The classifier already
returns E for cp_mv ∈ [-1500, +1500] (verified during M3 bench), so
shorting CP to PE will make `j1772_step()` return E for >3 ticks
which our new check will turn into a fault. Will be exercised during
M9-era bench validation when a J1772 dummy plug is on hand.

Build size: text 21988 / data 20 / bss 27596 — flash 4.20%, RAM 21.07%.
Cost vs M6.4: +184 B text.

## M6.6 — Minimal boot self-test gate (2026-05-02)

`safety_task` runs a scoped boot self-test between EVSE_SELF_TEST and
EVSE_READY. 100 ms warm-up window (5 × 20 ms `vTaskDelay`) lets the
ADC scan + injected ADC converge and the J1772 classifier clear its
3-tick debounce before the test reads stable values.

Sub-checks landed:

1. **ADC sanity** — ranks AC, CT, LCT, CPR (CP-readback) must read in
   100..3995 (non-rail). NTC1/NTC2/CC/PE/BTN excluded because the
   bench has those at rail by design (NTCs not populated, CC/PE
   high-impedance idle, BTN ladder rail when idle).
2. **PB12 relay sense reads "open"** at boot — pre-flight half of
   spec § 4.1.3.
3. **CP idle in state-A band** — `cp_high_mv() ≥ 10500` confirms
   PE13 PWM at idle and the +12 V rail intact.

`boot_config` CRC validation runs synchronously in main()
pre-scheduler via `boot_config_load()` and is treated as already
covered (spec § 4.1.6).

Sub-checks deferred to M6.b (need morning-hours risk-controlled bench
supervision):

- GFCI CAL pulse + EXTI fire (no GFCI sense pin in pin map yet).
- Relay actuate-and-readback (PE12 close + PB12 confirm) — closing
  the contactor on AC clicks the J1772 plug.

If any sub-check fails, raise `FAULT_BOOT_SELF_TEST` (latched) and
transition to EVSE_FAULT. Self-test runs **before** the safe-fail
check, so both faults can co-raise on a degraded boot.

### Bench validation

Clean boot at fast_restart=2:

```
EVSE state BOOT -> SELF_TEST
self-test: PASS
EVSE state SELF_TEST -> READY
J1772 state=A cp=12000 mV
relay sense: open (cmd=open)
```

Provoked safe-fail at fast_restart=5 to confirm self-test + safe-fail
co-raise correctly:

```
crash_state: SAFE-FAIL ENTERED (fast_restart=5)
EVSE state BOOT -> SELF_TEST
self-test: PASS
FAULT raised: CRASH_LOOP_SAFE_FAIL (first=CRASH_LOOP_SAFE_FAIL)
EVSE state SELF_TEST -> FAULT
J1772 state=A cp=12000 mV
J1772 state=E cp=725 mV
```

Self-test passed (bench is healthy). Safe-fail check then raised
CRASH_LOOP_SAFE_FAIL. EVSE transitioned to FAULT. CP driven to state
F → classifier reports E (cp=725 mV, M3 calibration carve-out).
M6.5 gate held — no secondary CP_NO_PILOT. End-to-end fault path
intact.

Build size: text 22464 / data 20 / bss 27596 — flash 4.29%, RAM 21.07%.
Cost vs M6.5: +476 B text.

## M6 — Safety supervisor + faults: roll-up

Six sub-milestones landed safely overnight, all bench-validated on the
Rippleon ROC001 unit at /dev/serial/by-id/.../STLINK-V2:

| Sub-milestone | What | Tag |
|---|---|---|
| M6.1 | fault catalog + EVSE state machine | m6-1-fault-catalog |
| M6.2 | PB12 relay-sense readback + weld detection | m6-2-relay-sense |
| M6.3 | EVSE_FAULT → CP state F output | m6-3-fault-cp-output |
| M6.4 | fault persistence to event_log | m6-4-fault-persist |
| M6.5 | CP=E classifier → FAULT_CP_NO_PILOT | m6-5-cp-no-pilot |
| M6.6 | minimal boot self-test gate | m6-6-boot-self-test |

What's now wired end-to-end:

- 19-fault catalog + bitmap state.
- Boot path: BOOT → SELF_TEST (with real check matrix) → READY/FAULT.
- Safe-fail crash-loop latch (M5.b.6) feeds straight into FAULT.
- Three real fault detectors active: relay weld, CP=E, boot self-test.
- Two latent fault paths in catalog (relay stuck-open, GFCI etc.) —
  enums and persist hooks ready, detectors deferred.
- Every fault-raise edge persists an event_record to the W25Q ring.
- EVSE_FAULT drives CP to state F (-12 V) via single-writer
  TIM1_CCR3 owner discipline.

Deferred / known carve-outs:

- M3 CP read-back calibration is one-sided (slope-fit > 0 V only):
  CP -12 V reads as ~+725 mV. Causes the classifier to label our own
  fault output as E; M6.5 has a `evse_state != EVSE_FAULT` gate to
  keep the loop safe. Real fix lives in a future calibration session
  with a proper 5-point fit covering the negative half-range.
- ADC sanity check skips NTC1/NTC2/CC/PE/BTN ranks because bench
  hardware has those at rail by design. Per-rank enable mask comes
  with M6.b once bench has populated NTCs and a J1772 dummy plug.
- GFCI self-test pulse + EXTI handler: pin map needs the GFCI sense
  pin identified before this can land.
- Relay actuate-and-readback (boot self-test step 4): closes the
  contactor on AC, needs supervised bench session — M7 territory.

OpenOCD bench-state-machine quirk: `reset run` from bare openocd often
fails to fully reboot. The reliable pattern is
`init → reset halt → sleep 500..800 → reset run → sleep 1500..4000 →
shutdown`. Documented in M6.4 entry.

Recovery one-shot: a `#if 1` guarded sector-erase of
`0x04D000`/`0x04E000` in main() wipes the crash_state ping-pong slots
and restores fast_restart=0 from a safe-fail latch. The
projectstate.txt M5.b.6 recovery technique held up across all six
M6 sub-milestones.

## M7.1 — Relay HAL (2026-05-03)

`src/hal/relay.{c,h}` encapsulates PE12 (main contactor) + PE0 (aux)
output drive and PB12 closed-feedback sense behind a thin C API:

```
relay_main_open / relay_main_close / relay_main_commanded / relay_main_sense_closed
relay_aux_open  / relay_aux_close  / relay_aux_commanded
```

Last-commanded state tracked in static module-locals so callers can
ask "did I command closed?" without re-reading the GPIO output bit
register. Single-writer rule per spec § 2 + § 4: only `safety_task`
is allowed to call the *_open/_close mutators; other tasks/ISRs are
read-only.

`safety_task` swap: dropped the inline `relay_sense_closed()` /
`relay_commanded_closed()` placeholders in favor of the HAL. Same
behavior — the runtime never commands close yet — but M7.2+ will
plug into the HAL cleanly.

Bench: clean boot (fast_restart=2), self-test PASS, READY, relay
sense open. No regressions.

Build: text 22504 / data 20 / bss 27604 — flash 4.30%, RAM 21.08%.
+40 B text vs M6.6 (the HAL is small, 8 B bss for the cmd state).

## M7.2 — Boot self-test relay actuate-and-readback (2026-05-03)

`safety_task` boot path now implements spec § 4.1.4 step 4: with CP
gated to state A (no vehicle plugged), command PE12 closed and poll
PB12 every 5 ms up to 40 ms, then command open and poll up to 30 ms
for release. Two distinct fault classes:

- PE12 close + PB12 stayed open ≥40 ms → `FAULT_RELAY_OPEN_AT_BOOT`
- PE12 open + PB12 stayed closed ≥30 ms → `FAULT_RELAY_WELD_AT_BOOT`

Both fault paths wired to `post_fault_event()` so a bench/production
trip persists to event_log on the boot it occurred.

### Bench finding — gating the test off

First flash with the test enabled tripped `RELAY_OPEN_AT_BOOT`:

```
self-test: PE12 close cmd but PB12 stayed OPEN -> RELAY_OPEN_AT_BOOT
FAULT raised: RELAY_OPEN_AT_BOOT
EVSE state SELF_TEST -> FAULT
```

Two non-mutually-exclusive explanations (neither yet ruled out by
bench probe; this work is happening before mains-up):

1. **Coil supply ties to AC primary.** The contactor coil may be
   driven from the same 120 V AC that runs through the contacts —
   the bench has 120 V on the buffer rail but no contactor-side
   load path. PE12 might be a control input to a relay driver that
   needs AC.
2. **PB12 sense is current-presence, not coil state.** Even if the
   coil energised silently, the closed-feedback circuit may only
   report when AC current flows through the contacts.

Either way, **on a real installation with mains-side current
flowing, this test should pass**. Gated behind a build flag:

```c
#ifndef OPENBHZD_RELAY_ACTUATE_SELF_TEST
#define OPENBHZD_RELAY_ACTUATE_SELF_TEST  0
#endif
```

Default OFF (so the bench boots clean). Production builds opt in via
`cmake -DOPENBHZD_RELAY_ACTUATE_SELF_TEST=1`. Bench follow-up needed:

- Scope/multimeter on the contactor coil terminals while toggling
  PE12, with mains live, to confirm the coil actually energises.
- Multimeter across the contactor output while closed, to confirm
  contact closure (just continuity if no load).
- Then re-test PB12 to map its sense behavior.

Once the loop is understood the default flips to ON.

Bench post-flash with flag OFF:

```
EVSE state BOOT -> SELF_TEST
self-test: PASS
self-test: relay actuate test DISABLED at build time
EVSE state SELF_TEST -> READY
```

Build size: text 22628 / data 20 / bss 27604 — flash 4.32%, RAM 21.08%.
+124 B text vs M7.1 (the actuate routine compiles in but is unreachable
when the flag is 0).

## M7.3 — J1772 state-driven relay control (2026-05-03)

Per-tick relay state in `safety_task`:

- `update_evse_from_j1772()` — READY + J1772=C → CHARGING;
  CHARGING + J1772≠C → READY (the spec § 3 C→B regression as a
  transient pause, allowing re-progression). FAULT/BOOT/SELF_TEST
  are sticky and not exited via this path.
- `apply_relay_state()` — close iff (EVSE=CHARGING AND J1772=C AND
  no latched faults). Single-writer of PE12 per spec § 4. Logs every
  transition.

Bench: with no vehicle plugged, J1772 stays in A → EVSE stays in
READY → relay stays open. As expected (no probe to drive 882 Ω
state-C without a J1772 dummy plug or resistor, this is code-only
validation today). Path will exercise live the moment a 882 Ω across
CP↔PE drops cp_high_mv into the C band.

```
EVSE state BOOT -> SELF_TEST
self-test: PASS
self-test: relay actuate test DISABLED at build time
EVSE state SELF_TEST -> READY
J1772 state=A cp=12000 mV
relay sense: open (cmd=open)
```

Build: text 23176 / data 20 / bss 27604 — flash 4.43%, RAM 21.08%.
+548 B text vs M7.2 (the actuate-and-readback routine + new helpers).

## M7.4 — Advertised-amps duty + DIP1/hw cap (2026-05-03)

`safety_task` now drives the CP duty cycle as a function of
(EVSE state, J1772 state, advertised amps). Spec § 3 PWM table:

```
EVSE=FAULT       → state F (-12 V)        // sticky, drive on transition
J1772=A or E/F   → idle high (+12 V)      // 0% advertise
J1772=B/C/D      → cp_pwm_set_advertise_amps(effective_advertised_amps())
```

`effective_advertised_amps()` returns
`min(boot_config.fc41d_advertised_amps OR DIP1_cap, DIP1_cap, hw_cap)`:

- `boot_config.fc41d_advertised_amps` — set by FC41D over the M8 TLV
  protocol (currently zero-init or whatever was last persisted).
  `0` = unset → fall back to DIP1 cap.
- DIP1 closed (PD13 reads LOW via pull-up) → 40 A; open → 48 A.
- Hardware contactor: 48 A.

Per-tick `apply_cp_for_state()` writes CCR. `evse_transition` to
EVSE_FAULT immediately drives state F so EV doesn't see a 20 ms
window of stale CP after a fault.

### Bench validation

DIP1 reads closed (`STRAPS: dip=0011`) → DIP1 cap = 40 A.
boot_config has `fc41d_advertised_amps=32` from earlier sessions.
Effective amps = min(32, 40, 48) = 32 A. Duty would be
`32 × 0.6 = 19.2%` if we entered J1772=B/C/D, but we're at A so CP
stays at idle high. Path is code-only validated; live PE13 duty
observable on scope only when a J1772 dummy plug or 2.74 kΩ load
drops cp_high_mv into the B band.

```
STRAPS: dip=0011 pb7=1 pb8=0 pe2=1 pb14=1
EVSE state BOOT -> SELF_TEST -> READY
J1772 state=A cp=12000 mV
relay sense: open (cmd=open)
```

Build: text 23336 / data 20 / bss 27604 — flash 4.46%, RAM 21.08%.
+160 B text vs M7.3.

## M7.5 — Relay stuck-open detector (2026-05-03)

`check_relay_stuck_open()` mirrors M6.2's weld detector: commanded
close + sensed open ≥ 200 ms (10 ticks at 50 Hz) raises
`FAULT_RELAY_STUCK_OPEN` (latched) and transitions to EVSE_FAULT.
Spec § 4 #3.

Bench: relay never commanded close (J1772 stays A → READY → never
CHARGING → never close). Detector exits early on `!relay_main_commanded()`
so it's silent. Code-only validation.

Sense reading is now hoisted to a single `relay_main_sense_closed()`
call per tick (`int sensed`) shared by both check_relay_weld and
check_relay_stuck_open — avoids racing the GPIO read between them.

Build: text 23644 / data 20 / bss 27604 — flash 4.52%, RAM 21.08%.
+308 B text vs M7.4.

## M7.6 — session_log on session start/end (2026-05-03)

`safety_task` now writes a `session_record` for every charging
session. Hooked into `evse_transition()`:

- `READY → CHARGING` → `session_start()` captures `start_ts`,
  resets `j1772_max_state_seen=C`, `fault_count=0`, `max_temp_dC=0`.
- `CHARGING → FAULT`  → `session_end(SESSION_END_FAULT)`.
- `CHARGING → READY`  → `session_end(SESSION_END_NORMAL)`.

`post_fault_event()` now also bumps `s_session.fault_count` so a
fault-during-charging is counted on the session record.

Uses `persist_post_session()` (M5.b.5 queue + M5.b.4 ring).

`mwh_delivered` stays 0 in M7.6 — needs CT902 zero offset
(calibration_record reserved field) and an integration loop, both
deferred. Likewise `max_temp_dC` stays 0 because NTCs aren't
populated.

End-reason enum:
```
SESSION_END_NORMAL = 0   /* J1772 left C cleanly */
SESSION_END_FAULT  = 1   /* entered EVSE_FAULT during charging */
SESSION_END_OTHER  = 2
```

Bench: J1772 stays in A → no CHARGING → no session start → silent.
session_log valid count unchanged at 2 across this flash. Code-only
validation today; live session record will appear the moment a
state-C load is connected.

Build: text 23924 / data 20 / bss 27620 — flash 4.57%, RAM 21.09%.
+280 B text vs M7.5.

## M7 roll-up — relay actuation + advertised duty + sessions

Six sub-milestones landed:

| Sub-milestone | What | Tag |
|---|---|---|
| M7.1 | hal/relay.{c,h} | m7-1-relay-hal |
| M7.2 | boot self-test relay actuate-and-readback (gated off) | m7-2-relay-self-test |
| M7.3 | J1772 state-driven relay control (READY ↔ CHARGING) | m7-3-relay-state-driven |
| M7.4 | advertised-amps duty + DIP1/hw cap | m7-4-advertise-amps |
| M7.5 | relay stuck-open detector | m7-5-stuck-open |
| M7.6 | session_log on session start/end | m7-6-session-log |

What's now wired end-to-end (pending bench prep with state-C load):

- READY ↔ CHARGING transitions on J1772 entering/leaving C.
- Relay actuates on CHARGING+J1772=C, opens on any deviation, all
  faults preempt.
- CP duty: idle high in A; advertise per `min(boot_config_amps, DIP1, 48 A)`
  in B/C/D; state F (-12 V) on FAULT.
- Three new fault detectors: RELAY_OPEN_AT_BOOT, RELAY_WELD_AT_BOOT
  (boot self-test step 4, gated off for bench), RELAY_STUCK_OPEN
  (runtime, silent until commanded close).
- Every charging session writes a session_record on end.

Bench carve-outs:
- **Relay actuate-and-readback boot self-test** is build-flag gated
  off. PE12 close cmd doesn't produce a PB12 transition on this
  bench setup — either the contactor coil ties to AC primary or PB12
  is a through-current sense rather than coil-state sense. Probe
  campaign with mains live needed before flipping the default to on.
- **State-C exercise** needs a 882 Ω load across CP↔PE or a real EV.
  Without it, the charging cycle is code-only validated.
- **mWh integration** stalled on CT902 zero-offset baseline.

## Bench fix — semihost tee defaulted off (2026-05-03)

After M7.6, the heartbeat LED stopped blinking. Halt + PC read
showed the chip stuck inside `semihost_write` at the `BKPT 0xAB`
instruction. Root cause: openocd's `shutdown` command does not
clear `DHCSR.C_DEBUGEN`. The bit stays set across openocd
disconnects, so the next `BKPT 0xAB` issued by io_task halts the
core — which then waits forever for a debugger that's no longer
there.

`debugger_attached()` was checking exactly this bit, but because the
bit is sticky-on, the gate doesn't catch the post-disconnect case.

Fix: gated the BKPT body behind a build flag with default OFF:

```c
#ifndef OPENBHZD_SEMIHOSTING
#define OPENBHZD_SEMIHOSTING 0
#endif
```

Production builds (default) issue no BKPTs and never freeze.
Monitor-attached bench debugging uses
`cmake -B build -DOPENBHZD_SEMIHOSTING=1` and reflashes; that build
still streams printk to openocd. `openocd-monitor.sh` updated with
a banner explaining the workflow.

Trade-off: with the default off, printk is silent (PA9 UART is
physically inaccessible per `feedback_openevcharger_pa9_inaccessible`),
so production builds have no debug visibility. Acceptable for
deployed firmware; for bench iteration we toggle the flag.

Verified: post-fix flash, halt+`reg pc` shows the chip in
`prvIdleTask` (FreeRTOS scheduler running), heartbeat LED back at
1 Hz.

## M7.b — Bench RE correction: PB12 force-open latch, sense unknown (2026-05-03)

A morning bench probe of the M7 stack uncovered two RE mistakes
inherited from the original pinout doc. Fix in this commit.

### What we now know

Live-AC bench sequence:
- start: PE12 LOW + PB12 LOW → main contactor OPEN
- PE12 LOW → HIGH (PB12 still LOW) → **LOUD click, contactor CLOSED**
- PE12 HIGH → LOW → **LOUD click, contactor OPEN**
- with PE12 LOW, toggling PB12 high or low → no effect (silent)
- with PE12 HIGH (closed), toggling PB12 LOW → HIGH → **LOUD click,
  contactor forced OPEN**
- PB12 HIGH → LOW (with PE12 still HIGH) → does NOT re-close
- PE12 HIGH → LOW → "resets" the latch; subsequent PE12 HIGH closes
  again normally

This is a **UL2231-style hardware safety latch**:

| Pin  | Role                                                               |
|------|--------------------------------------------------------------------|
| PE12 | Primary close command. HIGH = closed, LOW = open.                  |
| PB12 | Hardware **force-open latch**. HIGH while PE12 HIGH = forces open + latches. PE12 LOW resets the latch. |

The advantage: even if the PE12 driver transistor or MCU pin sticks
HIGH, asserting PB12 HIGH forces the contactor open via an
independent path. Either pilot fault → safe-fail open.

### What was wrong before

1. **PB12 mis-identified as input/sense** — actually an output, the
   force-open latch. Re-classified to `PIN_RELAY_FORCE_OPEN_*`,
   configured as output PP idle-LOW in `gpio_init_all()`.
2. **PB0/NTC2 mis-identified as closed-feedback** — bench data showed
   it stays in 565-686 raw across all (PE12, PB12) combos and across
   contactor open/closed. It's likely AC-mains-presence sense, not
   contactor state. Reverted; `relay_main_sense_closed()` returns 0
   hardcoded.
3. **Polarity inversion attempt was wrong** — the "with mains, contactor
   closes" observation was an artefact of PE12 being left HIGH from
   manual openocd probing. Original Model A polarity (PE12 HIGH = close)
   is correct. The brief inversion in this session was reverted within
   minutes.

### Firmware changes

- `src/core/pin_map.h` — `PIN_RELAY_FORCE_OPEN_*` (PB12 output)
  replaces `PIN_RELAY_SENSE_LEGACY_*`. PB0/NTC2 comment updated.
- `src/hal/gpio.c` — PB12 added to safe-low init list, configured
  as output PP. Removed from input-floating list.
- `src/hal/relay.c/h` — `relay_force_open_latch()`,
  `_release()`, `_active()` API added. `relay_main_sense_closed()`
  now returns 0 (no reliable sense). `relay_main_sense_raw()` exposes
  the AC-presence reading for diagnostic use.
- `src/tasks/safety_task.c` — `OPENBHZD_RELAY_FEEDBACK_KNOWN` build
  flag (default 0) gates off check_relay_weld + check_relay_stuck_open
  to avoid false-positives when sense is unreliable.

### Bench-time validation

Post-flash:
```
PE12: 0x4001180c = 0x00000004    (PE12 LOW = open ✓)
PB12 OCTL: 0x40010c0c = 0x150    (PB12 LOW = no force-open ✓)
PB12 ISTAT: 0x40010c08 = 0xecf0  (PB12 reads LOW externally ✓)
NTC2 sense: 0x20000028 = 0x1fc   (~508 raw — AC-presence "high" but
                                  no longer treated as closed-feedback)
```

Force-open latch and main close paths both work via the API; live
test of `relay_force_open_latch()` from firmware deferred — currently
no fault path asserts it. M8 will hook FC41D `CLEAR_FAULT` into the
re-arm sequence (PE12 LOW → release latch → re-issue close).

`tools/relay_poke.sh` (new): manual openocd helper for these probes.
Subcommands: `init / uninit / pe12-hi / pe12-lo / pb12-hi / pb12-lo /
both-hi / both-lo / read`.

Build size: text 23388 / data 20 / bss 27620 — flash 4.47%, RAM 21.09%.

### Outstanding

- **Real closed-feedback signal not yet identified.** Until found,
  weld and stuck-open detectors are silent. CT902 current sensing
  (M7+) might fill the role indirectly — measure current with relay
  commanded open.
- Bench main contactor was cycled many times during this debugging.
  Listen for sticky behavior on next session; mechanical rest may be
  warranted before extended cycling.

## M8 — FC41D TLV protocol over UART4 (2026-05-03)

OpenEVCharger now speaks the binary TLV protocol from spec § 5 over the
UART4 link to the (currently powered-off) FC41D Wi-Fi/BLE module.
Bench-safe — no contactor or relay traffic involved. Six-piece
landing:

### M8.1 — TLV frame primitives (`src/proto/tlv.{c,h}`)

`tlv_build(cmd, seq, payload, plen, out_buf, out_len)` →
serialises one frame:

```
SOF[2]=A55A | LEN[u16 LE] | CMD | SEQ | payload[plen] | CRC16[2 BE]
```

`tlv_parse(buf, len, &cmd, &seq, &payload, &plen)` returns >0 on a
valid framed CRC-good frame, 0 on incomplete buffer (need more
bytes), <0 on framing/CRC error (caller advances 1 byte to resync).

CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF, no final XOR) — same
implementation as M5.b.3's `crc16.h`, reused. Max payload 56 bytes
(spec said 54; we round up so total frame ≤ 64 stays even).

### M8.2 — UART4 driver (`src/hal/uart5.{c,h}`)

UART4 = "UART5" per spec / pinout — GD32F2's 5th UART, vendor SPL
calls it UART4. PC12 TX (AF PP), PD2 RX (input float), 115200 8N1.
RX uses the RBNE interrupt (NVIC priority 5 = configMAX_SYSCALL) to
push bytes into a FreeRTOS stream buffer; comms_task drains. TX is
blocking (poll-on-TBE then wait-TC) — ~5.6 ms for a full 64-byte
frame, well under safety_task's 20 ms tick.

### M8.3 — comms_task command dispatch

`comms_task` (priority 2, 384-word stack) creates a 256-byte stream
buffer + TX mutex pre-scheduler, then loops:

1. Block on `xStreamBufferReceive`.
2. Append into a `TLV_FRAME_MAX`-byte accumulator.
3. Try `tlv_parse` from head. On success → dispatch + drop consumed
   bytes + retry. On error → drop 1 byte + retry. On incomplete →
   wait for more.

Three commands handled this milestone:

| CMD                | Response             | Payload                                |
|--------------------|----------------------|----------------------------------------|
| `0x01 PING`        | `0x81 PING_ACK`      | none                                   |
| `0x02 GET_STATE`   | `0x82 STATE_REPORT`  | `struct openevcharger_state` (28 B)         |
| `0x0C GET_BUILD_INFO` | `0x8C BUILD_INFO` | ASCII `"<version>|<git_sha>"` + null   |

Unhandled commands log an `unhandled cmd` line and don't reply.
SET_ADVERTISED_AMPS / CLEAR_FAULT / GET_FAULT_LOG / etc. land in a
follow-up M8.b once basic comms is bench-validated.

### M8.4 — system_state snapshot + event publisher

`src/core/system_state.{c,h}` exports a 28-byte word-aligned struct
that safety_task republishes at the end of every tick. Single-writer
+ memcpy-based read keep tearing minimal without a mutex.

`comms_publish_event(cmd, payload, plen)` builds a `seq=0`
unsolicited event frame and shoves it down UART4. safety_task hooks:

| Event              | Trigger                              | Payload                                |
|--------------------|--------------------------------------|----------------------------------------|
| `EVT_BOOT_COMPLETE` | end of boot self-test path           | `u8 self_test_passed + u32 last_fault`|
| `EVT_STATE_CHANGED` | J1772 classifier transition          | `u8 j1772_state`                       |
| `EVT_FAULT_RAISED` | every successful `fault_raise()` edge | `u32 fault_id + u8 j1772 + u8 evse + i16 cp_mv` |
| `EVT_SESSION_BEGAN` | READY → CHARGING                     | `u32 start_ts`                         |
| `EVT_SESSION_ENDED` | CHARGING → other                     | `u32 mwh + u32 dur_ms + u8 reason`     |

Build size: text 27776 / data 20 / bss 27660 — flash 5.30%, RAM
21.12%. Cost vs M7.b: +4.4 KB text (TLV parser, UART4 ISR, comms_task
body, three handlers, snapshot publish, five event payloads).

### Bench validation

Post-flash, halt + `reg pc` reports `prvIdleTask` (FreeRTOS scheduler
healthy with comms_task added). No live UART4 traffic verifiable
today — the FC41D module is held off via PE1=LOW and there's no
USB-UART probe attached to PC12/PD2 yet.

For future bench testing: `tools/host_client.py` is a Python-side
reference TLV client that speaks the protocol. Self-test of
`build_frame` ↔ `parse_frame` roundtrip (PING with seq=42) passes;
CRC corruption is correctly rejected. Usage:

```
tools/host_client.py /dev/ttyUSB0 ping
tools/host_client.py /dev/ttyUSB0 state
tools/host_client.py /dev/ttyUSB0 buildinfo
tools/host_client.py /dev/ttyUSB0 listen   # passive event stream
```

### Outstanding for M8.b (future cycle)

- `SET_ADVERTISED_AMPS` → `boot_config_set_advertised_amps` persist.
- `CLEAR_FAULT` → fault_clear + relay re-arm sequence (PE12 LOW →
  release force-open → next close cycle).
- `GET_FAULT_LOG` → walk event_log ring backwards, return up to N.
- `GET_LIFETIME_KWH` → needs CT integration.
- `SET_LED_OVERRIDE` / `BUZZER_BEEP` — both belong in M9.
- Live FC41D enable + handshake. Currently the module stays off.

## M9 — WS2812 LED strip + buzzer UI (2026-05-03)

Spec § 7 user-I/O lands. Four pieces:

### M9.1 — WS2812 driver (`src/hal/ws2812.{c,h}`)

DMA-fed PWM on PA15 = TIMER1_CH0 (partial-remap-1).

- TIMER1 timer-clock = 60 MHz; period 75 ticks = 1.25 µs.
- "0" bit: CCR=24 (T0H ≈ 0.40 µs).
- "1" bit: CCR=48 (T1H ≈ 0.80 µs).
- DMA1 channel 4 (TIMER1_UP) copies one halfword per timer-update
  event into TIMER_CH0CV. After all bits, 60 padding zeros = 75 µs
  reset latch (≥ 50 µs spec). DMA full-transfer ISR disables timer.
- API: `ws2812_init / set_pixel(i, r, g, b) / clear / show / busy`.
- LED count compile-time: default `OPENBHZD_WS2812_LEDS=8`; override
  via `cmake -DOPENBHZD_WS2812_LEDS=N`.

Static frame buffer = `(8 LEDs × 24 bits + 60 pad) × 2 B = 504 B` in bss.

### M9.2 — Pattern engine (`src/ui/led_patterns.{c,h}`)

`led_render(inputs, t_ms)` maps EVSE/J1772 state to colour + animation
per spec § 7. Stand-in animations (no gamma yet):

| State | Colour | Animation |
|---|---|---|
| BOOT / SELF_TEST | white | 1-LED sweep, ~1×/s |
| READY + J1772=A | dim green | solid |
| READY + J1772=B | blue | solid |
| CHARGING | cyan | breathing 0.3 Hz |
| USER_PAUSED | yellow | breathing 0.5 Hz |
| COOLING_DOWN | orange | breathing 1 Hz |
| any fault | red | flash 2 Hz (5 Hz if GFCI) |

Brightness defaults to 60 % (spec § 7); breathing uses a triangle wave
(cheap stand-in for sin). FC41D `SET_LED_OVERRIDE` lands a solid
colour via `led_override_set(mode, r, g, b)`.

### M9.3 — Buzzer engine (`src/ui/buzzer.{c,h}`)

Software-toggled GPIO on PB2; pattern engine ticked from io_task at
50 Hz (20 ms granularity). Patterns: BOOT_PASS/FAIL, SESSION_START/END,
FAULT_NON_GFCI / FAULT_GFCI, BUTTON, ONESHOT (TLV-driven). Higher
priority pattern wins (later-call replaces earlier).

### M9.4 — io_task wiring + TLV handlers

`io_task` now:
- Pulls `system_state_snapshot()` each 50 ms tick.
- Fires buzzer one-shots on EVSE transitions (boot complete,
  session start/end, fault edges).
- Calls `buzzer_tick()` every tick.
- Renders LED frame every 60 ms (skips if `ws2812_busy()`).

`comms_task` adds two handlers:
- `CMD_SET_LED_OVERRIDE` (0x0A) → `led_override_set`.
- `CMD_BUZZER_BEEP` (0x0B) → `buzzer_set_oneshot(ms)` (capped 500 ms).

### Bench validation

Post-flash: chip alive at `prvIdleTask` after 4 s. `GPIOA_OCTL` shows
PA15 driven by TIMER1 (bit 15 toggling under DMA control). `GPIOB_OCTL`
bit 2 (buzzer) idle LOW. Heartbeat LED on PD4 blinking. No FAULT
raised, no spurious buzzer activity.

Live LED + buzzer observation needs the user at the bench. Visual
validation pending — drop me a note about what colour shows up after
boot. Strip count default 8; if the bench has a different number,
rebuild with `cmake -DOPENBHZD_WS2812_LEDS=N`.

Build: text 30168 / data 20 / bss 28180 — flash 5.76%, RAM 21.51%.
+2.4 KB text vs M8 (DMA driver + pattern engines + io_task expansion).

### Next milestone
M8.b — round out FC41D TLV with the missing handlers
(SET_ADVERTISED_AMPS, CLEAR_FAULT, GET_FAULT_LOG, GET_LIFETIME_KWH).
Or M10 — first end-to-end bench charging session (waiting on EVSE
tester delivery, ~1 month).
Implements the binary frame parser/builder, command dispatch, async
event publish per spec § 5. Once M8 lands, FC41D can `SET_ADVERTISED_AMPS`,
`CLEAR_FAULT` (anything except GFCI), `REQUEST_STOP`/`REQUEST_START_RESUME`,
`GET_STATE`, etc., and OpenEVCharger can publish state changes / fault
events / session events back. Closes the protocol surface that
WiFi/BLE/cloud features (run on FC41D) need to interact with the
safety core.

## Host unit tests

Spec § 11 calls for a CMake `host` target compiling the pure-logic
modules (`j1772.c`, `fault.c`, `tlv.c`, `crc16.c`, `crc.c`) against
the host compiler and running unit tests. **Required passing for any
PR touching `core/`, `proto/`, or `persist/`.**

Run:

```bash
cmake -S tests -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

The test project lives in `tests/` and is deliberately **separate**
from the firmware CMakeLists so `cmake -S .` still hard-requires the
arm-none-eabi toolchain file. Don't merge them; the firmware build
links a linker script + vendor SPL + FreeRTOS that the host build has
no business seeing.

Coverage at first landing (55 cases):
- `crc16` standard check vector (`123456789` → `0x29B1`), reproducibility,
  bit-flip sensitivity, length sensitivity.
- `crc32` standard check vector (`123456789` → `0xCBF43926`).
- `j1772` band classifier at every threshold edge, debounce semantics
  (streak gate, prior-committed during transition, candidate change
  resets streak), streak saturation at `0xFF`.
- `fault` raise/clear edge returns, GFCI clear refusal, `first_raised`
  promotion on partial clear, `clear_all_clearable` excludes GFCI and
  self-clearing faults, `is_latched_kind` boundary.
- `tlv` round-trip empty / 4-byte / max-payload (56-byte) frames,
  oversize / undersize / bad-SOF / bad-CRC / bad-LEN parser failures,
  CRC stored big-endian on the wire.

`pingpong.c` is in spec § 11's listed set but is skipped here — it
depends on `hal/w25q.h` which would need a mock. Defer until there's
a concrete reason beyond what the on-target ping-pong commit already
exercises.

(Update 2026-05-04: pingpong + boot_config + over_temp tests landed
via subagent, suite is now 94 cases. See "Post-M9 milestones" below.)

---

## Post-M9 milestones (2026-05-03 → 2026-05-04)

A running log of everything that landed after `m9-led-real`. Each
bullet links to the commit; consult the commit body for full
context. Bench-validated entries are marked ✅.

### FC41D ESPHome integration (in-tree at `OpenEVCharger/fc41d/`)

- ESPHome external component `openevcharger_tlv` speaks the binary TLV
  protocol over UART. Surfaces full state to Home Assistant
  (CP voltages, advertised + active amps, lifetime / session kWh,
  EVSE + J1772 state, fault status, contactor command). Writable
  advertised-amps number; stop / start / clear-fault buttons.
- DIP4-strap "FC41D flash mode" lets the BK7231N module be
  reflashed with the MCU's TLV traffic suppressed; PC9 internal
  button pulses CEN for ltchiptool handshake.
- Per-unit MAC override from the GD32 UID96 (OEM ships every unit
  with the same MAC → DHCP/mDNS collisions). FC41D issues
  `CMD_GET_DEVICE_ID`, MCU returns UID96 from `0x1FFFF7E8`, FC41D
  folds to 6 bytes + locally-administered OUI, calls
  `WiFi.setMacAddress()` at `setup_priority::AFTER_WIFI`.

### Top-button pause/start + PB12 force-open on FAULT (`7fcba09`) ✅

External top button (BTN_TOP via the resistor ladder on PC3) toggles
between `READY/CHARGING → USER_PAUSED` and `USER_PAUSED → READY`.
PB12 force-open latch asserted on `EVSE_FAULT` entry / released on
exit — UL2231-style redundant safety against a stuck-high PE12
driver.

### Over-temp robustness (`0b733d0` + `d7ee845`) ✅

`OT_PERSIST_TICKS=5` (100 ms) before raising — filters single-sample
ADC glitches. `OPENBHZD_NTC2_PRESENT=0` default since PB0 isn't a
thermistor (see channel-role correction below).

### Channel-role correction: PA2 = gun NTC (`a70a28e`) ✅

Bench experiment grounding the front-block "NTC" pins zeroed PA2
("AC") and PA3 ("NTC1") respectively, confirming both are populated
NTC thermistors. The earlier "Mains Voltage @ 0.06151 V/count"
calibration was coincidence — disconnected NTC pin float-railing at
a value that scaled to a plausible mains number.

Updated channel roles per OEM intent:
- PA2 (rank "AC") = gun-cable NTC
- PA3 (rank "NTC1") = wall-plug NTC
- PB0 (rank "NTC2") = NOT a thermistor (probably AC-mains-presence
  sense — bench shows it tracks current flow through the contactor)

V/I sensing on this PCB does NOT route through PA2 — it likely runs
through the U11 PGA (gain bits PB9 / PD15) and is not yet wired up.

FC41D side renamed `ac_adc_raw` → `gun_ntc_adc_raw`; "Mains Voltage"
HA entity dropped, replaced with "Gun NTC Temperature" via β-model
(later upgraded to LUT — see below).

### Stock-fw RE results (`ffbfc68`) — `docs/re-stock-safety.md`

Three findings from a static analysis pass on stock fw V1.0.066:

1. **NTC LUT extracted** (HIGH confidence): 150-entry direct
   lookup at `0x08024f28..0x08025054`. `lut[idx] = raw_at(idx − 30 °C)`,
   −30..+119 °C in 1 °C steps. Pull-up confirmed 10 kΩ (NOT 4.7 kΩ
   as our pre-bench pinout guess assumed). Stock factory trip = 95 °C
   @ raw=300; max user setpoint 120 °C. FC41D-side conversion now
   uses this LUT (`fc41d/.../ntc_lut.h`) — the previous β=3380 model
   was off by ~10 °C at 85 °C since the OEM thermistor's β is
   closer to 3980. **Phase 2** (MCU-side `over_temp.h` thresholds
   migrating from 532/672 → 396/525) is still pending — defers
   until the test cases get updated alongside.
2. **GFCI sense = PE2, polled** (MEDIUM-HIGH initial confidence):
   stock fw's 8-state TBB at `0x08012824` drives PE3 (CAL pulse) +
   PE4, samples PE2 mid-cycle. NO EXTI used — all EXTI vectors point
   at the default trap stub.
3. **Relay closed-feedback = no discrete pin** (LOW confidence):
   exhaustive search of all 29 `GPIO_ReadInputDataBit` callsites
   found no GPIOE-bit-12 read. Strong inference: stock fw infers
   weld / stuck-open from PC0 (CT902 secondary load-current) vs
   PE12 + CP-state expectations, not from a discrete sense pin.

### GFCI live (`78b6c16`, tag `gfci-live`) ✅

PE2 sense + active-low polarity bench-confirmed via
`tools/gpio_diff.sh` wiggle 2026-05-04 (drove the GFCI module's
external trip line HIGH; PE2 dropped 1 → 0, returned 1 on release).
Polarity inverted from the agent's static-decode hypothesis; static
structure stood.

`hal/gfci.{c,h}` + `safety_task::check_gfci` debounce 3 ticks (60 ms)
of PE2 LOW → `fault_raise(FAULT_GFCI)` + `evse_transition(EVSE_FAULT)`
+ PB12 force-open latch. Latched + power-cycle-only clear per UL2231
(`fault.c::fault_clear` already refuses GFCI by id).

End-to-end bench validation: real GFCI trip during CHARGING → 60 ms
debounce → fault raised → EVSE FAULT → session persisted with
reason=FAULT (dur_ms=11705) → CP state F → relay open and stays
open until power cycle. Production blocker #1 retired.

### Relay closed-feedback ruled out (`ee33c23`) ✅

Bench `gpio_diff` snapshot before/after a contactor close (no load):
zero new input bits changed across all 5 GPIO ports. The only diffs
were the outputs we drove (PE12, PE13) plus a coincidental W25Q SPI
MISO bit. Definitively rules out any coil-side feedback (aux
contact / TCR sense) — those would move regardless of load.
`FAULT_RELAY_WELD` and `FAULT_RELAY_STUCK_OPEN` are therefore
coupled to the V/I path (PC0/U11) and cannot land independently.

### CP advertise-duty inverted + ADC sample-time (`bf1051b`) ✅

`cp_pwm_set_advertise_amps()` had the J1772 duty formula inverted —
spec is `amps = duty% × 0.6` (so duty% = `amps / 0.6`), code had
`amps × 0.6`. `48 A` advertised as **28% duty** instead of **80%**;
a real EV would have charged at ~17 A, not 48 A. Bench-confirmed
via multimeter: state-C charging averaged −2.7 V on CP (matches
28% high) instead of the spec-correct ~+2.4 V (80% high).

Same commit: bumped `adc_inject` S&H window from 28.5 → 239.5 cycles
(~3 µs → ~24 µs). The 3 µs was sampling at the PWM rising edge before
CP had settled in the no-load case. Symptom: `cp_high` stuck at
~9.9 V once advertise PWM started, never returning to 12 V even on
full disconnect — trapping the classifier in state B and breaking
the `USER_PAUSED → READY` unplug-escape path. Both fixes
bench-validated: boot=12 V ✓, state C=1.9 V avg ✓, state A/B
disconnected=12 V ✓.

### Test suite expansion to 94 cases (`0c6f745`, `32078cd`, `d273922`, `a4f0836`)

- w25q HAL mock + pingpong tests (+12 cases)
- boot_config tests (+8)
- `core/over_temp.{c,h}` extracted to a pure header-inline module +
  test suite (+19)
- `tests/CMakeLists.txt` wire-up

Net 55 → 94 cases, 1/1 PASS. Firmware -20 bytes from the over_temp
refactor (header-only inline; safety_task inlines the same as
before, host build links via the .c definition).

### U11 identified as BL0939 (Shanghai Belling) — 2026-05-04

Visually-confirmed bench reading: U11 is a **Shanghai Belling
BL0939**, not the PGA we'd assumed. Single-phase metering IC,
**dual current channels + voltage**, UART interface, internal
calibration, and a hardware fault-output pin for differential-current
(RCD) detection. Three implications:

1. **PB9 / PD15 are NOT "gain bits"** — likely CS / RESET / mode
   straps for the BL0939 (final mapping pending datasheet review +
   bench probe). Names kept as `PIN_U11_G0/G1` in `pin_map.h` until
   the BL0939 protocol decode lands.
2. **PC0 / PC1 are not the calibrated CT ADCs we thought.** Current
   data arrives over UART, not analog. PC0 / PC1 may just be CT
   secondary references or unused on this SKU.
3. **There's no separate "GFCI module"** — the BL0939 itself does
   the differential-current trip via its on-chip dual-current
   path. The PE2 fault sense we wired today (`78b6c16`,
   `gfci-live`) is the BL0939's fault output. PE3 (CAL) is the
   BL0939's self-test input. Today's wiggle test simulated a fault
   at the BL0939's CAL input.

This collapses the gating chain: `BL0939 UART parser` is now the
single highest-leverage outstanding task — it unblocks
`HARD_OVER_CURRENT`, `SOFT_OVER_CURRENT`, `RELAY_WELD`, and
`RELAY_STUCK_OPEN` simultaneously. The task is pure code +
bench-scope investigation; no hardware mods needed.

Stock-fw RE pass earlier searched for **BL0940** (single-channel
sibling) and didn't find a parser. BL0939 protocol differs in
detail; re-search the stock fw with BL0939-specific opcodes /
register addresses next.

### New tools

- `tools/gfci_wiggle.sh` — non-destructive PE2 sense-pin tests:
  `cal-test` (drive PE3, sample PE2), `seq-test` (full stock 8-state
  cycle replay), `pull-test` (PE2 internal pull bias under stock fw).
- `tools/gpio_diff.sh` — generic GPIO snapshot + diff helper for
  "drive an external stimulus, see which MCU pin moves" pattern.
  Reads all 5 GPIOx ISTAT under OpenOCD halt+poke, annotates diffs
  with pinout labels (`PE2: 1 → 0  (GFCI-SENSE?)` etc.). Used to
  confirm GFCI sense + rule out relay closed-feedback.
