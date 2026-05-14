#include "buzzer.h"
#include "pin_map.h"
#include "gd32f20x.h"

static buzzer_pattern_t s_pattern = BUZ_OFF;
static uint32_t         s_pattern_start_ms = 0;
static uint16_t         s_oneshot_ms = 0;

static void buzz_set(int on)
{
    if (on) gpio_bit_set(PIN_BUZZER_PORT, PIN_BUZZER_PIN);
    else    gpio_bit_reset(PIN_BUZZER_PORT, PIN_BUZZER_PIN);
}

void buzzer_init(void)
{
    /* GPIO already configured output PP idle-LOW by gpio_init_all().
     * Nothing else to do; just make sure the pin is LOW. */
    buzz_set(0);
}

void buzzer_set_pattern(buzzer_pattern_t p)
{
    if (p == s_pattern) return;
    s_pattern = p;
    s_pattern_start_ms = 0;   /* tick() will stamp on next call */
}

void buzzer_set_oneshot(uint16_t ms)
{
    if (ms == 0) { buzzer_set_pattern(BUZ_OFF); return; }
    if (ms > 500) ms = 500;   /* spec § 7 cap */
    s_oneshot_ms = ms;
    s_pattern = BUZ_ONESHOT;
    s_pattern_start_ms = 0;
}

void buzzer_tick(uint32_t now_ms)
{
    if (s_pattern == BUZ_OFF) { buzz_set(0); return; }
    if (s_pattern_start_ms == 0) s_pattern_start_ms = now_ms;
    uint32_t dt = now_ms - s_pattern_start_ms;

    int on = 0;
    switch (s_pattern) {
    case BUZ_BOOT_PASS:
        on = (dt < 100u);
        if (dt >= 200u) { s_pattern = BUZ_OFF; }
        break;
    case BUZ_BOOT_FAIL: {
        /* 3 beeps × 100 ms on / 100 ms off = 600 ms, then 1.4 s gap, repeat. */
        uint32_t cyc = dt % 2000u;
        if      (cyc < 100u) on = 1;
        else if (cyc < 200u) on = 0;
        else if (cyc < 300u) on = 1;
        else if (cyc < 400u) on = 0;
        else if (cyc < 500u) on = 1;
        else                 on = 0;
        break;
    }
    case BUZ_SESSION_START:
        on = (dt < 200u);
        if (dt >= 300u) s_pattern = BUZ_OFF;
        break;
    case BUZ_SESSION_END: {
        if      (dt < 100u) on = 1;
        else if (dt < 200u) on = 0;
        else if (dt < 300u) on = 1;
        else                on = 0;
        if (dt >= 400u) s_pattern = BUZ_OFF;
        break;
    }
    case BUZ_FAULT_NON_GFCI:
        on = ((dt / 1000u) & 1u) == 0;   /* 1 s on / 1 s off */
        break;
    case BUZ_FAULT_GFCI:
        if (dt < 5000u) on = 1;
        else            on = (((dt - 5000u) / 1000u) & 1u) == 0;
        break;
    case BUZ_BUTTON:
        on = (dt < 30u);
        if (dt >= 50u) s_pattern = BUZ_OFF;
        break;
    case BUZ_ONESHOT:
        on = (dt < s_oneshot_ms);
        if (dt >= s_oneshot_ms) s_pattern = BUZ_OFF;
        break;
    case BUZ_OFF:
    default:
        on = 0;
        break;
    }
    buzz_set(on);
}
