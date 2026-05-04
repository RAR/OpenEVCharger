#include "gfci.h"
#include "../core/pin_map.h"
#include "gd32f20x.h"

/* PE2 is configured as input pull-up by gpio_init_all() (it appeared
 * in the legacy strap-list under `PIN_STRAP_PE2_*`; the role-rename
 * to `PIN_GFCI_SENSE_*` happened after the bench wiggle confirmed
 * the function — pin_map.h has both spellings until the strap macros
 * are deprecated). gfci_init() is therefore a no-op on the GPIO; it
 * exists for symmetry with other HAL inits and as a hook for future
 * CAL self-test scheduling. */
void gfci_init(void)
{
    /* Defensive: re-assert input pull-up in case some earlier init
     * touched the pad. Idempotent. */
    gpio_bit_set(PIN_GFCI_SENSE_PORT, PIN_GFCI_SENSE_PIN);
    gpio_init(PIN_GFCI_SENSE_PORT, GPIO_MODE_IPU,
              GPIO_OSPEED_50MHZ, PIN_GFCI_SENSE_PIN);
}

int gfci_fault_active(void)
{
    /* Active-low: module pulls PE2 LOW on fault, idle HIGH. */
    return (gpio_input_bit_get(PIN_GFCI_SENSE_PORT, PIN_GFCI_SENSE_PIN)
            == RESET) ? 1 : 0;
}
