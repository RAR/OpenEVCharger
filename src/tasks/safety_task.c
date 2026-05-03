#include "safety_task.h"
#include "../hal/wdg.h"
#include "../hal/uart.h"
#include "../hal/adc_inject.h"
#include "../core/j1772.h"
#include "../core/fault.h"
#include "../core/evse_state.h"
#include "../core/pin_map.h"
#include "../persist/crash_state.h"
#include "gd32f20x.h"

/* Debounce: 3 consecutive same-band reads at safety_task's 50 Hz tick
 * = 60 ms. Matches spec § 3. */
#define J1772_DEBOUNCE_N  3U

/* Relay weld detection: PB12 reads "closed" while PE12 is commanded
 * open for >= 200 ms (10 ticks at 50 Hz). Spec § 4 #2. */
#define WELD_PERSIST_TICKS  10U

static void evse_transition(evse_state_t *cur, evse_state_t next)
{
    if (*cur == next) return;
    printk("EVSE state %s -> %s\n", evse_state_name(*cur), evse_state_name(next));
    *cur = next;
}

static void check_safe_fail(fault_state_t *fs, evse_state_t *es)
{
    if (crash_state_is_safe_fail() &&
        !fault_is_active(fs, FAULT_CRASH_LOOP_SAFE_FAIL)) {
        if (fault_raise(fs, FAULT_CRASH_LOOP_SAFE_FAIL) == 1) {
            printk("FAULT raised: %s (first=%s)\n",
                   fault_name(FAULT_CRASH_LOOP_SAFE_FAIL),
                   fault_name(fs->first_raised));
        }
        evse_transition(es, EVSE_FAULT);
    }
}

/* PB12 read: HIGH = sensed-closed (relay coil energised + contacts mated),
 * LOW = sensed-open. (Pinout doc: "drive→1 made firmware's alarm routine
 * fire" — confirms HIGH means "closed-feedback".) */
static int relay_sense_closed(void)
{
    return (gpio_input_bit_get(PIN_RELAY_SENSE_PORT,
                               PIN_RELAY_SENSE_PIN) == SET) ? 1 : 0;
}

/* Returns 1 if the relay is currently being commanded closed. OpenBHZD
 * never commands close yet (PE12 driven LOW from gpio_init_all), so the
 * answer is unconditionally 0 — but expressed as a function so M7's
 * actuation work plugs in here cleanly. */
static int relay_commanded_closed(void)
{
    return 0;
}

static void check_relay_weld(fault_state_t *fs, evse_state_t *es,
                             int sensed_closed, int *weld_streak,
                             int *last_logged_sense)
{
    if (sensed_closed != *last_logged_sense) {
        printk("relay sense: %s (cmd=%s)\n",
               sensed_closed ? "CLOSED" : "open",
               relay_commanded_closed() ? "close" : "open");
        *last_logged_sense = sensed_closed;
    }

    if (sensed_closed && !relay_commanded_closed()) {
        if (*weld_streak < (int)WELD_PERSIST_TICKS) ++(*weld_streak);
    } else {
        *weld_streak = 0;
    }

    if (*weld_streak >= (int)WELD_PERSIST_TICKS &&
        !fault_is_active(fs, FAULT_RELAY_WELD)) {
        if (fault_raise(fs, FAULT_RELAY_WELD) == 1) {
            printk("FAULT raised: %s (sensed closed for >=%u ms while open-cmd)\n",
                   fault_name(FAULT_RELAY_WELD),
                   (unsigned)(WELD_PERSIST_TICKS * SAFETY_TASK_PERIOD_MS));
        }
        evse_transition(es, EVSE_FAULT);
    }
}

static void safety_task_run(void *arg)
{
    (void)arg;
    wdg_init();

    j1772_ctx_t cp;
    j1772_init(&cp);
    j1772_state_t last_logged_j1772 = J1772_STATE_INVALID;

    fault_state_t fs;
    fault_init(&fs);
    evse_state_t  es = EVSE_BOOT;
    int weld_streak = 0;
    int last_logged_sense = -1;     /* force initial print */

    /* Boot path: BOOT -> SELF_TEST -> (FAULT if safe-fail else READY).
     * SELF_TEST is a placeholder until M6.6 lands the real self-test. */
    evse_transition(&es, EVSE_SELF_TEST);
    check_safe_fail(&fs, &es);
    if (es != EVSE_FAULT) {
        evse_transition(&es, EVSE_READY);
    }

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        wdg_kick();

        int32_t cp_mv = cp_high_mv();
        j1772_state_t s = j1772_step(&cp, cp_mv, J1772_DEBOUNCE_N);

        if (s != last_logged_j1772 && s != J1772_STATE_INVALID) {
            printk("J1772 state=%s cp=%d mV\n",
                   j1772_state_name(s), (int)cp_mv);
            last_logged_j1772 = s;
        }

        check_safe_fail(&fs, &es);
        check_relay_weld(&fs, &es, relay_sense_closed(),
                         &weld_streak, &last_logged_sense);

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
