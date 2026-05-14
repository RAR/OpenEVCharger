/* boards/nexcyber/hal/gfci.c — GFCI CAL pulse driver.
 *
 * See gfci.h for the polarity / pulse-width rationale. Pad already
 * configured OUT_PP in M2 gpio_init_all. This file just toggles ODR. */

#include "gfci_nx.h"
#include "pin_map.h"
#include "n32g45x.h"

/* PIN_GFCI_CAL_PORT is uint32_t-typed (see pin_map.h Task 12 note);
 * the Nations SPL GPIO_Set/ResetBits() want a GPIO_Module *. */
#define GFCI_CAL_GPIO  ((GPIO_Module *)PIN_GFCI_CAL_PORT)

void gfci_init(void)
{
    GPIO_ResetBits(GFCI_CAL_GPIO, PIN_GFCI_CAL_PIN);
}

void gfci_cal_pulse(uint32_t pulse_ms, void (*delay_ms)(uint32_t))
{
    if (!delay_ms) return;
    GPIO_SetBits(GFCI_CAL_GPIO, PIN_GFCI_CAL_PIN);
    delay_ms(pulse_ms);
    GPIO_ResetBits(GFCI_CAL_GPIO, PIN_GFCI_CAL_PIN);
}
