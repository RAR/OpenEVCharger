#include "buttons.h"
#include "hal/uart.h"
#include "hal/adc_scan.h"
#include "hal/gpio.h"
#include "pin_map.h"

/* Threshold bands for the PC3 resistor ladder. Bench-tunable in the
 * M2.6 validation step once raw values are observed. */
#define BAND_BOT_HI   400u
#define BAND_MID_LO   800u
#define BAND_MID_HI  1700u
#define BAND_TOP_LO  1900u
#define BAND_TOP_HI  2800u
#define BAND_IDLE_LO 3500u

#define DEBOUNCE_N    3   /* consecutive same-reads to commit */

static button_id_t s_committed = BTN_NONE;
static button_id_t s_candidate = BTN_NONE;
static unsigned    s_count = 0;
static int         s_pc9_committed = 0;     /* 1 = pressed (active-low IDR=0) */
static int         s_pc9_candidate = 0;
static unsigned    s_pc9_count = 0;

/* One-shot press latch drained by buttons_consume_event(). Holds only
 * the most recent press; if the consumer is slow, an older queued press
 * is dropped. Sized at 1 entry intentionally — UI semantics here are
 * "act on the latest intent", not a typeahead buffer. */
static volatile button_id_t s_pending_event = BTN_NONE;

static const char *btn_name(button_id_t b)
{
    switch (b) {
    case BTN_TOP: return "top";
    case BTN_MID: return "mid";
    case BTN_BOT: return "bot";
    case BTN_PC9: return "pc9";
    default:      return "none";
    }
}

static button_id_t classify_ladder(uint16_t raw)
{
    if (raw <= BAND_BOT_HI)                          return BTN_BOT;
    if (raw >= BAND_MID_LO && raw <= BAND_MID_HI)    return BTN_MID;
    if (raw >= BAND_TOP_LO && raw <= BAND_TOP_HI)    return BTN_TOP;
    if (raw >= BAND_IDLE_LO)                         return BTN_NONE;
    return s_candidate;  /* "between" — keep last candidate */
}

void buttons_init(void)
{
    s_committed = s_candidate = BTN_NONE;
    s_count = 0;
    s_pc9_committed = s_pc9_candidate = 0;
    s_pc9_count = 0;
}

void buttons_poll(void)
{
    /* Ladder via ADC rank 9 (PC3) */
    uint16_t raw = adc_scan_rank(ADC_RANK_BTN);
    button_id_t classed = classify_ladder(raw);
    if (classed == s_candidate) {
        if (s_count < DEBOUNCE_N) ++s_count;
    } else {
        s_candidate = classed;
        s_count = 1;
    }
    if (s_count >= DEBOUNCE_N && s_committed != s_candidate) {
        if (s_committed != BTN_NONE) printk("btn: release %s\n", btn_name(s_committed));
        if (s_candidate != BTN_NONE) {
            printk("btn: press %s (raw=%u)\n", btn_name(s_candidate), raw);
            s_pending_event = s_candidate;
        }
        s_committed = s_candidate;
    }

    /* PC9 GPIO */
    int pressed = (gpio_pin_read(PIN_BTN_PC9_PORT, PIN_BTN_PC9_PIN) == 0) ? 1 : 0;
    if (pressed == s_pc9_candidate) {
        if (s_pc9_count < DEBOUNCE_N) ++s_pc9_count;
    } else {
        s_pc9_candidate = pressed;
        s_pc9_count = 1;
    }
    if (s_pc9_count >= DEBOUNCE_N && s_pc9_committed != s_pc9_candidate) {
        s_pc9_committed = s_pc9_candidate;
        printk("btn: %s pc9\n", s_pc9_committed ? "press" : "release");
        if (s_pc9_committed) s_pending_event = BTN_PC9;
    }
}

button_id_t buttons_consume_event(void)
{
    button_id_t e = s_pending_event;
    s_pending_event = BTN_NONE;
    return e;
}
