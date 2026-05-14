/* boards/nexcyber/hal/relay.c — Contactor driver.
 *
 * See relay.h for the close-pulse + hold model. PA0 = hold, PA1 = pulse.
 * Bench-confirmed 2026-05-11 against the charging-state SWD snapshot
 * (PA0 ODR=1, PA1 ODR=0 while contactors closed and current flowing).
 */

#include "relay_nx.h"
#include "pin_map.h"
#include "n32g45x.h"

/* PIN_*_PORT macros are uint32_t-typed (see pin_map.h Task 12 note);
 * the Nations SPL GPIO_Set/ResetBits() want a GPIO_Module *. */
#define CONTACTOR_HOLD_GPIO   ((GPIO_Module *)PIN_CONTACTOR_HOLD_PORT)
#define CONTACTOR_CLOSE_GPIO  ((GPIO_Module *)PIN_CONTACTOR_CLOSE_PORT)

static volatile bool s_hold_asserted = false;

void relay_init(void)
{
    /* Pads are already OUT_PP from gpio_init_all + safe-low init.
     * Just re-zero the ODR bits to be explicit about the safe state. */
    GPIO_ResetBits(CONTACTOR_HOLD_GPIO,  PIN_CONTACTOR_HOLD_PIN);
    GPIO_ResetBits(CONTACTOR_CLOSE_GPIO, PIN_CONTACTOR_CLOSE_PIN);
    s_hold_asserted = false;
}

bool relay_close(uint32_t pulse_ms, void (*delay_ms)(uint32_t))
{
    if (!delay_ms) return false;

    /* Step 1: assert hold (PA0). This must be HIGH before the close
     * pulse so the external latch can latch into the "closed" state.
     * If PA0 is LOW, the latch's reset input is dominant and PA1 can't
     * latch closed. */
    GPIO_SetBits(CONTACTOR_HOLD_GPIO, PIN_CONTACTOR_HOLD_PIN);
    s_hold_asserted = true;

    /* Brief settle — let the hold rail propagate through the SR latch's
     * input gating before firing the close pulse. */
    delay_ms(1);

    /* Step 2: pulse PA1 HIGH for the configured duration. */
    GPIO_SetBits(CONTACTOR_CLOSE_GPIO, PIN_CONTACTOR_CLOSE_PIN);
    delay_ms(pulse_ms);
    GPIO_ResetBits(CONTACTOR_CLOSE_GPIO, PIN_CONTACTOR_CLOSE_PIN);

    /* PA0 stays HIGH — the external latch now holds the contactors
     * closed for as long as PA0 is asserted. */
    return true;
}

void relay_open(void)
{
    /* Drop hold — external latch resets, contactors open. */
    GPIO_ResetBits(CONTACTOR_HOLD_GPIO, PIN_CONTACTOR_HOLD_PIN);
    GPIO_ResetBits(CONTACTOR_CLOSE_GPIO, PIN_CONTACTOR_CLOSE_PIN);
    s_hold_asserted = false;
}

bool relay_hold_asserted(void)
{
    return s_hold_asserted;
}
