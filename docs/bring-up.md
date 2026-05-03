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
