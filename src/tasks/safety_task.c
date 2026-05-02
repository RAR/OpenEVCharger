#include "safety_task.h"
#include "../hal/wdg.h"

static void safety_task_run(void *arg)
{
    (void)arg;

    /* Initialise the watchdog as the very first thing this task does.
     * From this point on the chip will reset if we don't loop within 1 s. */
    wdg_init();

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        /* M1: nothing safety-related to do yet. Just kick. */
        wdg_kick();

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAFETY_TASK_PERIOD_MS));
    }
}

void safety_task_create(void)
{
    xTaskCreate(safety_task_run,
                "safety",
                SAFETY_TASK_STACK_WORDS,
                NULL,
                SAFETY_TASK_PRIORITY,
                NULL);
}
