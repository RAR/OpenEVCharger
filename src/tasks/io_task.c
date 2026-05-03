#include "io_task.h"
#include "core/pin_map.h"
#include "core/system_state.h"
#include "core/evse_state.h"
#include "core/fault.h"
#include "ui/buttons.h"
#include "ui/buzzer.h"
#include "ui/led_patterns.h"
#include "hal/uart.h"
#include "hal/adc_scan.h"
#include "hal/adc_inject.h"
#include "hal/ws2812.h"
#include "tasks/persist_task.h"
#include "diag/stack_watch.h"
#include "gd32f20x.h"

#define IO_TICK_MS        50
#define HB_TOGGLE_MS      500
#define DUMP_MS           5000
#define ALIVE_MARKER_MS   60000U
#define LED_BRIGHTNESS_PCT  60u
#define STACK_DUMP_MS     30000U

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
    buzzer_init();
    ws2812_init();

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

    /* Track EVSE transitions to fire buzzer one-shots. */
    evse_state_t prev_evse = EVSE_BOOT;
    int boot_buzz_armed   = 1;
    int prev_charging     = 0;
    uint32_t prev_fault   = 0;

    for (;;) {
        if ((ms % HB_TOGGLE_MS) == 0) {
            hb_level ^= 1;
            if (hb_level) gpio_bit_set(PIN_HEARTBEAT_PORT, PIN_HEARTBEAT_PIN);
            else          gpio_bit_reset(PIN_HEARTBEAT_PORT, PIN_HEARTBEAT_PIN);
        }

        buttons_poll();

        struct openbhzd_state s = system_state_snapshot();

        /* Buzzer one-shots on EVSE transitions / fault edges. */
        if (boot_buzz_armed && s.evse_state != EVSE_BOOT &&
            s.evse_state != EVSE_SELF_TEST) {
            buzzer_set_pattern(s.evse_state == EVSE_FAULT
                                   ? BUZ_BOOT_FAIL
                                   : BUZ_BOOT_PASS);
            boot_buzz_armed = 0;
        }
        int charging_now = (s.evse_state == EVSE_CHARGING);
        if (charging_now && !prev_charging) buzzer_set_pattern(BUZ_SESSION_START);
        if (!charging_now && prev_charging) buzzer_set_pattern(BUZ_SESSION_END);
        prev_charging = charging_now;

        if (s.fault_active_bits && !prev_fault) {
            int gfci = (s.fault_active_bits >> FAULT_GFCI) & 1u;
            buzzer_set_pattern(gfci ? BUZ_FAULT_GFCI : BUZ_FAULT_NON_GFCI);
        } else if (!s.fault_active_bits && prev_fault) {
            buzzer_set_pattern(BUZ_OFF);
        }
        prev_fault = s.fault_active_bits;
        prev_evse  = (evse_state_t)s.evse_state;
        (void)prev_evse;

        buzzer_tick(ms);

        /* LED render — every ~60 ms is plenty for human eye. Strip
         * driver is wired up but bench protocol/electrical match is
         * still an open question (see projectstate "M9 LED strip
         * mystery"); render runs regardless so the moment the right
         * pin/timing/pack/level combination is found the colours fall
         * into place. */
        if ((ms % 60u) == 0) {
            uint8_t ovr_mode = 0;
            uint8_t ovr_rgb[3] = {0, 0, 0};
            led_override_get(&ovr_mode, ovr_rgb);

            struct led_inputs in = {
                .evse              = (evse_state_t)s.evse_state,
                .j1772             = (j1772_state_t)s.j1772_state,
                .fault_active_bits = s.fault_active_bits,
                .comms_degraded    = 0,
                .brightness_pct    = LED_BRIGHTNESS_PCT,
                .override_mode     = ovr_mode,
                .override_rgb      = { ovr_rgb[0], ovr_rgb[1], ovr_rgb[2] },
            };
            if (!ws2812_busy()) led_render(&in, ms);
        }

        if ((ms % DUMP_MS) == 0) adc_dump();
        if ((ms % STACK_DUMP_MS) == 0) stack_watch_dump();

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
    TaskHandle_t h = NULL;
    xTaskCreate(io_task_run,
                "io",
                IO_TASK_STACK_WORDS,
                NULL,
                IO_TASK_PRIORITY,
                &h);
    stack_watch_register("io", h, IO_TASK_STACK_WORDS);
}
