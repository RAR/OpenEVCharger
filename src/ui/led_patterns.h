#ifndef OPENBHZD_UI_LED_PATTERNS_H
#define OPENBHZD_UI_LED_PATTERNS_H

#include <stdint.h>
#include "../core/evse_state.h"
#include "../core/j1772.h"

/* Spec § 7 colour/animation matrix. led_render() reads the supplied
 * inputs, computes a per-pixel RGB frame, and pushes it into the
 * WS2812 driver. Caller (io_task) calls at ~30 Hz with monotonic ms
 * for animation phase.
 *
 * No mutex / no peripheral access except via ws2812_*; safe to run
 * from any task. */

struct led_inputs {
    evse_state_t  evse;
    j1772_state_t j1772;
    uint32_t      fault_active_bits;   /* bit-set per fault_id_t */
    uint8_t       comms_degraded;      /* 1 = overlay every-3rd magenta */
    uint8_t       brightness_pct;      /* 0..100 — default 60 per spec */
    /* CHARGING-state inputs: brightness scales by active/advertised
     * per spec § 7.  Both 0 = "no CT data" and the breathe envelope
     * runs at full amplitude. */
    uint8_t       advertised_amps;     /* effective DIP+TLV cap */
    uint16_t      active_amps_x10;     /* CT-measured load × 10 */
    /* Override (FC41D SET_LED_OVERRIDE). 0 = no override. */
    uint8_t       override_mode;       /* 1 = solid override colour */
    uint8_t       override_rgb[3];
};

void led_render(const struct led_inputs *in, uint32_t t_ms);

/* TLV SET_LED_OVERRIDE landing point. Set mode=1 + rgb to force the
 * strip to a solid colour; mode=0 reverts to the state-driven pattern.
 * Safe to call from any task: bytes are written atomically. */
void led_override_set(uint8_t mode, uint8_t r, uint8_t g, uint8_t b);
void led_override_get(uint8_t *mode, uint8_t rgb_out[3]);

#endif /* OPENBHZD_UI_LED_PATTERNS_H */
