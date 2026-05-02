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
#include "tasks/safety_task.h"
#include "tasks/io_task.h"
#include "tasks/comms_task.h"
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

    uart_init();
    printk("\n--- OpenBHZD M2 boot, SystemCoreClock=%u Hz ---\n",
           (unsigned)SystemCoreClock);

    gpio_init_all();
    gpio_log_straps();
    adc_scan_init();
    printk("ADC scan armed: 11 ranks @ ~3.6 kHz\n");
    cp_pwm_init();
    printk("CP PWM armed: TIMER0 1 kHz, PE13 idle HIGH\n");

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
