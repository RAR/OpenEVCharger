#include "led_patterns.h"
#include "../hal/ws2812.h"
#include "../core/fault.h"

static volatile uint8_t s_override_mode = 0;
static volatile uint8_t s_override_rgb[3] = {0, 0, 0};

void led_override_set(uint8_t mode, uint8_t r, uint8_t g, uint8_t b)
{
    s_override_rgb[0] = r;
    s_override_rgb[1] = g;
    s_override_rgb[2] = b;
    s_override_mode   = mode;
}

void led_override_get(uint8_t *mode, uint8_t rgb_out[3])
{
    rgb_out[0] = s_override_rgb[0];
    rgb_out[1] = s_override_rgb[1];
    rgb_out[2] = s_override_rgb[2];
    *mode      = s_override_mode;
}

/* Triangle wave that oscillates between BREATHE_MIN..255 every period_ms.
 *
 * BREATHE_MIN = 125 puts the dim point at ~24% of peak brightness
 * after gamma (scaled_gamma(125, 60) ≈ 37 vs scaled_gamma(255, 60) =
 * 153). Wider dynamic range than 180→255 (more dramatic breathe)
 * while still clear of the gamma "flat zero" zone (sub-50 input). At
 * 40 fps the input ramps ~2 levels per frame, just below the visible
 * stepping threshold for steady-state human vision. */
#define BREATHE_MIN  125u

static uint8_t breathe(uint32_t t_ms, uint32_t period_ms)
{
    if (period_ms == 0) return 255;
    uint32_t phase = (t_ms * 2u) % (period_ms * 2u);
    if (phase >= period_ms * 2u) phase = 0; /* belt + braces */
    uint32_t pos = (phase < period_ms) ? phase : (period_ms * 2u - phase);
    /* Map pos ∈ [0, period_ms] linearly into [BREATHE_MIN, 255]. */
    uint32_t v = BREATHE_MIN +
                 (pos * (255u - BREATHE_MIN)) / period_ms;
    return (uint8_t)v;
}

/* Square wave: on for first half, off for second, period in ms. */
static uint8_t flash(uint32_t t_ms, uint32_t period_ms)
{
    if (period_ms == 0) return 255;
    return ((t_ms % period_ms) < (period_ms / 2u)) ? 255u : 0u;
}

/* Combined gamma 2.0 + percent scale.  Earlier `gamma8(scale(v, pct))`
 * lost ~5 bits of precision at the bottom: scale(60%) of 0..30 is
 * 0..18, gamma8(18) = 1, gamma8(0..13) = 0 — so a ramp-up from
 * black sat at "0" for many frames in a row before jumping.  Doing
 * gamma first in 32-bit intermediates and only then scaling preserves
 * the bottom of the curve. */
static uint8_t scaled_gamma(uint8_t v, uint8_t pct)
{
    uint32_t g = ((uint32_t)v * v) / 255u;     /* full-range gamma 2.0 */
    return (uint8_t)((g * pct) / 100u);
}

static void fill_all(uint8_t r, uint8_t g, uint8_t b, uint8_t pct)
{
    uint8_t rg = scaled_gamma(r, pct);
    uint8_t gg = scaled_gamma(g, pct);
    uint8_t bg = scaled_gamma(b, pct);
    for (unsigned i = 0; i < OPENBHZD_WS2812_LEDS; ++i) {
        ws2812_set_pixel(i, rg, gg, bg);
    }
}

static void apply_comms_overlay(unsigned every_n)
{
    /* Override every n-th LED with magenta to indicate degraded comms. */
    for (unsigned i = 0; i < OPENBHZD_WS2812_LEDS; i += every_n) {
        ws2812_set_pixel(i, 64, 0, 64);
    }
}

void led_render(const struct led_inputs *in, uint32_t t_ms)
{
#if defined(OPENBHZD_LED_FORCE_GREEN) && OPENBHZD_LED_FORCE_GREEN
    /* Bench debug: pixel 0 = red, pixel 1 = green, pixel 2 = blue,
     * pixel 3..rest = solid dim green.  This both diagnoses byte
     * order (first 3 pixels) and overall protocol health (rest of
     * strip should be uniform dim green). */
    (void)t_ms;
    for (unsigned i = 3; i < OPENBHZD_WS2812_LEDS; ++i) {
        ws2812_set_pixel(i, 0, 64, 0);  /* dim green */
    }
    ws2812_set_pixel(0, 255,   0,   0);  /* red */
    ws2812_set_pixel(1,   0, 255,   0);  /* green */
    ws2812_set_pixel(2,   0,   0, 255);  /* blue */
    ws2812_show();
    return;
#endif
    if (in->override_mode == 1u) {
        fill_all(in->override_rgb[0], in->override_rgb[1], in->override_rgb[2],
                 in->brightness_pct);
        ws2812_show();
        return;
    }

    /* Faults take priority over EVSE state.  Spec § 7:
     *   any fault: red flash 2 Hz
     *   GFCI:      red flash 5 Hz */
    if (in->fault_active_bits) {
        int gfci = (in->fault_active_bits >> FAULT_GFCI) & 1u;
        uint8_t v = flash(t_ms, gfci ? 200u : 500u);
        fill_all(v, 0, 0, in->brightness_pct);
        ws2812_show();
        return;
    }

    switch (in->evse) {
    case EVSE_BOOT:
    case EVSE_SELF_TEST: {
        /* white sweep — 1 LED on, advancing 1×/s */
        unsigned pos = (unsigned)((t_ms / (1000u / OPENBHZD_WS2812_LEDS)) %
                                  OPENBHZD_WS2812_LEDS);
        ws2812_clear();
        ws2812_set_pixel(pos,
                         scaled_gamma(255, in->brightness_pct),
                         scaled_gamma(255, in->brightness_pct),
                         scaled_gamma(255, in->brightness_pct));
        break;
    }
    case EVSE_READY:
        if (in->j1772 == J1772_STATE_B) {
            /* blue solid — vehicle plugged, not charging */
            fill_all(0, 0, 255, in->brightness_pct);
        } else {
            /* dim green solid — no vehicle (or any other A-equivalent) */
            fill_all(0, 64, 0, in->brightness_pct);
        }
        break;
    case EVSE_CHARGING: {
        /* Cyan, breathing at 0.5 Hz (period 2 s).  Per spec § 7:
         * "brightness ∝ active amps / advertised".  When the CT isn't
         * yet calibrated (active_amps_x10 == 0) we ignore the ratio
         * and run the breathe envelope at full amplitude — otherwise
         * the strip would always be ~dim. */
        uint8_t v = breathe(t_ms, 2000u);
        if (in->advertised_amps > 0u && in->active_amps_x10 > 0u) {
            uint32_t active = (uint32_t)in->active_amps_x10 / 10u;
            if (active > in->advertised_amps) active = in->advertised_amps;
            uint32_t ratio_pct = (active * 100u) / in->advertised_amps;
            if (ratio_pct < 20u) ratio_pct = 20u;  /* keep visible */
            v = (uint8_t)(((uint32_t)v * ratio_pct) / 100u);
        }
        fill_all(0, v, v, in->brightness_pct);
        break;
    }
    case EVSE_USER_PAUSED: {
        /* yellow breathing 0.5 Hz */
        uint8_t v = breathe(t_ms, 2000u);
        fill_all(v, v, 0, in->brightness_pct);
        break;
    }
    case EVSE_COOLING_DOWN: {
        /* orange breathing 1 Hz (R full, G half) */
        uint8_t v = breathe(t_ms, 1000u);
        fill_all(v, (uint8_t)(v / 2u), 0, in->brightness_pct);
        break;
    }
    case EVSE_FAULT:
        /* Reached if fault_active_bits is somehow 0 — defensive. */
        fill_all(255, 0, 0, in->brightness_pct);
        break;
    default:
        ws2812_clear();
        break;
    }

    if (in->comms_degraded) apply_comms_overlay(3);
    ws2812_show();
}
