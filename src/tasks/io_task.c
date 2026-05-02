#include "io_task.h"
#include "core/pin_map.h"
#include "ui/buttons.h"
#include "hal/uart.h"
#include "hal/adc_scan.h"
#include "gd32f20x.h"

#define IO_TICK_MS    50
#define HB_TOGGLE_MS  500
#define DUMP_MS       5000

static void adc_dump(void)
{
    uint16_t b[ADC_RANKS];
    adc_scan_latest(b);
    printk("ADC: AC=%u NTC1=%u CT=%u LCT=%u CPR=%u CC=%u PE=%u "
           "NTC2=%u UNUSED=%u BTN=%u VREF=%u\n",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10]);
}

static void io_task_run(void *arg)
{
    (void)arg;
    buttons_init();

    TickType_t last_wake = xTaskGetTickCount();
    unsigned ms = 0;
    int hb_level = 0;

    for (;;) {
        if ((ms % HB_TOGGLE_MS) == 0) {
            hb_level ^= 1;
            if (hb_level) gpio_bit_set(PIN_HEARTBEAT_PORT, PIN_HEARTBEAT_PIN);
            else          gpio_bit_reset(PIN_HEARTBEAT_PORT, PIN_HEARTBEAT_PIN);
        }

        buttons_poll();

        if ((ms % DUMP_MS) == 0) adc_dump();

        ms += IO_TICK_MS;
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(IO_TICK_MS));
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
