/* boards/nexcyber/main.c — M1 bring-up entry point.
 *
 * Brings the clock + log UART up, then starts a FreeRTOS scheduler with
 * one heartbeat task. Proves the ARM_CM4F port + SysTick wiring +
 * vector-table aliasing + heap_4 all land coherently on the Nations
 * silicon. No safety core, no comms, no persistence — those land in
 * M2+ as the shared HAL ports.
 *
 * The previous M0 was a bare-metal busy-loop heartbeat in main()
 * (commit ab0a478). Swapping that for a scheduler-driven heartbeat
 * is the smallest possible "FreeRTOS works on this chip" trace —
 * if the printk timing changes from "tight nop loop" to "1 Hz exact
 * via vTaskDelay(pdMS_TO_TICKS(1000))" then the scheduler is alive.
 *
 * See boards/nexcyber/README.md for the porting milestone roadmap.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "hal/clock.h"
#include "hal/uart.h"

/* Newlib's __libc_init_array references _init/_fini; we have no C++
 * static ctors so empty stubs are fine. Same idiom as src/main.c on
 * the rippleon target. */
void _init(void) {}
void _fini(void) {}

/* FreeRTOS hooks. configCHECK_FOR_STACK_OVERFLOW=2 + configUSE_MALLOC_FAILED_HOOK=1
 * (see src/FreeRTOSConfig.h) make these mandatory link-time symbols. Trap
 * into an infinite loop with interrupts disabled so a debugger can break
 * in and inspect — same pattern as rippleon's src/main.c. */
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

static void heartbeat_task(void *arg)
{
    (void)arg;
    uint32_t beat = 0;
    for (;;) {
        printk("beat %u\n", (unsigned)beat++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int main(void)
{
    clock_real_120m_init();
    uart_init();
    clock_log_status();
    printk("openevcharger nexcyber: M1 bring-up (FreeRTOS scheduler)\n");

    /* 256 words = 1 KB stack — plenty for printk + an itoa scratch. */
    BaseType_t ok = xTaskCreate(heartbeat_task, "heartbeat",
                                256, NULL, tskIDLE_PRIORITY + 1, NULL);
    if (ok != pdPASS) {
        printk("xTaskCreate failed: %d\n", (int)ok);
        for (;;) {}
    }

    vTaskStartScheduler();

    /* Should never return. If it does, the scheduler couldn't start
     * — typically out of heap for the idle task. */
    printk("scheduler returned — halt\n");
    for (;;) {}
}
