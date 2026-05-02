#include "persist_task.h"

static void persist_task_run(void *arg)
{
    (void)arg;
    /* M1 stub — real body lands in M5 (W25Q persistence). */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

void persist_task_create(void)
{
    xTaskCreate(persist_task_run,
                "persist",
                PERSIST_TASK_STACK_WORDS,
                NULL,
                PERSIST_TASK_PRIORITY,
                NULL);
}
