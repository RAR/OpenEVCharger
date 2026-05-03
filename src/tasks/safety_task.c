#include "safety_task.h"
#include "../hal/wdg.h"
#include "../hal/uart.h"
#include "../hal/adc_inject.h"
#include "../hal/adc_scan.h"
#include "../hal/cp_pwm.h"
#include "../hal/relay.h"
#include "../core/j1772.h"
#include "../core/fault.h"
#include "../core/evse_state.h"
#include "../core/pin_map.h"
#include "../persist/crash_state.h"
#include "../persist/event_log.h"
#include "persist_task.h"
#include "gd32f20x.h"

/* Debounce: 3 consecutive same-band reads at safety_task's 50 Hz tick
 * = 60 ms. Matches spec § 3. */
#define J1772_DEBOUNCE_N  3U

/* Relay weld detection: PB12 reads "closed" while PE12 is commanded
 * open for >= 200 ms (10 ticks at 50 Hz). Spec § 4 #2. */
#define WELD_PERSIST_TICKS  10U

/* CP=E classifier-output fault: J1772 state E sustained for 3 ticks
 * (60 ms). Spec § 4 #5. */
#define CP_E_PERSIST_TICKS  3U

/* Boot self-test ADC rail thresholds. spec § 4.1.1 says "non-rail
 * values"; we use 100..3995 / 4095 to leave headroom for noise. */
#define ST_ADC_MIN  100U
#define ST_ADC_MAX  3995U

/* CP state-A floor in mV; spec § 3 state table threshold for A. */
#define ST_CP_STATE_A_MIN_MV  10500

/* Relay actuate-and-readback self-test: spec § 4.1.4 step 4. Total
 * budget 50 ms with CP held in state A. Close, poll PB12 every 5 ms
 * up to 40 ms (typical mechanical pickup is 10-15 ms), open, poll
 * again up to 30 ms for release.
 *
 * Default OFF until the bench investigation in docs/bring-up.md
 * (M7.2) resolves: on the bench-tested ROC001 with no AC load, PE12
 * close cmd does not produce a PB12 = HIGH transition. Two
 * possibilities, neither yet ruled out:
 *
 *  1. Contactor coil is supplied from the AC primary side, so without
 *     mains the coil cannot energise.
 *  2. PB12 sense circuit only reports through-current presence, not
 *     coil state — so even with the coil energised, no AC = no sense
 *     transition.
 *
 * Either way, on a real installation with AC mains live this should
 * pass, so the spec-correct enable lives behind a compile-time flag
 * that can flip once the bench has been re-probed with mains live. */
#ifndef OPENBHZD_RELAY_ACTUATE_SELF_TEST
#define OPENBHZD_RELAY_ACTUATE_SELF_TEST  0
#endif

#define ST_RELAY_CLOSE_POLL_MS    5
#define ST_RELAY_CLOSE_POLLS      8     /* 40 ms */
#define ST_RELAY_OPEN_POLL_MS     5
#define ST_RELAY_OPEN_POLLS       6     /* 30 ms */

enum st_relay_result {
    ST_RELAY_OK              = 0,
    ST_RELAY_OPEN_AT_BOOT    = 1,
    ST_RELAY_WELD_AT_BOOT    = 2,
};

/* Map EVSE state -> CP output. Called once per transition.
 *
 * - FAULT      : drive CP to -12 V (J1772 state F, "EVSE not ready")
 * - CHARGING   : (M7 will drive advertised duty here; for now idle high)
 * - everything : idle HIGH (+12 V) — state-A-equivalent advertise
 *   else
 *
 * safety_task is the single owner of TIM1_CCR3 per spec § 4. */
static void evse_apply_cp(evse_state_t s)
{
    if (s == EVSE_FAULT) {
        cp_pwm_set_state_f();
    } else {
        cp_pwm_set_idle_high();
    }
}

static void evse_transition(evse_state_t *cur, evse_state_t next)
{
    if (*cur == next) return;
    printk("EVSE state %s -> %s\n", evse_state_name(*cur), evse_state_name(next));
    *cur = next;
    evse_apply_cp(next);
}

/* Post a fault-raise event to the W25Q event_log via persist_task.
 * Non-blocking; drops if persist queue is full (logged in
 * persist_task itself). Caller has just confirmed fault_raise()==1
 * (i.e., this is a true edge, not a re-raise). */
static void post_fault_event(fault_id_t fid, j1772_state_t js,
                             evse_state_t es, int32_t cp_mv)
{
    struct event_record rec = {
        .timestamp       = (uint32_t)xTaskGetTickCount(),
        .fault_id        = (uint16_t)fid,
        .j1772_state     = (uint8_t)js,
        .evse_state      = (uint8_t)es,
        .cp_mv           = (int16_t)cp_mv,
        .cc_amps         = 0,
        .ntc1_dC         = 0,
        .ntc2_dC         = 0,
        .active_amps_x10 = 0,
    };
    int rc = persist_post_event(&rec);
    if (rc != 0) {
        printk("safety: persist_post_event(%s) FAIL rc=%d\n",
               fault_name(fid), rc);
    }
}

static void check_safe_fail(fault_state_t *fs, evse_state_t *es,
                            j1772_state_t js, int32_t cp_mv)
{
    if (crash_state_is_safe_fail() &&
        !fault_is_active(fs, FAULT_CRASH_LOOP_SAFE_FAIL)) {
        if (fault_raise(fs, FAULT_CRASH_LOOP_SAFE_FAIL) == 1) {
            printk("FAULT raised: %s (first=%s)\n",
                   fault_name(FAULT_CRASH_LOOP_SAFE_FAIL),
                   fault_name(fs->first_raised));
            post_fault_event(FAULT_CRASH_LOOP_SAFE_FAIL, js, *es, cp_mv);
        }
        evse_transition(es, EVSE_FAULT);
    }
}

static void check_relay_weld(fault_state_t *fs, evse_state_t *es,
                             int sensed_closed, int *weld_streak,
                             int *last_logged_sense,
                             j1772_state_t js, int32_t cp_mv)
{
    if (sensed_closed != *last_logged_sense) {
        printk("relay sense: %s (cmd=%s)\n",
               sensed_closed ? "CLOSED" : "open",
               relay_main_commanded() ? "close" : "open");
        *last_logged_sense = sensed_closed;
    }

    if (sensed_closed && !relay_main_commanded()) {
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
            post_fault_event(FAULT_RELAY_WELD, js, *es, cp_mv);
        }
        evse_transition(es, EVSE_FAULT);
    }
}

/* --- Boot self-test (spec § 4.1, scoped subset) --------------------------
 *
 * Runs once between EVSE_SELF_TEST and EVSE_READY. Returns 0 on pass,
 * non-zero (number of failed sub-checks) on fail. On fail, the caller
 * raises FAULT_BOOT_SELF_TEST and routes to EVSE_FAULT.
 *
 * Sub-checks landed:
 *   1. ADC sanity on AC, CT, LCT, CPR ranks. NTC1/NTC2/CC/PE/BTN
 *      excluded — bench has those at rail by design (NTCs not
 *      populated, CC/PE high-impedance idle, BTN ladder rail when
 *      idle). Carve-out documented in pin_map / projectstate.
 *   2. PB12 (relay sense) reads "open" at boot.
 *   3. CP at idle reads in state-A band (≥ 10.5 V).
 *
 * Sub-checks deferred (need risk-controlled bench supervision):
 *   - GFCI CAL pulse + EXTI fire (no GFCI sense pin in pin map).
 *   - Relay actuate-and-readback (PE12 close + PB12 confirm) — risks
 *     contactor click on AC; M7 territory.
 *
 * boot_config CRC validation runs synchronously in main() pre-scheduler
 * via boot_config_load(); we treat that as already covered. */

static int self_test_adc_sanity(void)
{
    uint16_t b[ADC_RANKS];
    adc_scan_latest(b);
    static const uint8_t ranks[] = {
        ADC_RANK_AC, ADC_RANK_CT, ADC_RANK_LCT, ADC_RANK_CP,
    };
    int fails = 0;
    for (size_t i = 0; i < sizeof(ranks)/sizeof(ranks[0]); ++i) {
        unsigned r = ranks[i];
        if (b[r] < ST_ADC_MIN || b[r] > ST_ADC_MAX) {
            printk("self-test: ADC rank %u out of band (%u)\n", r, b[r]);
            ++fails;
        }
    }
    return fails;
}

static int self_test_relay_open(void)
{
    if (relay_main_sense_closed()) {
        printk("self-test: PB12 reads CLOSED at boot\n");
        return 1;
    }
    return 0;
}

static int self_test_cp_state_a(int32_t cp_mv)
{
    if (cp_mv < ST_CP_STATE_A_MIN_MV) {
        printk("self-test: CP idle not in state-A band (cp=%d mV)\n",
               (int)cp_mv);
        return 1;
    }
    return 0;
}

static int run_boot_self_test(int32_t cp_mv)
{
    int fails = 0;
    fails += self_test_adc_sanity();
    fails += self_test_relay_open();
    fails += self_test_cp_state_a(cp_mv);
    if (fails) {
        printk("self-test: %d sub-check(s) failed\n", fails);
    } else {
        printk("self-test: PASS\n");
    }
    return fails;
}

/* Returns ST_RELAY_OK, _OPEN_AT_BOOT, or _WELD_AT_BOOT. Caller must
 * gate on CP being in state A (no vehicle plugged) so the brief
 * close has no load consequence. */
static int self_test_relay_actuate(void)
{
    relay_main_close();
    int closed_seen = 0;
    for (int i = 0; i < ST_RELAY_CLOSE_POLLS; ++i) {
        vTaskDelay(pdMS_TO_TICKS(ST_RELAY_CLOSE_POLL_MS));
        if (relay_main_sense_closed()) { closed_seen = 1; break; }
    }
    relay_main_open();

    if (!closed_seen) {
        printk("self-test: PE12 close cmd but PB12 stayed OPEN -> RELAY_OPEN_AT_BOOT\n");
        return ST_RELAY_OPEN_AT_BOOT;
    }

    int open_seen = 0;
    for (int i = 0; i < ST_RELAY_OPEN_POLLS; ++i) {
        vTaskDelay(pdMS_TO_TICKS(ST_RELAY_OPEN_POLL_MS));
        if (!relay_main_sense_closed()) { open_seen = 1; break; }
    }

    if (!open_seen) {
        printk("self-test: PE12 open cmd but PB12 stayed CLOSED -> RELAY_WELD_AT_BOOT\n");
        return ST_RELAY_WELD_AT_BOOT;
    }
    printk("self-test: relay actuate-and-readback OK\n");
    return ST_RELAY_OK;
}

/* J1772 state -> EVSE state transitions. Spec § 3:
 *   READY    + J1772=C → CHARGING
 *   CHARGING + J1772≠C → READY (C->B regression is a transient pause;
 *                       relay opens immediately; allows re-progression)
 * Sticky: BOOT, SELF_TEST, FAULT do not transition out via this path.
 * USER_PAUSED / COOLING_DOWN are M8/M6.b respectively, not entered yet. */
static void update_evse_from_j1772(evse_state_t *es, j1772_state_t js)
{
    if (*es == EVSE_FAULT || *es == EVSE_BOOT || *es == EVSE_SELF_TEST) {
        return;
    }
    if (*es == EVSE_READY && js == J1772_STATE_C) {
        evse_transition(es, EVSE_CHARGING);
    } else if (*es == EVSE_CHARGING && js != J1772_STATE_C) {
        evse_transition(es, EVSE_READY);
    }
}

/* Per-tick relay-state owner. Closes the contactor only when the
 * vehicle is actively requesting current (J1772=C) AND no latched
 * fault is active AND we're in a charging-eligible EVSE state. Any
 * deviation opens immediately. Single-writer of PE12 per spec § 4. */
static void apply_relay_state(j1772_state_t js, evse_state_t es,
                              const fault_state_t *fs)
{
    int want_closed = (es == EVSE_CHARGING) &&
                      (js == J1772_STATE_C) &&
                      !fault_any_latched_active(fs);

    if (want_closed && !relay_main_commanded()) {
        printk("relay: close (J1772=C, EVSE=%s)\n", evse_state_name(es));
        relay_main_close();
    } else if (!want_closed && relay_main_commanded()) {
        printk("relay: open (J1772=%s, EVSE=%s, faults=0x%x)\n",
               j1772_state_name(js), evse_state_name(es),
               (unsigned)fs->active_bits);
        relay_main_open();
    }
}

static void check_cp_e(fault_state_t *fs, evse_state_t *es,
                       j1772_state_t js, int32_t cp_mv,
                       int *cp_e_streak)
{
    /* Don't run while we're driving state F ourselves: the M3
     * read-back calibration is one-sided (slope fit > 0 V only) so
     * cp_high_mv() reads ~+725 mV when we set CCR=0 → CP physically
     * -12 V. The classifier reports E in that case, which would
     * spuriously re-raise CP_NO_PILOT. Once we've already entered
     * EVSE_FAULT for any reason, suppress this check. */
    if (*es == EVSE_FAULT) {
        *cp_e_streak = 0;
        return;
    }

    if (js == J1772_STATE_E) {
        if (*cp_e_streak < (int)CP_E_PERSIST_TICKS) ++(*cp_e_streak);
    } else {
        *cp_e_streak = 0;
    }

    if (*cp_e_streak >= (int)CP_E_PERSIST_TICKS &&
        !fault_is_active(fs, FAULT_CP_NO_PILOT)) {
        if (fault_raise(fs, FAULT_CP_NO_PILOT) == 1) {
            printk("FAULT raised: %s (J1772=E for >=%u ticks, cp=%d mV)\n",
                   fault_name(FAULT_CP_NO_PILOT),
                   (unsigned)CP_E_PERSIST_TICKS, (int)cp_mv);
            post_fault_event(FAULT_CP_NO_PILOT, js, *es, cp_mv);
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
    int cp_e_streak = 0;

    /* Boot path: BOOT -> SELF_TEST -> READY (or FAULT on failure /
     * safe-fail). 100 ms warm-up gives ADC scan + injected ADC time to
     * converge and the J1772 classifier time to clear its 3-tick
     * debounce so the self-test reads stable values. Spec § 4.1
     * timing budget. */
    evse_transition(&es, EVSE_SELF_TEST);

    int32_t cp_mv0 = 0;
    j1772_state_t js0 = J1772_STATE_INVALID;
    for (int i = 0; i < 5; ++i) {
        cp_mv0 = cp_high_mv();
        js0 = j1772_step(&cp, cp_mv0, J1772_DEBOUNCE_N);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    int st_fails = run_boot_self_test(cp_mv0);
    if (st_fails > 0 && !fault_is_active(&fs, FAULT_BOOT_SELF_TEST)) {
        if (fault_raise(&fs, FAULT_BOOT_SELF_TEST) == 1) {
            printk("FAULT raised: %s (%d sub-check fails)\n",
                   fault_name(FAULT_BOOT_SELF_TEST), st_fails);
            post_fault_event(FAULT_BOOT_SELF_TEST, js0, es, cp_mv0);
        }
        evse_transition(&es, EVSE_FAULT);
    }

    /* Spec § 4.1.4 step 4: relay actuate-and-readback. Only run with
     * CP in state A (no vehicle plugged) so the brief close is a
     * no-op on the J1772 plug side. Skip if anything is connected. */
#if OPENBHZD_RELAY_ACTUATE_SELF_TEST
    if (js0 == J1772_STATE_A) {
        int relay_st = self_test_relay_actuate();
        if (relay_st == ST_RELAY_OPEN_AT_BOOT &&
            !fault_is_active(&fs, FAULT_RELAY_OPEN_AT_BOOT)) {
            if (fault_raise(&fs, FAULT_RELAY_OPEN_AT_BOOT) == 1) {
                printk("FAULT raised: %s\n",
                       fault_name(FAULT_RELAY_OPEN_AT_BOOT));
                post_fault_event(FAULT_RELAY_OPEN_AT_BOOT, js0, es, cp_mv0);
            }
            evse_transition(&es, EVSE_FAULT);
        } else if (relay_st == ST_RELAY_WELD_AT_BOOT &&
                   !fault_is_active(&fs, FAULT_RELAY_WELD_AT_BOOT)) {
            if (fault_raise(&fs, FAULT_RELAY_WELD_AT_BOOT) == 1) {
                printk("FAULT raised: %s\n",
                       fault_name(FAULT_RELAY_WELD_AT_BOOT));
                post_fault_event(FAULT_RELAY_WELD_AT_BOOT, js0, es, cp_mv0);
            }
            evse_transition(&es, EVSE_FAULT);
        }
    } else {
        printk("self-test: skipping relay actuate (J1772=%s, not A)\n",
               j1772_state_name(js0));
    }
#else
    printk("self-test: relay actuate test DISABLED at build time "
           "(OPENBHZD_RELAY_ACTUATE_SELF_TEST=0; bench carve-out)\n");
    (void)self_test_relay_actuate;     /* avoid -Wunused-function */
#endif

    check_safe_fail(&fs, &es, js0, cp_mv0);
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

        check_safe_fail(&fs, &es, s, cp_mv);
        check_relay_weld(&fs, &es, relay_main_sense_closed(),
                         &weld_streak, &last_logged_sense,
                         s, cp_mv);
        check_cp_e(&fs, &es, s, cp_mv, &cp_e_streak);

        /* After all fault checks: classifier-driven EVSE transitions
         * and relay state. Faults preempt — both helpers honor
         * EVSE_FAULT as sticky. */
        update_evse_from_j1772(&es, s);
        apply_relay_state(s, es, &fs);

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
