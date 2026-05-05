#include "safety_task.h"
#include "../hal/wdg.h"
#include "../hal/uart.h"
#include "../hal/adc_inject.h"
#include "../hal/adc_scan.h"
#include "../hal/cp_pwm.h"
#include "../hal/relay.h"
#include "../hal/gfci.h"
#include "../hal/bl0939.h"
#include "../hal/rfid.h"
#include "../core/j1772.h"
#include "../core/fault.h"
#include "../core/evse_state.h"
#include "../core/over_temp.h"
#include "../core/rfid.h"
#include "../core/pin_map.h"
#include "../persist/crash_state.h"
#include "../persist/event_log.h"
#include "../persist/session_log.h"
#include "../persist/boot_config.h"
#include "../persist/calibration.h"
#include "../persist/rfid_authlist.h"
#include "../proto/commands.h"
#include "../core/system_state.h"
#include "persist_task.h"
#include "comms_task.h"
#include "../diag/stack_watch.h"
#include "queue.h"
#include "gd32f20x.h"

/* Cross-task control inbox (TLV CLEAR_FAULT / pause / resume). */
typedef enum {
    SAFETY_REQ_CLEAR_FAULT = 0,
    SAFETY_REQ_USER_PAUSE,
    SAFETY_REQ_USER_RESUME,
    SAFETY_REQ_RFID_LEARN_ARM,
    SAFETY_REQ_PUBLISH_RFID_CONFIG,
} safety_req_type_t;

struct safety_req {
    uint8_t  type;
    uint8_t  arg_u8;
    uint8_t  pad[2];
    uint32_t arg_u32;
};

static QueueHandle_t s_safety_inbox;

/* RFID learn-mode countdown. Set by SAFETY_REQ_RFID_LEARN_ARM, decremented
 * by the swipe block each tick, cleared on capture. Single-writer
 * (safety_task body) so no atomicity concerns. */
static uint16_t s_rfid_learn_ticks = 0;

/* Per-session "user has authorized this session" flag. Set when a
 * matched authorized-tag swipe lands; cleared on plug-removal
 * (J1772=A). Read by update_evse_from_j1772 to gate READY → CHARGING
 * when boot_config_require_rfid_auth() is set. Always reads as
 * authorized when the require-auth flag is OFF. */
static uint8_t s_session_authorized = 0;

/* Per-session soft-OC derate. Subtracted from effective_advertised_amps()
 * by check_soft_over_current after a sustained 1.05× threshold cross.
 * Floor at SOC_DERATE_FLOOR_A (J1772 minimum); reset on session end so
 * a fresh charge starts at the user-configured rate. Sticky within a
 * session — we don't auto-restore advertised on a transient cool-off,
 * spec semantic is "ramp duty −10%" and stay there until the user /
 * CSMS re-issues. */
static uint8_t s_soc_derate_amps = 0;

/* Forward decl — session_end() lives upstream of the soft-OC detector
 * that owns the accumulator. */
static void clear_soc_derate_for_session_end(void);

static void publish_rfid_config(void);

/* Debounce: 3 consecutive same-band reads at safety_task's 50 Hz tick
 * = 60 ms. Matches spec § 3. */
#define J1772_DEBOUNCE_N  3U

/* Relay weld detection: sense reads "closed" while commanded open
 * for >= 200 ms. Spec § 4 #2.
 * Relay stuck-open detection: sense reads "open" while commanded
 * closed for >= 200 ms. Spec § 4 #3.
 *
 * Both detectors require a working closed-feedback signal. PB12 was
 * misidentified as that sense — it's actually the FORCE-OPEN LATCH
 * output (see hal/relay.c). PB0/NTC2 was the next candidate but bench
 * data showed it isn't relay-correlated either. Until we find the
 * real sense, the detectors are gated off via the build flag below.
 * Default 0 (silent) — no false positives, but also no weld/stuck
 * detection. Production deployments must identify the closed-feedback
 * signal AND set =1 before charging is safe under fault scenarios. */
#define WELD_PERSIST_TICKS         10U
#define STUCK_OPEN_PERSIST_TICKS   10U

#ifndef OPENBHZD_RELAY_FEEDBACK_KNOWN
#define OPENBHZD_RELAY_FEEDBACK_KNOWN 0
#endif

/* CP=E classifier-output fault: J1772 state E sustained for 3 ticks
 * (60 ms). Spec § 4 #5. */
#define CP_E_PERSIST_TICKS  3U

/* BL0939 RMS registers update on a 400-800 ms internal cadence per
 * datasheet § 3.2.4. Polling faster than 400 ms repeats stale data and
 * wastes ~1 ms of bit-banged SPI per round. 20 ticks @ 20 ms = 400 ms.
 * We poll at the END of the tick so the safety/CP/relay path runs
 * first on every tick — telemetry update is best-effort. */
#define BL0939_POLL_TICKS  20U

/* Stock fw cadence is ~3 Hz for the RFID keepalive. 17 ticks × 20 ms
 * = 340 ms, close enough. The module is silent without our keepalive
 * (request/response, not auto-stream — see docs/mcu-re/rfid-protocol.md). */
#define RFID_KEEPALIVE_TICKS  17U

/* Learn-mode auto-disarm: after this many ticks the next swipe falls
 * back to the lookup path. 1500 ticks @ 20 ms = 30 s — long enough for
 * the user to walk over to the reader, short enough that a forgotten
 * arm-and-walk-away can't quietly add a stranger's tag the next time
 * someone swipes. */
#define RFID_LEARN_TICKS  1500U

/* BL0939-derived detector debounce. Each "tick" here is a successful
 * poll cycle, not a safety tick — so 3 means ~1.2 s sustained
 * condition. Gives plenty of margin against AC zero-crossing
 * transients and the chip's own 400-800 ms RMS update window. */
#define BL0939_DETECTOR_PERSIST  3U

/* AC presence threshold — V_RMS *raw* below this means the chip is
 * not seeing mains. Bench reading at 120 V was 0x208cae (≈ 2.13 M);
 * 0x080000 (≈ 524 k) is well below any plausible mains and well
 * above the no-AC noise floor. Tune once we've bench-tested with
 * a brown-out scenario. */
#define BL0939_V_RMS_AC_PRESENT_RAW  0x00080000U

/* IA_RMS *raw* current-flow threshold for relay weld / stuck-open.
 * Bench no-load IA_RMS = 0x000a70 (= 2672); choosing 0x002000 (= 8192)
 * = ~3× no-load offset. A real load draws orders-of-magnitude more
 * raw counts than this, so the signal is unambiguous. Tune lower once
 * we've validated against a low-current bench load (e.g. 1 A LED
 * dummy). */
#define BL0939_IA_RMS_FLOW_RAW       0x00002000U

/* Hard / soft over-current thresholds. Both ride on top of a per-
 * chassis raw → mA scale (see calibration.c). Until the scale is
 * non-zero, both detectors are silent.
 *
 * Soft OC trips per spec § 4 #11: > advertised × 1.05 sustained for
 * 30 s → ramp advertised duty -10% and log; only raise the fault
 * once the ramp has bottomed at the J1772 6 A floor and we're still
 * over. Persist counter is in BL0939 poll cycles (one per 400 ms),
 * so 75 polls = 30 s.
 *
 * Hard OC trips at (DIP1 / hw cap) × 1.25 — anything past that is
 * broken hardware, no derate, latched. */
#define BL0939_SOC_TOL_NUM           105U   /* / 100 = 1.05× advertised */
#define BL0939_SOC_TOL_DEN           100U
#define BL0939_SOC_PERSIST_POLLS      75U   /* 75 × 400 ms = 30 s */
#define SOC_DERATE_FLOOR_A             6U   /* J1772 minimum advertised */
#define BL0939_HOC_TOL_NUM           125U   /* / 100 = 1.25× hw cap */
#define BL0939_HOC_TOL_DEN           100U

/* GFCI fault sense (PE2 LOW) sustained for this many ticks before
 * raising. 3 ticks @ 20 ms = 60 ms — well under UL2231's 25 ms +
 * upstream contactor-open budget for the trip path, but long enough
 * to ride out coupling glitches like the one we saw on PD6 during
 * the bench wiggle. */
#define GFCI_PERSIST_TICKS  3U

/* PE continuity sense (PC5, ADC_RANK_PE). When the protective-earth
 * wire is intact the divider pulls PC5 to ~0 V (bench: raw 0..3 with
 * AC absent, single-digit raw with AC live). A broken / disconnected
 * PE lets the node float toward the rail or pick up mains-coupled
 * noise. Spec § 4 #4: raise FAULT_PE_CONTINUITY when PC5 is "out of
 * expected band" for >10 ticks (200 ms @ 50 Hz).
 *
 * Threshold raw 400 ≈ 0.32 V is well above any plausible
 * intact-PE reading and well below what a floating input would
 * settle to. Tune once we've bench-tested with the PE wire
 * intentionally lifted. */
#define PE_OK_RAW_MAX        400U
#define PE_PERSIST_TICKS     10U

/* Runtime ADC sanity (spec § 4 #8). Same band as the boot self-test
 * (ST_ADC_MIN..ST_ADC_MAX) but checked every tick against the safety-
 * relevant analog rails. >5 consecutive ticks any-rank-rail = stuck
 * mux / broken op-amp / sheared ADC reference → latched fault.
 *
 * Channels excluded from runtime sanity:
 *   - PA7 (CC) rails high when nothing is plugged in (J1772 spec)
 *   - PC5 (PE) rails low when bonded to mains earth (covered by
 *     check_pe_continuity, which is the inverse semantic)
 *   - PB0 (NTC2) is non-thermistor and reads 0 raw on this PCB
 *   - PA4 (CP) rails low while we drive state F (in EVSE_FAULT only,
 *     and the detector self-suppresses during EVSE_FAULT) */
#define ADC_RUNTIME_PERSIST_TICKS  5U

/* CC (Control Cable / cable-rating) ladder on PA7 (ADC_RANK_CC).
 * SAE J1772 § 5: cable carries a CC resistor between CC and GND;
 * EVSE has a pull-up to 3.3 V. Resistor value encodes the cable's
 * max amperage. Standard ladder + the EVSE divider (assumed 330 Ω
 * pull-up — same magnitude the OEM uses on the analogous CP read
 * divider; **bench-characterise with a resistor decade to tune**):
 *
 *   1500 Ω → 13 A → raw ≈ 3360
 *    680 Ω → 20 A → raw ≈ 2757
 *    220 Ω → 32 A → raw ≈ 1638
 *    100 Ω → 63 A → raw ≈  955
 *    open  → no cable / wire severed → raw ≈ 4095
 *    short → broken cable → raw ≈ 0
 *
 * Bands picked with ~300-raw guard either side of each centre.
 * Re-fit once bench data lands (workflow: plug a J1772 with a
 * known CC resistor / decade, capture PA7 raw at each rung, write
 * tighter band edges). Per spec § 3 the ladder is 13/20/32/40/80;
 * 40/80 don't have canonical SAE resistor values so this map keeps
 * the standard 13/20/32/63 bands and treats anything else as
 * out-of-range. */
#define CC_BAND_OPEN_MIN_RAW   3700U
#define CC_BAND_13A_LO         3060U
#define CC_BAND_20A_LO         2400U
#define CC_BAND_32A_LO         1300U
#define CC_BAND_63A_LO          600U
#define CC_PERSIST_TICKS         10U   /* 200 ms debounce */

/* Bench unit on 2026-05-04 reads PA7 raw ≈ 12 with no J1772 plug
 * inserted, contradicting the M2 bring-up reading of raw=4095. The
 * OEM CC divider topology is therefore different from the standard
 * SAE pull-up-to-3.3V assumption baked into the band map above —
 * "no cable" appears to read at the LOW rail, not the high rail.
 * Until a bench characterisation pass with a known-resistor decade
 * pins down the actual map, leave the FAULT_CC_OUT_OF_RANGE raise
 * path gated. The decoder still runs (publishes cc_max_amps for
 * diagnostic) so HA can log raw vs decoded as we capture data. */
#ifndef OPENBHZD_CC_DETECTOR
#define OPENBHZD_CC_DETECTOR  0
#endif

/* GFCI CAL self-test (spec § 4.1.2). Pulses PE3 to inject a synthetic
 * fault into the GFCI module's CAL input, expects PE2 to assert the
 * fault sense within ~50 ms, then de-asserts and verifies the sense
 * recovers. Gated default OFF until bench-validated on this PCB —
 * the wire-trace on PE3 polarity is contradicted by the inline
 * pin_map.h comment, and the CAL-→-sense path hasn't been
 * empirically demonstrated. The hal/gfci.c implementation is
 * polarity-agnostic, but a wrong PCB topology (e.g. CAL not actually
 * wired to the module) would always fail. Set =1 once bench
 * confirms the path on a real unit. */
#ifndef OPENBHZD_GFCI_CAL_SELF_TEST
#define OPENBHZD_GFCI_CAL_SELF_TEST  0
#endif

/* Asymmetric hysteresis on the READY → CHARGING transition: require
 * J1772 state C confirmed for an additional 50 ticks (1 s at the
 * 50 Hz safety tick) before closing the relay. Prevents rapid
 * relay-cycle / session-log thrash from CP-flap (transient fault on
 * the CP wire, indecisive EV, hand-toggled resistor on the bench).
 *
 * 1 s is ~10× any realistic CP transient and well within J1772's
 * 3 s response-time window for state C. Real EVs hold C indefinitely
 * once requesting charge so the dwell is invisible during normal
 * operation.
 *
 * The reverse transition (CHARGING → READY when J1772 leaves C) is
 * deliberately UN-hysteresised — opening the contactor must be
 * immediate per safety priorities. Spec § 3 / 4. */
#define EVSE_C_DWELL_TICKS  50U

/* Minimum session duration to persist to session_log (ms). Sessions
 * shorter than this still emit the SESSION_ENDED TLV event but skip
 * the W25Q write — protects against flash wear from CP-flap
 * scenarios. 2 s is a sane bench / production-fault threshold; real
 * charging sessions are minutes. */
#define SESSION_MIN_PERSIST_MS  2000U

/* Boot self-test ADC rail thresholds. spec § 4.1.1 says "non-rail
 * values"; we use 100..3995 / 4095 to leave headroom for noise. */
#define ST_ADC_MIN  100U
#define ST_ADC_MAX  3995U

/* CP "pilot present" floor in mV. The boot self-test must accept any
 * J1772 band with a working pilot wire (A/B/C/D — any positive
 * voltage above ~1.5 V), because the unit may power on with a vehicle
 * already plugged in. Only states E (cp ≈ 0 V, no diode/no pilot) and
 * F (cp negative) should trip the self-test. Spec § 3 state-D floor
 * is 1500 mV. */
#define ST_CP_PILOT_PRESENT_MV  1500

/* Hardware advertise caps. Polarity inverted 2026-05-05 vs the
 * stock spec § 3 — bench-observed (dip=0011 from straps) maps to
 * DIP1=closed but the operator wanted the higher 48 A cap. The
 * physical switch is now treated as "set when closed = full 48 A,
 * cleared when open = limit 40 A":
 *   - DIP1 closed → 48 A; DIP1 open → 40 A
 *   - Hardware contactor rating: 48 A
 *   - FC41D-requested amps clamps to min(DIP1, hw cap, fc41d). */
#define HW_AMPS_MAX        48U
#define DIP1_AMPS_CLOSED   48U
#define DIP1_AMPS_OPEN     40U

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
/* M7.2 — gated off by default. Polarity inversion landed in M7.b
 * (PE12 HIGH = force open). Re-enable only after bench confirms the
 * inverted-polarity actuate-and-readback works without welding the
 * contactor. */
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

/* Session tracking — populated only between READY→CHARGING and
 * CHARGING→{anything else} transitions. Energy delivered is integrated
 * from BL0939 active-power readings each poll cycle (gated on the
 * relay being commanded closed). When the BL0939 chassis scale is
 * uncalibrated (calibration_bl0939_pa_mw_per_raw == 0), the
 * accumulator stays 0 — uncalibrated chargers report no kWh rather
 * than spurious totals. */
typedef enum {
    SESSION_END_NORMAL    = 0,   /* J1772 dropped from C cleanly */
    SESSION_END_FAULT     = 1,   /* entered EVSE_FAULT */
    SESSION_END_OTHER     = 2,
} session_end_reason_t;

static struct {
    int      active;
    uint32_t start_ts;
    uint8_t  j1772_max;
    uint16_t fault_count;
    uint16_t max_temp_dC;
    int64_t  mws_accum;          /* integrated milli-watt-seconds (mJ) */
} s_session;

static void session_start(void)
{
    s_session.active       = 1;
    s_session.start_ts     = (uint32_t)xTaskGetTickCount();
    s_session.j1772_max    = (uint8_t)J1772_STATE_C;
    s_session.fault_count  = 0;
    s_session.max_temp_dC  = 0;
    s_session.mws_accum    = 0;
    printk("session: start ts=%u\n", (unsigned)s_session.start_ts);
    (void)comms_publish_event(EVT_SESSION_BEGAN,
                              &s_session.start_ts,
                              sizeof(s_session.start_ts));
}

static void session_end(session_end_reason_t reason)
{
    if (!s_session.active) return;
    uint32_t now = (uint32_t)xTaskGetTickCount();
    uint32_t dur_ms = (uint32_t)(now - s_session.start_ts) *
                      portTICK_PERIOD_MS;

    /* Skip the session_log W25Q write for sessions shorter than the
     * min-persist threshold AND with no faults raised — these are
     * almost certainly CP-flap artifacts, not real charging. The
     * SESSION_ENDED TLV event still emits so consumers can see the
     * activity. Sessions that hit a fault always persist regardless
     * of duration so the post-mortem trail is intact. */
    int persist = (dur_ms >= SESSION_MIN_PERSIST_MS) ||
                  (s_session.fault_count > 0u) ||
                  (reason == SESSION_END_FAULT);

    /* mWs accumulator → mWh (1 Wh = 3600 J). Clamp to u32 max so a
     * runaway BL0939 cal value doesn't wrap; saturating is a clearer
     * signal than rolling. */
    int64_t mwh64 = (s_session.mws_accum > 0)
                       ? (s_session.mws_accum / 3600)
                       : 0;
    if (mwh64 > 0xFFFFFFFFLL) mwh64 = 0xFFFFFFFFLL;
    uint32_t session_mwh = (uint32_t)mwh64;

    struct session_record rec = {
        .start_ts             = s_session.start_ts,
        .end_ts               = now,
        .mwh_delivered        = session_mwh,
        .end_reason           = (uint8_t)reason,
        .j1772_max_state_seen = s_session.j1772_max,
        .fault_count          = s_session.fault_count,
        .max_temp_dC          = s_session.max_temp_dC,
    };
    int rc = persist ? persist_post_session(&rec) : 0;
    printk("session: end reason=%u dur_ms=%u faults=%u %s rc=%d\n",
           (unsigned)reason,
           (unsigned)dur_ms,
           (unsigned)s_session.fault_count,
           persist ? "persisted" : "skipped",
           rc);
    /* TLV event: u32 mwh + u32 dur_ms + u8 reason. */
    struct __attribute__((packed)) {
        uint32_t mwh;
        uint32_t dur_ms;
        uint8_t  reason;
    } evt = {
        .mwh    = rec.mwh_delivered,
        .dur_ms = now - s_session.start_ts,
        .reason = (uint8_t)reason,
    };
    (void)comms_publish_event(EVT_SESSION_ENDED, &evt, sizeof(evt));
    s_session.active = 0;
    clear_soc_derate_for_session_end();
}

/* Effective advertised amps = min(FC41D, DIP1 cap, hardware cap),
 * minus any in-session soft-OC derate. FC41D=0 means "unset" → fall
 * back to DIP1 cap. DIP1 input is pull-up, active-low (closed switch
 * reads LOW). Spec § 3 + § 4 #11. */
static uint8_t effective_advertised_amps(void)
{
    int dip1_closed = (gpio_input_bit_get(PIN_DIP1_PORT, PIN_DIP1_PIN) == RESET) ? 1 : 0;
    uint8_t hw_cap   = HW_AMPS_MAX;
    uint8_t dip1_cap = dip1_closed ? DIP1_AMPS_CLOSED : DIP1_AMPS_OPEN;
    uint8_t cap = (dip1_cap < hw_cap) ? dip1_cap : hw_cap;
    uint8_t fc  = boot_config_advertised_amps();
    uint8_t base = (fc == 0U) ? cap : ((fc < cap) ? fc : cap);
    uint8_t derate = s_soc_derate_amps;
    if (derate >= base) return SOC_DERATE_FLOOR_A;
    uint8_t derated = base - derate;
    return (derated < SOC_DERATE_FLOOR_A) ? SOC_DERATE_FLOOR_A : derated;
}

/* Per-tick CP output dispatch. Spec § 3 PWM-duty-vs-state table:
 *   J1772=A     → idle high (+12 V), 0% advertise
 *   J1772=B/C/D → advertised amps duty
 *   J1772=E     → 0% (idle high) — relay open by other path
 *   J1772=F     → state-F (-12 V) — but we only drive F on FAULT
 *   EVSE=FAULT  → state-F regardless of J1772
 *   EVSE=USER_PAUSED → idle high (no advertise; tells the EV that
 *                       charging isn't currently available)
 *   EVSE=COOLING_DOWN → idle high (same semantic: present but not
 *                       offering charge — transient thermal pause)
 *
 * safety_task is the single owner of TIM1_CCR3 per spec § 4. */
static void apply_cp_for_state(evse_state_t es, j1772_state_t js)
{
    if (es == EVSE_FAULT) {
        cp_pwm_set_state_f();
        return;
    }
    if (es == EVSE_COOLING_DOWN) {
        cp_pwm_set_idle_high();
        return;
    }
    if (es == EVSE_USER_PAUSED) {
        cp_pwm_set_idle_high();
        return;
    }
    if (js == J1772_STATE_B || js == J1772_STATE_C || js == J1772_STATE_D) {
        cp_pwm_set_advertise_amps(effective_advertised_amps());
    } else {
        cp_pwm_set_idle_high();
    }
}

static void evse_transition(evse_state_t *cur, evse_state_t next)
{
    if (*cur == next) return;
    printk("evse: state %s -> %s\n", evse_state_name(*cur), evse_state_name(next));

    /* Session lifecycle around CHARGING entry/exit. */
    if (next == EVSE_CHARGING && *cur != EVSE_CHARGING) {
        session_start();
    } else if (*cur == EVSE_CHARGING && next != EVSE_CHARGING) {
        session_end((next == EVSE_FAULT) ? SESSION_END_FAULT
                                         : SESSION_END_NORMAL);
    }

    *cur = next;
    if (next == EVSE_FAULT) {
        /* FAULT entry must drive CP to state F immediately (don't wait
         * for the next 20 ms tick). Other states refresh per-tick from
         * apply_cp_for_state(). */
        cp_pwm_set_state_f();
        /* Hard-open the contactor and arm the redundant force-open
         * latch (PB12) per spec § 4 / pin_map.h. PE12 is already LOW
         * (apply_relay_state will keep it that way), but if the PE12
         * driver were stuck HIGH from any cause, asserting PB12 HIGH
         * forces the contactor open via the hardware latch. */
        relay_main_open();
        relay_force_open_latch();
    } else if (*cur != next && relay_force_open_active()) {
        /* Leaving FAULT (typically via CLEAR_FAULT → READY): drop the
         * force-open assert so future close commands aren't latched
         * out. PE12 is currently LOW so the latch is already disarmed
         * mechanically; this just cleans up the GPIO state. */
        relay_force_open_release();
    }
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
    if (s_session.active &&
        s_session.fault_count < (uint16_t)0xFFFFu) {
        s_session.fault_count++;
    }

    /* TLV event: u32 fault_id + u8 j1772 + u8 evse + i16 cp_mv. */
    struct __attribute__((packed)) {
        uint32_t fault_id;
        uint8_t  j1772_state;
        uint8_t  evse_state;
        int16_t  cp_mv;
    } evt = {
        .fault_id    = (uint32_t)fid,
        .j1772_state = (uint8_t)js,
        .evse_state  = (uint8_t)es,
        .cp_mv       = (int16_t)cp_mv,
    };
    (void)comms_publish_event(EVT_FAULT_RAISED, &evt, sizeof(evt));
}

static void check_safe_fail(fault_state_t *fs, evse_state_t *es,
                            j1772_state_t js, int32_t cp_mv)
{
    if (crash_state_is_safe_fail() &&
        !fault_is_active(fs, FAULT_CRASH_LOOP_SAFE_FAIL)) {
        if (fault_raise(fs, FAULT_CRASH_LOOP_SAFE_FAIL) == 1) {
            printk("fault: raised %s (first=%s)\n",
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
        printk("relay: sense %s (cmd=%s)\n",
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
            printk("fault: raised %s (sensed closed for >=%u ms while open-cmd)\n",
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

static int self_test_cp_pilot_present(int32_t cp_mv)
{
    /* Accept any J1772 band with positive pilot voltage (state A, B,
     * C, or D). Reject only E (no pilot / diode missing) or F
     * (negative). This lets the unit boot cleanly with a vehicle
     * already plugged in. */
    if (cp_mv < ST_CP_PILOT_PRESENT_MV) {
        printk("self-test: CP pilot not present (cp=%d mV; expected >= %d mV)\n",
               (int)cp_mv, ST_CP_PILOT_PRESENT_MV);
        return 1;
    }
    return 0;
}

static int run_boot_self_test(int32_t cp_mv)
{
    int fails = 0;
    fails += self_test_adc_sanity();
    fails += self_test_relay_open();
    fails += self_test_cp_pilot_present(cp_mv);
    if (fails) {
        printk("self-test: %d sub-check(s) failed\n", fails);
    } else {
        printk("self-test: PASS\n");
    }
    return fails;
}

/* GFCI CAL self-test wrapper (spec § 4.1.2). Runs as a separate boot
 * step (not folded into run_boot_self_test) because the test pulse
 * actively drives PE3 + flips PE2 LOW for ~50 ms, which would race
 * the live check_gfci detector once the safety loop starts ticking.
 * Has to fire AFTER gfci_init() and BEFORE the for(;;) loop. Returns
 * 0 on PASS or no-op-skip (build-flag gated), 1 on FAIL. Gated
 * behind OPENBHZD_GFCI_CAL_SELF_TEST default 0 — the PE3 polarity is
 * contradicted in pin_map.h and the CAL-→-sense path hasn't been
 * bench-validated. */
static int self_test_gfci_cal(void)
{
#if OPENBHZD_GFCI_CAL_SELF_TEST
    int rc = gfci_self_test();
    if (rc == 0) {
        printk("self-test: GFCI CAL PASS\n");
        return 0;
    }
    const char *why = (rc == -1) ? "no sense edge during CAL pulse"
                    : (rc == -2) ? "sense stuck-low after CAL release"
                    : (rc == -3) ? "sense already asserted at start"
                    :              "unknown rc";
    printk("self-test: GFCI CAL FAIL (rc=%d, %s)\n", rc, why);
    return 1;
#else
    printk("self-test: GFCI CAL DISABLED at build time "
           "(OPENBHZD_GFCI_CAL_SELF_TEST=0; bench carve-out)\n");
    return 0;
#endif
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

/* Symmetric counterpart to check_relay_weld: commanded close +
 * sensed open >= 200 ms → FAULT_RELAY_STUCK_OPEN. Spec § 4 #3.
 * Only runs when we've commanded close — so it's silent on bench
 * until M7 progresses to a state-C-driving load AND the relay
 * sense circuit is wired up (same bench unknown that gates the
 * boot self-test in M7.2). */
static void check_relay_stuck_open(fault_state_t *fs, evse_state_t *es,
                                   int sensed_closed, int *stuck_streak,
                                   j1772_state_t js, int32_t cp_mv)
{
    if (!relay_main_commanded() || sensed_closed) {
        *stuck_streak = 0;
        return;
    }
    if (*stuck_streak < (int)STUCK_OPEN_PERSIST_TICKS) ++(*stuck_streak);

    if (*stuck_streak >= (int)STUCK_OPEN_PERSIST_TICKS &&
        !fault_is_active(fs, FAULT_RELAY_STUCK_OPEN)) {
        if (fault_raise(fs, FAULT_RELAY_STUCK_OPEN) == 1) {
            printk("fault: raised %s (cmd=close, PB12=open >=%u ms)\n",
                   fault_name(FAULT_RELAY_STUCK_OPEN),
                   (unsigned)(STUCK_OPEN_PERSIST_TICKS * SAFETY_TASK_PERIOD_MS));
            post_fault_event(FAULT_RELAY_STUCK_OPEN, js, *es, cp_mv);
        }
        evse_transition(es, EVSE_FAULT);
    }
}

/* J1772 state -> EVSE state transitions. Spec § 3:
 *   READY    + J1772=C → CHARGING
 *   CHARGING + J1772≠C → READY (C->B regression is a transient pause;
 *                       relay opens immediately; allows re-progression)
 *   USER_PAUSED + J1772=A → READY (unplug ends the paused session;
 *                       USER_PAUSED otherwise exits only via RESUME)
 * Sticky: BOOT, SELF_TEST, FAULT, COOLING_DOWN do not transition out
 * via this path. COOLING_DOWN exits only when check_over_temp clears
 * the over-temp condition (hysteresis-low). */
static void update_evse_from_j1772(evse_state_t *es, j1772_state_t js,
                                   int *c_streak)
{
    if (*es == EVSE_FAULT || *es == EVSE_BOOT || *es == EVSE_SELF_TEST ||
        *es == EVSE_COOLING_DOWN) {
        *c_streak = 0;
        return;
    }
    /* Plug-removal clears the per-session authorization regardless of
     * which non-fault state we're in — a fresh plug-in starts a new
     * session and (when require_rfid_auth is on) needs a fresh swipe. */
    if (js == J1772_STATE_A && s_session_authorized) {
        s_session_authorized = 0;
        printk("rfid: session authorization cleared (J1772=A)\n");
        publish_rfid_config();
    }
    if (*es == EVSE_USER_PAUSED) {
        *c_streak = 0;
        /* Unplug ends the paused session — clears USER_PAUSED back to
         * READY so the next plug-in starts fresh. Holding the gun in
         * (J1772 in B/C/D) keeps the pause sticky as the user
         * intended. RESUME also exits, handled in the inbox path. */
        if (js == J1772_STATE_A) {
            evse_transition(es, EVSE_READY);
        }
        return;
    }
    if (js == J1772_STATE_C) {
        if (*c_streak < (int)EVSE_C_DWELL_TICKS) ++(*c_streak);
    } else {
        *c_streak = 0;
    }
    /* Authorization gate: when require_rfid_auth is set, hold READY
     * until a valid tag swipe sets s_session_authorized. The dwell
     * keeps counting up so charging starts immediately on swipe
     * (no extra 1-second wait after auth). */
    int authorized = !boot_config_require_rfid_auth() || s_session_authorized;
    if (*es == EVSE_READY && *c_streak >= (int)EVSE_C_DWELL_TICKS &&
        authorized) {
        evse_transition(es, EVSE_CHARGING);
    } else if (*es == EVSE_CHARGING && js != J1772_STATE_C) {
        /* Open is immediate — no hysteresis on the safety side. */
        evse_transition(es, EVSE_READY);
    }
}

/* Per-tick relay-state owner. Closes the contactor only when the
 * vehicle is actively requesting current (J1772=C) AND no latched
 * fault is active AND we're in a charging-eligible EVSE state. Any
 * deviation opens immediately. Single-writer of PE12 per spec § 4.
 * USER_PAUSED, FAULT, COOLING_DOWN are not charging-eligible — relay
 * stays open. */
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

/* --- Cross-task control inbox ------------------------------------------- */

static int post_safety_req(uint8_t type, uint8_t arg_u8, uint32_t arg_u32)
{
    if (s_safety_inbox == NULL) return -1;
    struct safety_req r = { .type = type, .arg_u8 = arg_u8, .arg_u32 = arg_u32 };
    return (xQueueSend(s_safety_inbox, &r, 0) == pdTRUE) ? 0 : -1;
}

int safety_request_clear_fault(uint32_t fault_id)
{
    return post_safety_req(SAFETY_REQ_CLEAR_FAULT, 0, fault_id);
}

int safety_request_pause(uint8_t reason)
{
    return post_safety_req(SAFETY_REQ_USER_PAUSE, reason, 0);
}

int safety_request_resume(void)
{
    return post_safety_req(SAFETY_REQ_USER_RESUME, 0, 0);
}

int safety_request_rfid_learn(void)
{
    return post_safety_req(SAFETY_REQ_RFID_LEARN_ARM, 0, 0);
}

int safety_request_publish_rfid_config(void)
{
    return post_safety_req(SAFETY_REQ_PUBLISH_RFID_CONFIG, 0, 0);
}

static void publish_rfid_config(void)
{
    struct __attribute__((packed)) {
        uint8_t require_rfid_auth;
        uint8_t session_authorized;
    } cfg = {
        .require_rfid_auth  = boot_config_require_rfid_auth(),
        .session_authorized = s_session_authorized,
    };
    (void)comms_publish_event(EVT_RFID_CONFIG, &cfg, sizeof cfg);
}

/* Apply one inbox entry. Returns the (possibly updated) EVSE state by
 * mutating *es. Emits TLV events on success. The streak ptr lets
 * the CLEAR_FAULT path reset cp_e_streak so the CP-settling window
 * after FAULT → READY doesn't immediately re-fire CP_NO_PILOT. */
static void process_request(struct safety_req *r,
                            fault_state_t *fs, evse_state_t *es,
                            int *cp_e_streak, j1772_ctx_t *cp)
{
    switch (r->type) {
    case SAFETY_REQ_CLEAR_FAULT: {
        uint32_t fid = r->arg_u32;
        int cleared = 0;
        if (fid == 0u) {
            cleared = fault_clear_all_clearable(fs);
            printk("safety: CLEAR_FAULT all -> %d cleared\n", cleared);
        } else if (fid < FAULT_COUNT) {
            int rc = fault_clear(fs, (fault_id_t)fid);
            if (rc == 1) cleared = 1;
            else if (rc < 0) {
                printk("safety: CLEAR_FAULT %s refused (rc=%d)\n",
                       fault_name((fault_id_t)fid), rc);
                return;
            }
            printk("safety: CLEAR_FAULT %s -> %s\n",
                   fault_name((fault_id_t)fid),
                   cleared ? "cleared" : "no-op");
        } else {
            printk("safety: CLEAR_FAULT bad id 0x%x\n", (unsigned)fid);
            return;
        }
        if (cleared) {
            uint32_t evt_id = fid;
            (void)comms_publish_event(EVT_FAULT_CLEARED, &evt_id, sizeof evt_id);
            /* Clearing CP_NO_PILOT (or all-clearable that includes it)
             * also resets the per-tick streak so the post-FAULT CP
             * settling window — CP physically transitions from -12 V
             * to idle high over a few ticks, J1772 classifier debounce
             * holds state E for the same period — doesn't re-trip the
             * detector immediately. */
            if (fid == 0u || fid == (uint32_t)FAULT_CP_NO_PILOT) {
                *cp_e_streak = 0;
            }
        }
        /* If we're in EVSE_FAULT and no latched faults remain, return
         * to SELF_TEST so the boot self-test re-runs before READY.
         * Conservative: any subtle reason we entered FAULT (e.g. ADC
         * out-of-range, relay readback) deserves a fresh check. */
        if (*es == EVSE_FAULT && !fault_any_latched_active(fs)) {
            printk("safety: faults all clear -> SELF_TEST then READY\n");
            evse_transition(es, EVSE_READY);
            fs->first_raised = FAULT_NONE;
            /* Belt-and-suspenders: same reset as above for the case
             * where CP_NO_PILOT wasn't in the cleared bits but we're
             * leaving FAULT (so apply_cp_for_state is about to swing
             * CP from -12 V back to idle high). */
            *cp_e_streak = 0;
            /* Re-init the J1772 classifier so its internal debounce
             * starts fresh on the post-FAULT CP swing. Bench-observed
             * 2026-05-04: a fault raised in state C drove EVSE_FAULT
             * → state F → CP went to -12 V → classifier committed
             * to E. CLEAR_FAULT resets cp_e_streak, but on the next
             * tick j1772_step still reports E (no debounce-N reads
             * of A yet) and check_cp_e re-fires within 3 ticks even
             * though cp_high_mv already reads +12000 mV. Re-init
             * forces committed=INVALID so check_cp_e skips until the
             * classifier resettles on the live CP. */
            j1772_init(cp);
        }
        break;
    }
    case SAFETY_REQ_USER_PAUSE:
        if (*es == EVSE_READY || *es == EVSE_CHARGING) {
            printk("safety: USER_PAUSE (reason=%u, from %s)\n",
                   (unsigned)r->arg_u8, evse_state_name(*es));
            evse_transition(es, EVSE_USER_PAUSED);
        } else {
            printk("safety: USER_PAUSE ignored (state=%s)\n",
                   evse_state_name(*es));
        }
        break;
    case SAFETY_REQ_USER_RESUME:
        if (*es == EVSE_USER_PAUSED) {
            printk("safety: USER_RESUME -> READY\n");
            evse_transition(es, EVSE_READY);
        } else {
            printk("safety: USER_RESUME ignored (state=%s)\n",
                   evse_state_name(*es));
        }
        break;
    case SAFETY_REQ_RFID_LEARN_ARM:
        s_rfid_learn_ticks = RFID_LEARN_TICKS;
        printk("safety: RFID learn-mode armed (%u ticks)\n",
               (unsigned)RFID_LEARN_TICKS);
        break;
    case SAFETY_REQ_PUBLISH_RFID_CONFIG:
        publish_rfid_config();
        break;
    default:
        printk("safety: unknown inbox req type=%u\n", (unsigned)r->type);
        break;
    }
}

static void drain_inbox(fault_state_t *fs, evse_state_t *es,
                        int *cp_e_streak, j1772_ctx_t *cp)
{
    if (s_safety_inbox == NULL) return;
    struct safety_req r;
    /* Bound to a small handful per tick so a flood from comms can't
     * starve the safety loop. */
    for (int i = 0; i < 4; ++i) {
        if (xQueueReceive(s_safety_inbox, &r, 0) != pdTRUE) break;
        process_request(&r, fs, es, cp_e_streak, cp);
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
            printk("fault: raised %s (J1772=E for >=%u ticks, cp=%d mV)\n",
                   fault_name(FAULT_CP_NO_PILOT),
                   (unsigned)CP_E_PERSIST_TICKS, (int)cp_mv);
            post_fault_event(FAULT_CP_NO_PILOT, js, *es, cp_mv);
        }
        evse_transition(es, EVSE_FAULT);
    }
}

/* PE-continuity loss detector (spec § 4 #4). PC5 reads ~0 raw when
 * the PE wire is bonded to mains earth — the OEM divider yanks the
 * node low. A broken or disconnected PE wire lets PC5 float toward
 * the rail or bias on mains-coupled noise. Out-of-band for
 * PE_PERSIST_TICKS consecutive 50 Hz ticks (≥200 ms) raises a
 * latched fault.
 *
 * Suppressed during BOOT/SELF_TEST to avoid raising before the ADC
 * scan has converged on its first stable values; the boot self-test
 * has its own ADC-rail check. Suppressed in FAULT to avoid noise
 * during the post-fault settling window. */
static void check_pe_continuity(fault_state_t *fs, evse_state_t *es,
                                j1772_state_t js, int32_t cp_mv,
                                int *pe_streak)
{
    if (*es == EVSE_BOOT || *es == EVSE_SELF_TEST || *es == EVSE_FAULT) {
        *pe_streak = 0;
        return;
    }

    uint16_t raw = adc_scan_rank(ADC_RANK_PE);
    if (raw > (uint16_t)PE_OK_RAW_MAX) {
        if (*pe_streak < (int)PE_PERSIST_TICKS) ++(*pe_streak);
    } else {
        *pe_streak = 0;
    }

    if (*pe_streak >= (int)PE_PERSIST_TICKS &&
        !fault_is_active(fs, FAULT_PE_CONTINUITY)) {
        if (fault_raise(fs, FAULT_PE_CONTINUITY) == 1) {
            printk("fault: raised %s (PC5 raw=%u >%u for >=%u ticks)\n",
                   fault_name(FAULT_PE_CONTINUITY),
                   (unsigned)raw, (unsigned)PE_OK_RAW_MAX,
                   (unsigned)PE_PERSIST_TICKS);
            post_fault_event(FAULT_PE_CONTINUITY, js, *es, cp_mv);
        }
        evse_transition(es, EVSE_FAULT);
    }
}

/* CP regression detector (spec § 4 #14). A J1772 C → B transition
 * mid-charging-session is a "transient pause" — the EV stopped
 * requesting current but stayed plugged in (e.g. SOC reached, charge
 * scheduling, or a momentary fault on the EV side). Spec semantic
 * is SELF-CLEAR: log the regression so HA / OCPP can see it, but
 * don't drive EVSE_FAULT — update_evse_from_j1772 already opens the
 * contactor immediately by transitioning CHARGING → READY when J1772
 * leaves C. Allow re-progression: the fault clears as soon as J1772
 * returns to C (driver / EV resumes charging) or to A (unplug ends
 * the session normally).
 *
 * Edge-triggered on (prev_js == C && js == B && prev_es == CHARGING)
 * so we don't raise during the pre-CHARGING dwell window or for
 * unrelated B-state pre-charge drops. Self-clear runs unconditionally
 * each tick — re-progress closes the loop without waiting for a
 * CMD_CLEAR_FAULT round-trip from HA. */
static void check_cp_regression(fault_state_t *fs,
                                j1772_state_t prev_js, j1772_state_t js,
                                evse_state_t prev_es, evse_state_t es,
                                int32_t cp_mv)
{
    /* Downgraded 2026-05-05: a C->B mid-CHARGING transition is a
     * legitimate "EV stopped requesting" edge — happens on every
     * normal end-of-charge, on bench-tester-driven unwinds, and on
     * BMS-cutoff scenarios alike. We can't tell those apart from the
     * EVSE side, so the fault bit was always going to be noisy.
     *
     * Keep the diagnostic trace: a printk on the wire and a record
     * in the event_log via post_fault_event. Don't raise the fault
     * bit (so HA's Fault Active sensor stays clean) and skip the
     * self-clear branch (nothing to clear). */
    (void)fs;
    if (prev_js == J1772_STATE_C && js == J1772_STATE_B &&
        prev_es == EVSE_CHARGING) {
        printk("%s observed (J1772 C->B during charging)\n",
               fault_name(FAULT_CP_REGRESSION));
        post_fault_event(FAULT_CP_REGRESSION, js, es, cp_mv);
    }
}

/* CC ladder decode. Maps PA7 raw to a J1772 cable-rating amp value;
 * 0 means "no cable / not installed" (raw at open-rail) and is used
 * by the snapshot to populate system_state.cc_max_amps. The detector
 * below distinguishes "0 because open-rail (OK)" from "0 because
 * raw fell into a between-bands no-mans-land (fault)". */
static uint16_t decode_cc_amps(uint16_t raw)
{
    if (raw >= CC_BAND_OPEN_MIN_RAW) return 0u;     /* open = no cable */
    if (raw >= CC_BAND_13A_LO)       return 13u;
    if (raw >= CC_BAND_20A_LO)       return 20u;
    if (raw >= CC_BAND_32A_LO)       return 32u;
    if (raw >= CC_BAND_63A_LO)       return 63u;
    return 0u;                                       /* shorted / out-of-band */
}

/* In-band = decoder returned a known amp value, or raw is in the
 * "open" band (cable absent / EVSE-side CC unwired — also OK on the
 * bench unit before a cable is hooked up). Anything else is a real
 * out-of-band reading and trips the detector. */
static int cc_raw_in_band(uint16_t raw)
{
    if (raw >= CC_BAND_OPEN_MIN_RAW)        return 1;
    if (raw >= CC_BAND_13A_LO)              return 1;
    if (raw >= CC_BAND_20A_LO)              return 1;
    if (raw >= CC_BAND_32A_LO)              return 1;
    if (raw >= CC_BAND_63A_LO)              return 1;
    return 0;
}

/* CC out-of-range detector (spec § 4 #12). Self-clearing — if the
 * raw drifts back into a band the streak resets and the fault clears
 * on the next tick that publishes a CLEAR_FAULT or transitions away
 * from EVSE_FAULT. The fault is in the latched range (id=18) but
 * the spec marks it [SELF-CLEAR]; treat it as recoverable via
 * CMD_CLEAR_FAULT in the meantime — bench tuning + a re-read of the
 * map will tell us if we need to demote it to FAULT_FIRST_SELF_CLEARING. */
static void check_cc_out_of_range(fault_state_t *fs, evse_state_t *es,
                                  j1772_state_t js, int32_t cp_mv,
                                  int *cc_streak)
{
#if OPENBHZD_CC_DETECTOR
    if (*es == EVSE_BOOT || *es == EVSE_SELF_TEST || *es == EVSE_FAULT) {
        *cc_streak = 0;
        return;
    }

    uint16_t raw = adc_scan_rank(ADC_RANK_CC);
    if (!cc_raw_in_band(raw)) {
        if (*cc_streak < (int)CC_PERSIST_TICKS) ++(*cc_streak);
    } else {
        *cc_streak = 0;
    }

    if (*cc_streak >= (int)CC_PERSIST_TICKS &&
        !fault_is_active(fs, FAULT_CC_OUT_OF_RANGE)) {
        if (fault_raise(fs, FAULT_CC_OUT_OF_RANGE) == 1) {
            printk("fault: raised %s (PA7 raw=%u out-of-band for >=%u ticks)\n",
                   fault_name(FAULT_CC_OUT_OF_RANGE),
                   (unsigned)raw, (unsigned)CC_PERSIST_TICKS);
            post_fault_event(FAULT_CC_OUT_OF_RANGE, js, *es, cp_mv);
        }
        evse_transition(es, EVSE_FAULT);
    }
#else
    (void)fs; (void)es; (void)js; (void)cp_mv;
    (void)cc_streak; (void)cc_raw_in_band;
#endif
}

/* Runtime ADC out-of-range detector (spec § 4 #8). Debounces
 * ADC_RUNTIME_PERSIST_TICKS (5 ticks = 100 ms) of any-rail reads on
 * AC / CT / LCT / CP ranks before raising. The boot self-test
 * (run_boot_self_test) covers the same set in a one-shot way; this
 * detector keeps watching once we're past the boot warm-up. */
static void check_adc_runtime(fault_state_t *fs, evse_state_t *es,
                              j1772_state_t js, int32_t cp_mv,
                              int *adc_streak)
{
    if (*es == EVSE_BOOT || *es == EVSE_SELF_TEST || *es == EVSE_FAULT) {
        *adc_streak = 0;
        return;
    }

    uint16_t b[ADC_RANKS];
    adc_scan_latest(b);

    /* ADC_RANK_CP excluded from runtime: the regular DMA scan samples
     * PA4 asynchronously to the PWM, so it lands in either HIGH or
     * LOW phase. With a vehicle plugged in J1772 state C/D the EV's
     * diode pulls LOW phase to -12 V, which the OEM read divider
     * clamps to raw≈0 — bench-confirmed with state C cp_mv=5954
     * driving the regular scan reading CPR=0 next sample. The boot
     * self-test still validates the CP rail one-shot at startup, and
     * the live cp_high path uses the inject group synchronised to
     * the PWM update event (see hal/adc_inject.c). */
    static const uint8_t ranks[] = {
        ADC_RANK_AC, ADC_RANK_CT, ADC_RANK_LCT,
    };
    int rail = 0;
    unsigned bad_rank = 0;
    uint16_t bad_raw = 0;
    for (size_t i = 0; i < sizeof(ranks)/sizeof(ranks[0]); ++i) {
        unsigned r = ranks[i];
        if (b[r] < ST_ADC_MIN || b[r] > ST_ADC_MAX) {
            rail = 1;
            bad_rank = r;
            bad_raw = b[r];
            break;
        }
    }

    if (rail) {
        if (*adc_streak < (int)ADC_RUNTIME_PERSIST_TICKS) ++(*adc_streak);
    } else {
        *adc_streak = 0;
    }

    if (*adc_streak >= (int)ADC_RUNTIME_PERSIST_TICKS &&
        !fault_is_active(fs, FAULT_ADC_OUT_OF_RANGE)) {
        if (fault_raise(fs, FAULT_ADC_OUT_OF_RANGE) == 1) {
            printk("fault: raised %s (rank %u raw=%u for >=%u ticks)\n",
                   fault_name(FAULT_ADC_OUT_OF_RANGE),
                   bad_rank, (unsigned)bad_raw,
                   (unsigned)ADC_RUNTIME_PERSIST_TICKS);
            post_fault_event(FAULT_ADC_OUT_OF_RANGE, js, *es, cp_mv);
        }
        evse_transition(es, EVSE_FAULT);
    }
}

/* GFCI fault detector. Reads PE2 (active-low: module pulls LOW on
 * fault, idle HIGH). Debounces GFCI_PERSIST_TICKS consecutive LOW
 * reads before raising. FAULT_GFCI is latched + power-cycle-only
 * clear per UL2231 (fault.c::fault_clear() refuses GFCI by id).
 *
 * Once raised, a GFCI fault keeps the EVSE in FAULT until
 * power-cycle. We don't re-trigger on subsequent LOW reads — the
 * fault_is_active() guard skips the raise path; the streak still
 * accumulates harmlessly. The redundant force-open latch fired by
 * evse_transition(EVSE_FAULT) ensures the contactor is mechanically
 * latched open even if PE12 driver were stuck. */
static void check_gfci(fault_state_t *fs, evse_state_t *es,
                       j1772_state_t js, int32_t cp_mv,
                       int *gfci_streak)
{
    if (gfci_fault_active()) {
        if (*gfci_streak < (int)GFCI_PERSIST_TICKS) ++(*gfci_streak);
    } else {
        *gfci_streak = 0;
    }

    if (*gfci_streak >= (int)GFCI_PERSIST_TICKS &&
        !fault_is_active(fs, FAULT_GFCI)) {
        if (fault_raise(fs, FAULT_GFCI) == 1) {
            printk("fault: raised %s (PE2=LOW for >=%u ticks)\n",
                   fault_name(FAULT_GFCI),
                   (unsigned)GFCI_PERSIST_TICKS);
            post_fault_event(FAULT_GFCI, js, *es, cp_mv);
        }
        evse_transition(es, EVSE_FAULT);
    }
}

/* --- BL0939-derived detectors --------------------------------------------
 *
 * The BL0939 metering IC is the single source of truth for V/I/W on this
 * PCB. There's no discrete relay closed-feedback signal, so weld /
 * stuck-open detection rides on "is current flowing through the contactor
 * at all?" — a question only the BL0939 can answer.
 *
 * Each detector receives the latest poll snapshot via *bl. They run only
 * on the BL0939 poll cadence (400 ms) — not every safety tick — to
 * avoid debouncing repeats of identical data. The streak counters are
 * stored persist-across-call via static. */

/* AC mains presence. Self-clearing fault. Trips when V_RMS raw drops
 * below the AC-present floor for BL0939_DETECTOR_PERSIST consecutive
 * polls; clears on first poll above the floor. */
static void check_ac_absent(fault_state_t *fs, evse_state_t *es,
                            const struct bl0939_readings *bl,
                            j1772_state_t js, int32_t cp_mv,
                            unsigned *streak)
{
    if (!bl->valid) return;
    if (bl->v_rms < BL0939_V_RMS_AC_PRESENT_RAW) {
        if (*streak < BL0939_DETECTOR_PERSIST) ++(*streak);
    } else {
        *streak = 0;
        if (fault_is_active(fs, FAULT_AC_ABSENT)) {
            if (fault_clear(fs, FAULT_AC_ABSENT) == 1) {
                printk("fault: cleared AC_ABSENT (V_RMS raw=%u)\n",
                       (unsigned)bl->v_rms);
            }
        }
        return;
    }
    if (*streak >= BL0939_DETECTOR_PERSIST &&
        !fault_is_active(fs, FAULT_AC_ABSENT)) {
        if (fault_raise(fs, FAULT_AC_ABSENT) == 1) {
            printk("fault: raised AC_ABSENT (V_RMS raw=%u, threshold=%u)\n",
                   (unsigned)bl->v_rms, (unsigned)BL0939_V_RMS_AC_PRESENT_RAW);
            post_fault_event(FAULT_AC_ABSENT, js, *es, cp_mv);
        }
        /* Self-clearing: don't transition to EVSE_FAULT — it's safe to
         * let the state machine keep running with no contactor close. */
    }
}

/* Relay weld via BL0939: current is flowing through the contactor
 * while we commanded it open. Latched fault per UL2231 — clearable
 * via FC41D CLEAR_FAULT (not GFCI-locked). Spec § 4 #2.
 *
 * Gated behind a non-zero IA cal scale: without calibration we
 * can't reliably distinguish chip noise (bench observed 3-14 k raw
 * floating around without real load) from a real low-current
 * residual. Once the chassis is calibrated, the threshold rides
 * on a real-amps comparison and the noise floor doesn't matter. */
static void check_relay_weld_bl0939(fault_state_t *fs, evse_state_t *es,
                                    const struct bl0939_readings *bl,
                                    j1772_state_t js, int32_t cp_mv,
                                    unsigned *streak)
{
    if (!bl->valid) return;
    int16_t scale = calibration_bl0939_ia_ua_per_raw();
    if (scale <= 0) { *streak = 0; return; }
    /* Real-amps threshold: any current ≥ 100 mA flowing through the
     * contactor while we commanded it open is a weld. 100 mA is well
     * above any realistic noise/leakage floor and well below a real
     * EV load. */
    uint64_t ma = ((uint64_t)bl->ia_rms * (uint64_t)scale) / 1000u;
    int sensing_flow = (ma >= 100u);
    if (sensing_flow && !relay_main_commanded()) {
        if (*streak < BL0939_DETECTOR_PERSIST) ++(*streak);
    } else {
        *streak = 0;
    }
    if (*streak >= BL0939_DETECTOR_PERSIST &&
        !fault_is_active(fs, FAULT_RELAY_WELD)) {
        if (fault_raise(fs, FAULT_RELAY_WELD) == 1) {
            printk("fault: raised RELAY_WELD (mA=%u, cmd=open)\n",
                   (unsigned)ma);
            post_fault_event(FAULT_RELAY_WELD, js, *es, cp_mv);
        }
        evse_transition(es, EVSE_FAULT);
    }
}

/* Relay stuck open via BL0939: no current flowing while commanded
 * close + EVSE in CHARGING state (so the EV is plugged in and
 * requesting current). Spec § 4 #3.
 *
 * Only meaningful in CHARGING — at idle (READY) the EV is not
 * drawing current even with the contactor closed, so an honest zero
 * IA_RMS isn't a fault. Same cal-scale gate as weld: silent until
 * a real engineering-units threshold can be applied. */
static void check_relay_stuck_open_bl0939(fault_state_t *fs, evse_state_t *es,
                                          const struct bl0939_readings *bl,
                                          j1772_state_t js, int32_t cp_mv,
                                          unsigned *streak)
{
    if (!bl->valid) return;
    int16_t scale = calibration_bl0939_ia_ua_per_raw();
    if (scale <= 0) { *streak = 0; return; }
    if (*es != EVSE_CHARGING || !relay_main_commanded()) {
        *streak = 0;
        return;
    }
    /* < 500 mA flowing while commanded close + EVSE=CHARGING means
     * the contactor isn't actually conducting. EVs draw at least
     * a few amps when actively charging; 500 mA is a generous
     * floor that won't trip on idle pre-charge currents. */
    uint64_t ma = ((uint64_t)bl->ia_rms * (uint64_t)scale) / 1000u;
    if (ma < 500u) {
        if (*streak < BL0939_DETECTOR_PERSIST) ++(*streak);
    } else {
        *streak = 0;
    }
    if (*streak >= BL0939_DETECTOR_PERSIST &&
        !fault_is_active(fs, FAULT_RELAY_STUCK_OPEN)) {
        if (fault_raise(fs, FAULT_RELAY_STUCK_OPEN) == 1) {
            printk("fault: raised RELAY_STUCK_OPEN (mA=%u, "
                   "cmd=close, EVSE=CHARGING)\n", (unsigned)ma);
            post_fault_event(FAULT_RELAY_STUCK_OPEN, js, *es, cp_mv);
        }
        evse_transition(es, EVSE_FAULT);
    }
}

/* Hard over-current — > hw_cap × 1.25. Latched. Spec § 4. Active
 * only once a per-chassis raw → mA cal scale is non-zero in
 * boot_config. */
static void check_hard_over_current(fault_state_t *fs, evse_state_t *es,
                                    const struct bl0939_readings *bl,
                                    j1772_state_t js, int32_t cp_mv,
                                    unsigned *streak)
{
    if (!bl->valid) return;
    int16_t scale = calibration_bl0939_ia_ua_per_raw();
    if (scale <= 0) { *streak = 0; return; }
    /* mA = raw * (uA/raw) / 1000. Use uint64_t for headroom. */
    uint64_t ma = ((uint64_t)bl->ia_rms * (uint64_t)scale) / 1000u;
    uint64_t threshold_ma = (uint64_t)HW_AMPS_MAX * 1000u
                            * BL0939_HOC_TOL_NUM / BL0939_HOC_TOL_DEN;
    if (ma > threshold_ma) {
        if (*streak < BL0939_DETECTOR_PERSIST) ++(*streak);
    } else {
        *streak = 0;
    }
    if (*streak >= BL0939_DETECTOR_PERSIST &&
        !fault_is_active(fs, FAULT_HARD_OVER_CURRENT)) {
        if (fault_raise(fs, FAULT_HARD_OVER_CURRENT) == 1) {
            printk("fault: raised HARD_OVER_CURRENT (mA=%u > %u)\n",
                   (unsigned)ma, (unsigned)threshold_ma);
            post_fault_event(FAULT_HARD_OVER_CURRENT, js, *es, cp_mv);
        }
        evse_transition(es, EVSE_FAULT);
    }
}

/* Soft over-current (spec § 4 #11). Sequence:
 *
 *   1. IA_RMS > advertised × 1.05 sustained for 30 s →
 *      ramp advertised duty -10% (or -1 A min step), reset streak,
 *      log the derate. Apply via the s_soc_derate_amps accumulator
 *      that effective_advertised_amps subtracts.
 *
 *   2. After ramp, the new (lower) effective_advertised_amps becomes
 *      the threshold baseline for the next 30 s window. If the
 *      vehicle keeps drawing > 1.05× of the new advertised, ramp
 *      again. Repeat down to the J1772 6 A floor.
 *
 *   3. At the floor and still over-threshold for 30 s → raise
 *      FAULT_SOFT_OVER_CURRENT (self-clearing). The fault clears
 *      automatically once IA_RMS drops below the (derated) threshold;
 *      the derate itself is sticky for the session — session_end
 *      resets it via clear_soc_derate_for_session_end().
 *
 * Self-clear semantic: fault tracks the OVER condition; the derate
 * stays in place to keep the cause from immediately re-occurring. */
static void check_soft_over_current(fault_state_t *fs, evse_state_t *es,
                                    const struct bl0939_readings *bl,
                                    j1772_state_t js, int32_t cp_mv,
                                    unsigned *streak)
{
    if (!bl->valid) return;
    int16_t scale = calibration_bl0939_ia_ua_per_raw();
    if (scale <= 0) { *streak = 0; return; }
    uint8_t adv = effective_advertised_amps();
    if (adv == 0) { *streak = 0; return; }
    uint64_t ma = ((uint64_t)bl->ia_rms * (uint64_t)scale) / 1000u;
    uint64_t threshold_ma = (uint64_t)adv * 1000u
                            * BL0939_SOC_TOL_NUM / BL0939_SOC_TOL_DEN;

    if (ma <= threshold_ma) {
        *streak = 0;
        if (fault_is_active(fs, FAULT_SOFT_OVER_CURRENT)) {
            if (fault_clear(fs, FAULT_SOFT_OVER_CURRENT) == 1) {
                printk("fault: cleared SOFT_OVER_CURRENT (current %u mA back below %u mA)\n",
                       (unsigned)ma, (unsigned)threshold_ma);
                uint32_t evt_id = (uint32_t)FAULT_SOFT_OVER_CURRENT;
                (void)comms_publish_event(EVT_FAULT_CLEARED, &evt_id, sizeof evt_id);
            }
        }
        return;
    }

    if (*streak < BL0939_SOC_PERSIST_POLLS) {
        ++(*streak);
        return;
    }

    /* 30 s sustained over threshold — try to ramp first; only raise
     * the fault if we can't ramp any further. */
    if (adv > SOC_DERATE_FLOOR_A) {
        uint8_t step = (adv >= 10u) ? (uint8_t)(adv / 10u) : 1u;
        s_soc_derate_amps = (uint8_t)(s_soc_derate_amps + step);
        uint8_t new_adv = effective_advertised_amps();
        printk("soc: ramp advertised %uA -> %uA (cur=%u mA, thr=%u mA, derate=%u)\n",
               (unsigned)adv, (unsigned)new_adv,
               (unsigned)ma, (unsigned)threshold_ma,
               (unsigned)s_soc_derate_amps);
        *streak = 0;
        return;
    }

    if (!fault_is_active(fs, FAULT_SOFT_OVER_CURRENT)) {
        if (fault_raise(fs, FAULT_SOFT_OVER_CURRENT) == 1) {
            printk("fault: raised SOFT_OVER_CURRENT (at %uA floor, cur=%u mA still over %u mA)\n",
                   (unsigned)adv, (unsigned)ma, (unsigned)threshold_ma);
            post_fault_event(FAULT_SOFT_OVER_CURRENT, js, *es, cp_mv);
        }
    }
}

/* Reset the soft-OC derate accumulator. Called from session_end so a
 * fresh plug-in / CSMS-StartTransaction starts at the user-configured
 * advertised amps; an over-current condition that ramped us down on
 * the previous session shouldn't bleed into the next. */
static void clear_soc_derate_for_session_end(void)
{
    if (s_soc_derate_amps != 0) {
        printk("soc: derate cleared (%uA -> 0) on session end\n",
               (unsigned)s_soc_derate_amps);
        s_soc_derate_amps = 0;
    }
}

/* Over-temp detector. The pure decision logic lives in core/over_temp.c
 * (β-model thresholds, populated guards, persistence streak). This
 * wrapper translates the edge result into firmware-side bookkeeping:
 * fault_raise / fault_clear, EVSE_FAULT transition, post_fault_event,
 * and the printk diagnostics. The constants live in over_temp.h.
 *
 * Per-channel NTC presence masks.
 *
 *   NTC1 (PA3, wall-plug end NTC): on the PCB / inside the housing,
 *     always populated. Default ON.
 *   GUN_NTC (PA2, gun-cable NTC at the J1772 handle): populated on
 *     production units; the bench unit may not have the gun cable
 *     fitted. Default ON; override to 0 on a bench build that lacks
 *     the gun cable.
 *
 * PB0 ("NTC2" in system_state) is NOT a thermistor — see pin_map.h —
 * and is never fed to the over-temp detector. Future: runtime masks
 * in boot_config so a single image runs on any chassis variant. */
#ifndef OPENBHZD_NTC1_PRESENT
#define OPENBHZD_NTC1_PRESENT  1
#endif
#ifndef OPENBHZD_GUN_NTC_PRESENT
#define OPENBHZD_GUN_NTC_PRESENT  1
#endif

static void check_over_temp(fault_state_t *fs, evse_state_t *es,
                            uint16_t ntc1_raw, uint16_t gun_raw,
                            over_temp_ctx_t *ot)
{
    /* Re-sync ctx with the fault module — fault may have been cleared
     * via the inbox path while the detector wasn't looking. */
    ot->fault_active = fault_is_active(fs, FAULT_OVER_TEMP);

    over_temp_action_t act = over_temp_step(ot, ntc1_raw, gun_raw,
                                            OPENBHZD_NTC1_PRESENT,
                                            OPENBHZD_GUN_NTC_PRESENT);

    if (act.trip) {
        if (fault_raise(fs, FAULT_OVER_TEMP) == 1) {
            printk("fault: raised OVER_TEMP (wall_raw=%u gun_raw=%u, "
                   "trip<=%u, sustained %ux ticks)\n",
                   (unsigned)ntc1_raw, (unsigned)gun_raw,
                   OT_TRIP_RAW, (unsigned)OT_PERSIST_TICKS);
            post_fault_event(FAULT_OVER_TEMP, J1772_STATE_INVALID, *es, 0);
        }
        /* Spec § 4 #9: over-temp is SELF-CLEAR with hysteresis. Route
         * to EVSE_COOLING_DOWN rather than EVSE_FAULT — relay opens
         * (apply_relay_state gates close on EVSE_CHARGING only),
         * apply_cp_for_state drives idle_high (no advertise = "EVSE
         * present but not currently offering charge"), and the unit
         * auto-recovers to READY when temps drop below the clear
         * threshold. Other latched faults still take us to FAULT
         * normally — only do this transition if we're not already
         * in a hard-fault state. */
        if (*es != EVSE_FAULT) {
            evse_transition(es, EVSE_COOLING_DOWN);
        }
    } else if (act.clear) {
        if (fault_clear(fs, FAULT_OVER_TEMP) == 1) {
            printk("fault: cleared OVER_TEMP (wall_raw=%u gun_raw=%u, "
                   "clear>=%u)\n",
                   (unsigned)ntc1_raw, (unsigned)gun_raw, OT_CLEAR_RAW);
        }
        /* Auto-recover from COOLING_DOWN. Don't disturb FAULT — if a
         * latched fault is also active, the user must CLEAR_FAULT
         * first. From READY/CHARGING/USER_PAUSED the clear is a
         * no-op transition. */
        if (*es == EVSE_COOLING_DOWN) {
            evse_transition(es, EVSE_READY);
        }
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
    int stuck_open_streak = 0;
    int last_logged_sense = -1;     /* force initial print */
    int cp_e_streak = 0;
    int evse_c_streak = 0;
    int gfci_streak = 0;
    int pe_streak = 0;
    int adc_runtime_streak = 0;
    int cc_streak = 0;
    j1772_state_t prev_js = J1772_STATE_INVALID;
    evse_state_t  prev_es = EVSE_BOOT;
    over_temp_ctx_t ot_ctx = { .trip_streak = 0, .fault_active = 0 };
    rfid_ctx_t      rfid_ctx;
    rfid_init_ctx(&rfid_ctx);
    unsigned        rfid_keepalive_div = 0;
    unsigned bl0939_poll_div = 0;
    unsigned ac_absent_streak = 0;
    unsigned weld_bl_streak = 0;
    unsigned stuck_open_bl_streak = 0;
    unsigned hoc_streak = 0;
    unsigned soc_streak = 0;

    gfci_init();
    bl0939_init();
    rfid_init();
    /* Soft-reset the BL0939's SPI state machine so the first poll
     * doesn't trip on half-frame state from a prior boot or transient.
     * Idempotent if the smoke-test build flag already ran reset. */
    bl0939_soft_reset();

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
            printk("fault: raised %s (%d sub-check fails)\n",
                   fault_name(FAULT_BOOT_SELF_TEST), st_fails);
            post_fault_event(FAULT_BOOT_SELF_TEST, js0, es, cp_mv0);
        }
        evse_transition(&es, EVSE_FAULT);
    }

    /* GFCI CAL self-test runs separately from run_boot_self_test
     * because its 50 ms CAL pulse actively flips PE2 LOW — the live
     * check_gfci detector would race it once the safety loop starts.
     * Has to fire BEFORE the for(;;) tick loop. Build-flag gated. */
    if (self_test_gfci_cal() != 0 &&
        !fault_is_active(&fs, FAULT_GFCI_SELF_TEST)) {
        if (fault_raise(&fs, FAULT_GFCI_SELF_TEST) == 1) {
            printk("fault: raised %s (CAL pulse did not provoke sense)\n",
                   fault_name(FAULT_GFCI_SELF_TEST));
            post_fault_event(FAULT_GFCI_SELF_TEST, js0, es, cp_mv0);
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
                printk("fault: raised %s\n",
                       fault_name(FAULT_RELAY_OPEN_AT_BOOT));
                post_fault_event(FAULT_RELAY_OPEN_AT_BOOT, js0, es, cp_mv0);
            }
            evse_transition(&es, EVSE_FAULT);
        } else if (relay_st == ST_RELAY_WELD_AT_BOOT &&
                   !fault_is_active(&fs, FAULT_RELAY_WELD_AT_BOOT)) {
            if (fault_raise(&fs, FAULT_RELAY_WELD_AT_BOOT) == 1) {
                printk("fault: raised %s\n",
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

    /* BOOT_COMPLETE event: u8 self_test_passed + u32 last_fault_id. */
    {
        struct __attribute__((packed)) {
            uint8_t  self_test_passed;
            uint8_t  pad[3];
            uint32_t last_fault_id;
        } boot = {
            .self_test_passed = (st_fails == 0) ? 1u : 0u,
            .last_fault_id    = (uint32_t)fs.first_raised,
        };
        (void)comms_publish_event(EVT_BOOT_COMPLETE, &boot, sizeof(boot));
    }

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        wdg_kick();

        /* Drain control inbox first so a CLEAR_FAULT or USER_PAUSE
         * lands before the rest of the tick reads/applies state. */
        drain_inbox(&fs, &es, &cp_e_streak, &cp);

        int32_t cp_mv = cp_high_mv();
        j1772_state_t s = j1772_step(&cp, cp_mv, J1772_DEBOUNCE_N);

        if (s != last_logged_j1772 && s != J1772_STATE_INVALID) {
            printk("j1772: state=%s cp=%d mV\n",
                   j1772_state_name(s), (int)cp_mv);
            last_logged_j1772 = s;
            uint8_t st = (uint8_t)s;
            (void)comms_publish_event(EVT_STATE_CHANGED, &st, sizeof(st));
        }

        check_safe_fail(&fs, &es, s, cp_mv);
#if OPENBHZD_RELAY_FEEDBACK_KNOWN
        int sensed = relay_main_sense_closed();
        check_relay_weld(&fs, &es, sensed,
                         &weld_streak, &last_logged_sense,
                         s, cp_mv);
        check_relay_stuck_open(&fs, &es, sensed,
                               &stuck_open_streak, s, cp_mv);
#else
        /* The PB12-based relay-feedback path was retired on 2026-05-04
         * once gpio_diff confirmed there's no discrete relay closed-
         * feedback signal. Weld / stuck-open detection now rides on
         * BL0939 IA_RMS (see check_relay_weld_bl0939 below). The
         * legacy detectors are kept compiled-in so a future PCB
         * revision that adds a real sense pin can flip the flag
         * without re-writing the code. */
        (void)weld_streak; (void)stuck_open_streak; (void)last_logged_sense;
        (void)check_relay_weld; (void)check_relay_stuck_open;
#endif
        check_gfci(&fs, &es, s, cp_mv, &gfci_streak);
        check_pe_continuity(&fs, &es, s, cp_mv, &pe_streak);
        check_adc_runtime(&fs, &es, s, cp_mv, &adc_runtime_streak);
        check_cc_out_of_range(&fs, &es, s, cp_mv, &cc_streak);
        check_cp_e(&fs, &es, s, cp_mv, &cp_e_streak);
        /* PA3 (rank NTC1) is the wall-plug NTC; PA2 (rank "AC", legacy
         * name pre-channel-role correction) is the gun-cable NTC.
         * PB0 ("NTC2") is exposed in system_state for diag but is not
         * a thermistor and is intentionally not in this call. */
        check_over_temp(&fs, &es,
                        adc_scan_rank(ADC_RANK_NTC1),
                        adc_scan_rank(ADC_RANK_AC),
                        &ot_ctx);

        /* After all fault checks: classifier-driven EVSE transitions,
         * relay state, and CP output. Faults preempt — helpers honor
         * EVSE_FAULT as sticky. */
        update_evse_from_j1772(&es, s, &evse_c_streak);
        check_cp_regression(&fs, prev_js, s, prev_es, es, cp_mv);
        apply_relay_state(s, es, &fs);
        apply_cp_for_state(es, s);

        prev_js = s;
        prev_es = es;

        /* BL0939 telemetry poll. Best-effort, ~1 ms of bit-banged SPI
         * once per 400 ms — no checksum/timing impact on the safety
         * loop. The BL0939-derived detectors (AC absent, weld,
         * stuck-open, hard/soft over-current) run only on this same
         * cadence — debounce streaks count poll cycles, not safety
         * ticks. */
        int bl0939_polled = 0;
        if (++bl0939_poll_div >= BL0939_POLL_TICKS) {
            bl0939_poll_div = 0;
            bl0939_poll();
            bl0939_polled = 1;
        }
        struct bl0939_readings bl;
        bl0939_get_readings(&bl);
        if (bl0939_polled) {
            check_ac_absent(&fs, &es, &bl, s, cp_mv, &ac_absent_streak);
            check_relay_weld_bl0939(&fs, &es, &bl, s, cp_mv, &weld_bl_streak);
            check_relay_stuck_open_bl0939(&fs, &es, &bl, s, cp_mv,
                                          &stuck_open_bl_streak);
            check_hard_over_current(&fs, &es, &bl, s, cp_mv, &hoc_streak);
            check_soft_over_current(&fs, &es, &bl, s, cp_mv, &soc_streak);

            /* Energy integration: only count power flow while we're
             * actively driving the contactor (avoids integrating noise
             * across READY / USER_PAUSED / FAULT stretches). bl.a_watt
             * is BL0939's signed channel-A active power; with a
             * non-zero chassis scale it converts to mW. Negative power
             * (back-flow) is dropped so fluctuations around zero don't
             * artificially deplete the accumulator. Integration window
             * is the BL0939 poll period = BL0939_POLL_TICKS ×
             * SAFETY_TASK_PERIOD_MS = 400 ms. */
            if (s_session.active && relay_main_commanded()) {
                int16_t pa_scale = calibration_bl0939_pa_mw_per_raw();
                if (pa_scale > 0) {
                    int64_t mw = (int64_t)bl.a_watt * (int64_t)pa_scale;
                    if (mw > 0) {
                        s_session.mws_accum +=
                            (mw * (int64_t)BL0939_POLL_TICKS *
                                 (int64_t)SAFETY_TASK_PERIOD_MS) / 1000;
                    }
                }
            }
        }

        /* RFID: drain RX ring + re-frame; periodic keepalive @ ~3 Hz.
         * Module is silent without the keepalive — confirmed at the
         * bench tap. Edge-detect new UID / removed-card transitions
         * and publish EVT_RFID_SWIPE so HA gets a one-shot event per
         * swipe, not a stream. On a card-present edge, run the
         * authorized-list lookup and either learn the new UID (if
         * armed) or drive a start/stop based on EVSE state. */
        {
            uint8_t rx[RFID_FRAME_MAX_LEN * 2];
            size_t  got = rfid_rx_pop(rx, sizeof(rx));
            if (got > 0) {
                rfid_action_t a;
                (void)rfid_feed(&rfid_ctx, rx, got, &a);
                if (a.edge) {
                    struct __attribute__((packed)) {
                        uint32_t uid;
                        uint8_t  present;
                    } payload = { .uid = a.uid, .present = a.present };
                    (void)comms_publish_event(EVT_RFID_SWIPE,
                                              &payload, sizeof payload);
                    printk("rfid: %s uid=0x%08x\n",
                           a.present ? "swipe" : "removed",
                           (unsigned)a.uid);

                    /* Authorize / learn / start / stop only on the
                     * card-present edge. Lift-off is informational. */
                    if (a.present && a.uid != 0u) {
                        uint8_t result;
                        if (s_rfid_learn_ticks > 0) {
                            s_rfid_learn_ticks = 0;
                            if (rfid_authlist_count() >= RFID_AUTHLIST_MAX) {
                                result = RFID_AUTH_RESULT_LIST_FULL;
                                printk("rfid: learn rejected — list full\n");
                            } else {
                                (void)persist_post_rfid_authlist_add(a.uid);
                                result = RFID_AUTH_RESULT_LEARNED;
                                printk("rfid: learned uid=0x%08x\n",
                                       (unsigned)a.uid);
                            }
                        } else if (rfid_authlist_contains(a.uid)) {
                            /* Matched tag → grant the session. When
                             * require_rfid_auth is OFF this flag still
                             * gets set; it just doesn't gate anything.
                             * Emit a config event if the bit flipped so
                             * HA reflects "session authorized" live. */
                            uint8_t prior_auth = s_session_authorized;
                            s_session_authorized = 1;
                            if (es == EVSE_CHARGING || es == EVSE_READY) {
                                if (es == EVSE_CHARGING) {
                                    (void)post_safety_req(
                                        SAFETY_REQ_USER_PAUSE, 0xFFu, 0);
                                    result = RFID_AUTH_RESULT_STOP;
                                } else {
                                    /* READY: starts charging once dwell
                                     * elapses (immediate if dwell already
                                     * met). */
                                    result = RFID_AUTH_RESULT_START;
                                }
                            } else if (es == EVSE_USER_PAUSED) {
                                (void)post_safety_req(
                                    SAFETY_REQ_USER_RESUME, 0, 0);
                                result = RFID_AUTH_RESULT_START;
                            } else {
                                result = RFID_AUTH_RESULT_MATCHED_NOOP;
                            }
                            if (!prior_auth) publish_rfid_config();
                            printk("rfid: matched uid=0x%08x result=%u "
                                   "(EVSE=%s)\n",
                                   (unsigned)a.uid, (unsigned)result,
                                   evse_state_name(es));
                        } else {
                            result = RFID_AUTH_RESULT_REJECTED;
                            printk("rfid: rejected uid=0x%08x\n",
                                   (unsigned)a.uid);
                        }
                        struct __attribute__((packed)) {
                            uint32_t uid;
                            uint8_t  result;
                        } auth = { .uid = a.uid, .result = result };
                        (void)comms_publish_event(EVT_RFID_AUTH_RESULT,
                                                  &auth, sizeof auth);
                    }
                }
            }
            if (s_rfid_learn_ticks > 0) --s_rfid_learn_ticks;
            if (++rfid_keepalive_div >= RFID_KEEPALIVE_TICKS) {
                rfid_keepalive_div = 0;
                rfid_send_keepalive();
            }
        }

        /* Publish snapshot for comms / future UI consumers. */
        struct openbhzd_state snap = {
            .j1772_state       = (uint8_t)s,
            .evse_state        = (uint8_t)es,
            /* Carry the *user-configured* value so the FC41D Number
             * entity round-trips correctly across reboot. When unset
             * (0 = "fall back to DIP1") fall back here too so the
             * sensor still shows a sensible amps figure. The CP-PWM
             * duty cycle continues to use effective_advertised_amps
             * (DIP / HW caps applied) — those caps clamp the wire
             * value, not the persisted intent. */
            .advertised_amps   = (boot_config_advertised_amps() != 0u)
                                    ? boot_config_advertised_amps()
                                    : effective_advertised_amps(),
            .contactor_cmd     = (uint8_t)relay_main_commanded(),
            .cp_high_mv        = (int16_t)cp_mv,
            .cp_low_mv         = (int16_t)cp_low_mv(),
            .active_amps_x10   = 0,
            .ntc1_dC           = 0,
            .ntc2_dC           = 0,
            .cc_max_amps       = decode_cc_amps(adc_scan_rank(ADC_RANK_CC)),
            /* AC presence: BL0939 V_RMS above the floor for the
             * AC-absent detector. Threshold matches check_ac_absent
             * (BL0939_V_RMS_AC_PRESENT_RAW). Reported even when the
             * BL0939 reading is stale — `bl.valid` carries that. */
            .ac_present        = (bl.valid &&
                                  bl.v_rms >= BL0939_V_RMS_AC_PRESENT_RAW)
                                    ? 1u : 0u,
            .pad               = 0,
            .fault_active_bits = fs.active_bits,
            .first_fault_id    = (uint32_t)fs.first_raised,
            /* Live session-mWh from the integrator. Saturate at u32
             * max if the cal scale is wildly out of range so HA shows
             * a clear signal rather than wrapping. */
            .session_mwh       = (s_session.mws_accum > 0)
                ? (uint32_t)((s_session.mws_accum / 3600) > 0xFFFFFFFFLL
                                ? 0xFFFFFFFFu
                                : (s_session.mws_accum / 3600))
                : 0u,
            .ac_adc_raw        = adc_scan_rank(ADC_RANK_AC),
            .ntc1_adc_raw      = adc_scan_rank(ADC_RANK_NTC1),
            .ntc2_adc_raw      = adc_scan_rank(ADC_RANK_NTC2),
            .bl0939_v_rms       = bl.v_rms,
            .bl0939_ia_rms      = bl.ia_rms,
            .bl0939_ib_rms      = bl.ib_rms,
            .bl0939_a_watt      = bl.a_watt,
            .bl0939_valid       = bl.valid,
            .bl0939_freq_hz_x10 = bl.v_freq_hz_x10,
        };
        system_state_publish(&snap);

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAFETY_TASK_PERIOD_MS));
    }
}

void safety_task_create(void)
{
    /* Pre-scheduler queue create so any task can call safety_request_*
     * the moment the scheduler runs. */
    s_safety_inbox = xQueueCreate(SAFETY_INBOX_DEPTH, sizeof(struct safety_req));
    configASSERT(s_safety_inbox != NULL);

    TaskHandle_t h = NULL;
    xTaskCreate(safety_task_run,
                "safety",
                SAFETY_TASK_STACK_WORDS,
                NULL,
                SAFETY_TASK_PRIORITY,
                &h);
    stack_watch_register("safety", h, SAFETY_TASK_STACK_WORDS);
}
