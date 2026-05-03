/* OpenBHZD M1 — FreeRTOS scaffold + 4-task skeleton.
 *
 * main() does no real work: it creates the four safety-core tasks then
 * starts the scheduler. PD4 heartbeat moves into io_task. safety_task
 * arms the watchdog as its first action.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "hal/uart.h"
#include "hal/gpio.h"
#include "hal/adc_scan.h"
#include "hal/cp_pwm.h"
#include "hal/adc_inject.h"
#include "hal/spi3.h"
#include "hal/w25q.h"
#include "tasks/safety_task.h"
#include "tasks/io_task.h"
#include "tasks/comms_task.h"
#include "tasks/persist_task.h"

extern uint32_t SystemCoreClock;

/* M4 self-test: round-trip 256 bytes at the last sector of the 8 MB chip.
 * Stock firmware uses 0x000000–0x04CFFF per the spec § 6 memory map;
 * 0x07F000 sits in the upper "reserved" area, untouched by stock fw. */
#define M4_TEST_ADDR  0x07F000U

static void m4_self_test(void)
{
    int rc;

    if (w25q_init() != 0) {
        printk("W25Q: JEDEC init FAIL (probably no SPI / wrong wiring)\n");
        return;
    }
    uint32_t id = w25q_jedec_id();
    printk("W25Q: JEDEC ID = 0x%06x", (unsigned)id);
    if (id == 0x00EF4017) printk(" (W25Q64JV)\n");
    else                  printk(" (unrecognised — non-Winbond or different capacity)\n");

    rc = w25q_erase_sector(M4_TEST_ADDR);
    if (rc != 0) {
        printk("W25Q: erase FAIL (timeout)\n");
        return;
    }
    printk("W25Q: erased sector @ 0x%06x\n", M4_TEST_ADDR);

    uint8_t pat[W25Q_PAGE_SIZE];
    for (unsigned i = 0; i < W25Q_PAGE_SIZE; ++i) {
        pat[i] = (i & 1) ? 0x5A : 0xA5;
    }
    rc = w25q_program(M4_TEST_ADDR, pat, sizeof pat);
    if (rc != 0) {
        printk("W25Q: program FAIL (timeout)\n");
        return;
    }
    printk("W25Q: programmed %u bytes @ 0x%06x\n", (unsigned)sizeof pat, M4_TEST_ADDR);

    uint8_t back[W25Q_PAGE_SIZE];
    w25q_read(M4_TEST_ADDR, back, sizeof back);

    int first_mismatch = -1;
    for (unsigned i = 0; i < sizeof back; ++i) {
        if (back[i] != pat[i]) { first_mismatch = (int)i; break; }
    }
    if (first_mismatch < 0) {
        printk("W25Q round-trip PASS: %u bytes match\n", (unsigned)sizeof back);
    } else {
        printk("W25Q round-trip FAIL: first mismatch idx=%d expect=0x%02x got=0x%02x\n",
               first_mismatch,
               (unsigned)pat[first_mismatch], (unsigned)back[first_mismatch]);
    }
}

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

    uart_init();
    printk("\n--- OpenBHZD M2 boot, SystemCoreClock=%u Hz ---\n",
           (unsigned)SystemCoreClock);

    gpio_init_all();
    gpio_log_straps();
    adc_scan_init();
    printk("ADC scan armed: 11 ranks @ ~3.6 kHz\n");
    cp_pwm_init();
    printk("CP PWM armed: TIMER0 1 kHz, PE13 idle HIGH\n");
    adc_inject_init();
    printk("CP injected ADC armed: PA4 sampled on each PWM rising edge\n");

    spi3_init();
    m4_self_test();

    safety_task_create();
    io_task_create();
    comms_task_create();
    persist_task_create();

    printk("scheduler starting\n");
    vTaskStartScheduler();

    /* Should never return. If we get here, the scheduler ran out of
     * heap before creating the idle task. Trap. */
    for (;;) {}
}
