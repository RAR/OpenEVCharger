#include "fc41d_flash_helper.h"
#include "pin_map.h"
#include "../hal/uart.h"
#include "../diag/stack_watch.h"

#include "FreeRTOS.h"
#include "task.h"

#include "gd32f20x.h"

#define FC41D_FLASH_TASK_STACK_WORDS  192U
#define FC41D_FLASH_TASK_PRIORITY     1U   /* lowest above idle */

#define POLL_MS                       10U  /* button poll period */
#define DEBOUNCE_TICKS                3U   /* 3 × 10 ms = 30 ms steady-low */
#define CEN_PULSE_MS                  50U  /* hold CEN low long enough for the
                                              BK7231N bootloader to re-arm */

static void cen_pulse_low(void)
{
    gpio_bit_reset(PIN_FC41D_CEN_PORT, PIN_FC41D_CEN_PIN);
    vTaskDelay(pdMS_TO_TICKS(CEN_PULSE_MS));
    gpio_bit_set(PIN_FC41D_CEN_PORT, PIN_FC41D_CEN_PIN);
}

static void fc41d_flash_helper_run(void *arg)
{
    (void)arg;

    /* Bring the module up: supply on, then release reset. The 50 ms
     * gap matches what the stock firmware's Thd_Wifi did between
     * VEN-high and CEN-high. */
    gpio_bit_set(PIN_FC41D_VEN_PORT, PIN_FC41D_VEN_PIN);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_bit_set(PIN_FC41D_CEN_PORT, PIN_FC41D_CEN_PIN);
    printk("fc41d-flash: VEN=1 CEN=1 — module released; press PC9 to "
           "pulse CEN for ltchiptool handshake\n");

    /* PC9 button is active-low (pull-up) per pin_map.h. Edge-detect on
     * a debounced press: first DEBOUNCE_TICKS-consecutive lows after
     * any high transition counts as one press. Holding the button
     * doesn't repeat — release + re-press is required. */
    int prev_committed = 0;   /* 0 = released, 1 = pressed */
    unsigned low_streak = 0;
    unsigned high_streak = 0;

    for (;;) {
        int low = (gpio_input_bit_get(PIN_BTN_PC9_PORT, PIN_BTN_PC9_PIN)
                   == RESET) ? 1 : 0;
        if (low) {
            high_streak = 0;
            if (low_streak < 0xFF) ++low_streak;
        } else {
            low_streak = 0;
            if (high_streak < 0xFF) ++high_streak;
        }

        if (!prev_committed && low_streak >= DEBOUNCE_TICKS) {
            prev_committed = 1;
            printk("fc41d-flash: PC9 pressed → CEN pulse\n");
            cen_pulse_low();
            /* Don't reset low_streak — the button is still down.
             * We'll wait for the high transition to re-arm. */
        } else if (prev_committed && high_streak >= DEBOUNCE_TICKS) {
            prev_committed = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void fc41d_flash_helper_create(void)
{
    TaskHandle_t h = NULL;
    xTaskCreate(fc41d_flash_helper_run,
                "fc41dfl",
                FC41D_FLASH_TASK_STACK_WORDS,
                NULL,
                FC41D_FLASH_TASK_PRIORITY,
                &h);
    stack_watch_register("fc41dfl", h, FC41D_FLASH_TASK_STACK_WORDS);
}
