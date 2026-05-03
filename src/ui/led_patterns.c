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

/* Triangle wave 0..255..0 every period_ms ms. Cheap stand-in for sin(); the
 * eye doesn't really care about waveform shape at a few Hz. */
static uint8_t breathe(uint32_t t_ms, uint32_t period_ms)
{
    if (period_ms == 0) return 255;
    uint32_t phase = (t_ms * 2u) % (period_ms * 2u);
    if (phase >= period_ms * 2u) phase = 0; /* belt + braces */
    /* phase ∈ [0, period_ms*2). map to triangle 0..period_ms..0 */
    uint32_t pos = (phase < period_ms) ? phase : (period_ms * 2u - phase);
    /* scale to 0..255 */
    return (uint8_t)((pos * 255u) / period_ms);
}

/* Square wave: on for first half, off for second, period in ms. */
static uint8_t flash(uint32_t t_ms, uint32_t period_ms)
{
    if (period_ms == 0) return 255;
    return ((t_ms % period_ms) < (period_ms / 2u)) ? 255u : 0u;
}

static uint8_t scale(uint8_t v, uint8_t pct)
{
    return (uint8_t)(((uint32_t)v * pct) / 100u);
}

static void fill_all(uint8_t r, uint8_t g, uint8_t b, uint8_t pct)
{
    for (unsigned i = 0; i < OPENBHZD_WS2812_LEDS; ++i) {
        ws2812_set_pixel(i, scale(r, pct), scale(g, pct), scale(b, pct));
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
    if (in->override_mode == 1u) {
        fill_all(in->override_rgb[0], in->override_rgb[1], in->override_rgb[2],
                 in->brightness_pct);
        ws2812_show();
        return;
    }

    /* Faults take priority over EVSE state. */
    if (in->fault_active_bits) {
        int gfci = (in->fault_active_bits >> FAULT_GFCI) & 1u;
        uint8_t v = flash(t_ms, gfci ? 200u : 500u);  /* 5 Hz vs 2 Hz */
        fill_all(scale(255u, v / 4u + 191u), 0, 0, in->brightness_pct);
        /* simpler: just flash full red on/off */
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
                         scale(255, in->brightness_pct),
                         scale(255, in->brightness_pct),
                         scale(255, in->brightness_pct));
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
        /* cyan, breathing at 0.3 Hz (period ≈ 3.3 s) */
        uint8_t v = breathe(t_ms, 3333u);
        fill_all(scale(0, v), scale(255, v), scale(255, v), in->brightness_pct);
        break;
    }
    case EVSE_USER_PAUSED: {
        /* yellow breathing 0.5 Hz */
        uint8_t v = breathe(t_ms, 2000u);
        fill_all(scale(255, v), scale(255, v), 0, in->brightness_pct);
        break;
    }
    case EVSE_COOLING_DOWN: {
        /* orange breathing 1 Hz */
        uint8_t v = breathe(t_ms, 1000u);
        fill_all(scale(255, v), scale(128, v), 0, in->brightness_pct);
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
