/* OpenBHZD M1 — FreeRTOS scaffold + 4-task skeleton.
 *
 * main() does no real work: it creates the four safety-core tasks then
 * starts the scheduler. PD4 heartbeat moves into io_task. safety_task
 * arms the watchdog as its first action.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "gd32f20x.h"
#include "hal/clock.h"
#include "hal/uart.h"
#include "core/pin_map.h"
#include "hal/gpio.h"
#include "hal/adc_scan.h"
#include "hal/cp_pwm.h"
#include "hal/adc_inject.h"
#include "hal/spi3.h"
#include "hal/w25q.h"
#include "persist/boot_count.h"
#include "persist/boot_config.h"
#include "persist/calibration.h"
#include "persist/event_log.h"
#include "persist/session_log.h"
#include "persist/crash_state.h"
#include "tasks/safety_task.h"
#include "tasks/io_task.h"
#include "tasks/comms_task.h"
#include "tasks/fc41d_flash_helper.h"
#include "tasks/persist_task.h"

extern uint32_t SystemCoreClock;

/* Newlib's __libc_init_array references _init/_fini; we have no C++
 * static ctors so empty stubs are fine. */
void _init(void) {}
void _fini(void) {}

/* FreeRTOS hooks. configCHECK_FOR_STACK_OVERFLOW = 2 means this fires
 * if any task touches a low-water canary. configUSE_MALLOC_FAILED_HOOK = 1
 * means this fires if pvPortMalloc returns NULL. We trap into an infinite
 * loop in both cases — a debugger session can break in and inspect. */

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task; (void)task_name;
    __asm volatile("cpsid i");
    for (;;) {}
}

void vApplicationMallocFailedHook(void)
{
    __asm volatile("cpsid i");
    for (;;) {}
}

int main(void)
{
    /* Vendor SystemInit() has already run from startup_gd32f20x_cl.S
     * (clock tree at 120 MHz). */

    /* If built with OPENBHZD_REAL_120M_PLL=1, swap the SDK's broken
     * 120m_hxtal config for a clean direct chain. No-op otherwise. */
    clock_real_120m_init();

    uart_init();
    printk("\n--- OpenBHZD M2 boot, SystemCoreClock=%u Hz ---\n",
           (unsigned)SystemCoreClock);
    clock_log_status();

    /* Release JTAG pins (PA15, PB3, PB4) so SPI3 (PB3/PB4/PB5) and
     * TIMER1_CH0 (PA15) can use them. SWDPENABLE keeps SWD alive
     * (PA13/PA14) for the OpenOCD probe. Must run before any AF init
     * touches those pins — including spi3_init() / w25q_init(). */
    rcu_periph_clock_enable(RCU_AF);
    gpio_pin_remap_config(GPIO_SWJ_SWDPENABLE_REMAP, ENABLE);

    gpio_init_all();
    gpio_log_straps();
    adc_scan_init();
    printk("ADC scan armed: 11 ranks @ ~3.6 kHz\n");
    cp_pwm_init();
    printk("CP PWM armed: TIMER0 1 kHz, PE13 idle HIGH\n");
    adc_inject_init();
    printk("CP injected ADC armed: PA4 sampled on each PWM rising edge\n");

#if defined(OPENBHZD_BL0939_SMOKE) && OPENBHZD_BL0939_SMOKE
    /* One-shot bench probe: bit-bang SPI to U11 (BL0939) and log
     * what we get back. Pure observation; no protocol decoding yet.
     * Enable with `cmake -DOPENBHZD_BL0939_SMOKE=1`. Default off so
     * production builds don't waste boot time on the test. */
    {
        extern void bl0939_smoke_test(void);
        bl0939_smoke_test();
    }
#endif

    spi3_init();
    if (w25q_init() != 0) {
        printk("W25Q: JEDEC init FAIL — boot_count not persisted\n");
    } else {
        printk("W25Q: JEDEC ID = 0x%06x\n", (unsigned)w25q_jedec_id());

#if defined(OPENBHZD_BENCH_CRASH_RESET) && OPENBHZD_BENCH_CRASH_RESET
        /* One-shot recovery: erase crash_state ping-pong sectors so
         * fast_restart_count returns to 0. Set during a single
         * flash cycle when the bench has gotten itself into
         * CRASH_LOOP_SAFE_FAIL, then unset and re-flash. */
        extern int w25q_erase_sector(uint32_t addr);
        w25q_erase_sector(0x04D000U);
        w25q_erase_sector(0x04E000U);
        printk("BENCH: crash_state sectors erased (one-shot)\n");
#endif
        uint32_t bc = boot_count_increment();
        if (bc == 0xFFFFFFFFu) printk("W25Q: boot_count write FAIL\n");
        else                   printk("boot_count = %u\n", (unsigned)bc);

        if (boot_config_load() < 0) {
            printk("boot_config: load failed; defaults uninitialised\n");
        }
        if (calibration_load() < 0) {
            printk("calibration: load failed; defaults active\n");
        }

        event_log_init();
        event_log_set_boot_count((uint16_t)bc);

        session_log_init();
        session_log_set_boot_count((uint16_t)bc);

        crash_state_boot_increment();
    }

    safety_task_create();
    io_task_create();
    /* DIP4 held LOW at boot = "FC41D flash mode": skip comms_task so
     * uart5_init() never runs and PC12/PD2 stay tri-stated. The
     * BK7231N's serial bootloader on the same wires (its UART1
     * P10/P11) then has the bus to itself for ltchiptool flash.
     *
     * Spawn the flash helper instead — it powers up the FC41D
     * (VEN+CEN high) and watches PC9 for a button press, pulsing
     * CEN low to drop the module back into bootloader mode for
     * ltchiptool's handshake. Slide DIP4 back open + power-cycle
     * to resume normal TLV traffic. */
    if (gpio_dip4_held()) {
        printk("MODE: FC41D flash (DIP4 held) — comms_task suppressed\n");
        fc41d_flash_helper_create();
    } else {
        /* Normal mode: power up the FC41D and release reset so its
         * firmware runs and starts answering us on UART4. Stock
         * firmware's Thd_Wifi did the same VEN-then-CEN release with
         * a delay between. Without this the module sits dead. */
        gpio_bit_set(PIN_FC41D_VEN_PORT, PIN_FC41D_VEN_PIN);
        for (volatile int i = 0; i < 600000; ++i) { __asm__ volatile (""); }
        gpio_bit_set(PIN_FC41D_CEN_PORT, PIN_FC41D_CEN_PIN);
        printk("FC41D: VEN=1 CEN=1 — module released\n");
        comms_task_create();
    }
    persist_task_create();

    printk("scheduler starting\n");
    vTaskStartScheduler();

    /* Should never return. If we get here, the scheduler ran out of
     * heap before creating the idle task. Trap. */
    for (;;) {}
}
