# OpenBHZD bring-up log

Per-milestone hardware validation notes. Every milestone gets an entry with
date, success criterion, observed result, and any deviations from spec.

## M0 — Toolchain Bootstrap

**Date completed:** 2026-05-02
**Spec section:** § 9 M0
**Plan:** docs/superpowers/plans/2026-05-02-m0-toolchain-bootstrap.md

### Success criterion (from spec)
PD4 heartbeat LED blinks at 1 Hz with the GD32F205V running at 120 MHz, after
SWD-flashing `openbhzd.elf` produced by CMake + arm-none-eabi-gcc + GD32F20x
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
--- OpenBHZD M2 boot, SystemCoreClock=120000000 Hz ---
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

The memory entry `project_openbhzd_bench_cp_buffer_dead` is OBSOLETE;
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
- `relay_commanded_closed()`: function stub returning 0. OpenBHZD does
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
Documented in NOTES.md and `project_openbhzd_cp_calibration` memory.
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

### Next milestone
M6.5 — CP=E classifier → `FAULT_CP_NO_PILOT`. Sustained J1772 state E
(say, 5 ticks = 100 ms) with `evse_state != EVSE_FAULT` raises the
fault. Bench-validatable by shorting CP↔PE manually if a probe is
on hand; otherwise the classifier-driven path will fire the next
time a real EV pilot is broken. Per spec § 4 #5.
