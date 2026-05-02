#include "io_task.h"
#include "core/pin_map.h"
#include "gd32f20x.h"

static void io_task_run(void *arg)
{
    (void)arg;
    /* GPIO already configured by gpio_init_all() in main(). */

    for (;;) {
        gpio_bit_set(PIN_HEARTBEAT_PORT, PIN_HEARTBEAT_PIN);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_bit_reset(PIN_HEARTBEAT_PORT, PIN_HEARTBEAT_PIN);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void io_task_create(void)
{
    xTaskCreate(io_task_run,
                "io",
                IO_TASK_STACK_WORDS,
                NULL,
                IO_TASK_PRIORITY,
                NULL);
}
