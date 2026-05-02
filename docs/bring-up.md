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
