#include "comms_task.h"

static void comms_task_run(void *arg)
{
    (void)arg;
    /* M1 stub — real body lands in M8 (FC41D TLV protocol). */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

void comms_task_create(void)
{
    xTaskCreate(comms_task_run,
                "comms",
                COMMS_TASK_STACK_WORDS,
                NULL,
                COMMS_TASK_PRIORITY,
                NULL);
}
