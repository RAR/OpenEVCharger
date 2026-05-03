#include "io_task.h"
#include "core/pin_map.h"
#include "ui/buttons.h"
#include "hal/uart.h"
#include "hal/adc_scan.h"
#include "hal/adc_inject.h"
#include "tasks/persist_task.h"
#include "gd32f20x.h"

#define IO_TICK_MS        50
#define HB_TOGGLE_MS      500
#define DUMP_MS           5000
#define ALIVE_MARKER_MS   60000U

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

    {
        struct event_record rec = {
            .timestamp = (uint32_t)xTaskGetTickCount(),
            .fault_id  = 0xB001U,
            .j1772_state = 0xA0U,
            .evse_state  = 0xB1U,
            .cp_mv = (int16_t)cp_high_mv(),
            .cc_amps = 0,
            .ntc1_dC = 0, .ntc2_dC = 0, .active_amps_x10 = 0,
        };
        int rc = persist_post_event(&rec);
        printk("io_task: posted startup event rc=%d\n", rc);
    }

    TickType_t last_wake = xTaskGetTickCount();
    unsigned ms = 0;
    int hb_level = 0;
    int alive_posted = 0;

    for (;;) {
        if ((ms % HB_TOGGLE_MS) == 0) {
            hb_level ^= 1;
            if (hb_level) gpio_bit_set(PIN_HEARTBEAT_PORT, PIN_HEARTBEAT_PIN);
            else          gpio_bit_reset(PIN_HEARTBEAT_PORT, PIN_HEARTBEAT_PIN);
        }

        buttons_poll();

        if ((ms % DUMP_MS) == 0) adc_dump();

        if (!alive_posted && ms >= ALIVE_MARKER_MS) {
            (void)persist_post_crash_state_reset();
            alive_posted = 1;
            printk("io_task: alive marker posted (ms=%u)\n", ms);
        }

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
