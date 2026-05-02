#include "safety_task.h"
#include "../hal/wdg.h"
#include "../hal/uart.h"
#include "../hal/adc_inject.h"
#include "../core/j1772.h"

/* Debounce: 3 consecutive same-band reads at safety_task's 50 Hz tick
 * = 60 ms. Matches spec § 3. */
#define J1772_DEBOUNCE_N  3U

static void safety_task_run(void *arg)
{
    (void)arg;
    wdg_init();

    j1772_ctx_t cp;
    j1772_init(&cp);
    j1772_state_t last_logged = J1772_STATE_INVALID;

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        wdg_kick();

        int32_t cp_mv = cp_high_mv();
        j1772_state_t s = j1772_step(&cp, cp_mv, J1772_DEBOUNCE_N);

        if (s != last_logged && s != J1772_STATE_INVALID) {
            printk("J1772 state=%s cp=%d mV\n",
                   j1772_state_name(s), (int)cp_mv);
            last_logged = s;
        }

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
