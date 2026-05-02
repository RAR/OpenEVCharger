#include "io_task.h"
#include "gd32f20x.h"

#define HEARTBEAT_PORT GPIOD
#define HEARTBEAT_PIN  GPIO_PIN_4
#define HEARTBEAT_RCU  RCU_GPIOD

static void heartbeat_init(void)
{
    rcu_periph_clock_enable(HEARTBEAT_RCU);
    gpio_init(HEARTBEAT_PORT,
              GPIO_MODE_OUT_PP,
              GPIO_OSPEED_2MHZ,
              HEARTBEAT_PIN);
    gpio_bit_reset(HEARTBEAT_PORT, HEARTBEAT_PIN);
}

static void io_task_run(void *arg)
{
    (void)arg;
    heartbeat_init();

    for (;;) {
        gpio_bit_set(HEARTBEAT_PORT, HEARTBEAT_PIN);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_bit_reset(HEARTBEAT_PORT, HEARTBEAT_PIN);
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
