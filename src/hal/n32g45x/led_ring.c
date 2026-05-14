/* boards/nexcyber/hal/led_ring.c — LED ring driver.
 *
 * See led_ring.h for the per-channel pin mapping and the pin_map.h
 * commentary for the topology + bench identification trail.
 */

#include "led_ring.h"
#include "pin_map.h"
#include "n32g45x.h"

/* PIN_*_PORT macros are uint32_t-typed (see pin_map.h Task 12 note);
 * the Nations SPL GPIO_Set/ResetBits() want a GPIO_Module *. */
#define LED_BLUE_GPIO   ((GPIO_Module *)PIN_LED_BLUE_PORT)
#define LED_GREEN_GPIO  ((GPIO_Module *)PIN_LED_GREEN_PORT)

void led_ring_init(void)
{
    /* Pads are already OUT_PP from gpio_init_all (added in the same
     * commit that introduced this file). Drive them LOW so both LEDs
     * start OFF. The buffer's gate at LOW = no current through LED. */
    GPIO_ResetBits(LED_BLUE_GPIO,  PIN_LED_BLUE_PIN);
    GPIO_ResetBits(LED_GREEN_GPIO, PIN_LED_GREEN_PIN);
}

void led_blue_set(bool on)
{
    if (on) {
        GPIO_SetBits(LED_BLUE_GPIO, PIN_LED_BLUE_PIN);
    } else {
        GPIO_ResetBits(LED_BLUE_GPIO, PIN_LED_BLUE_PIN);
    }
}

void led_green_set(bool on)
{
    if (on) {
        GPIO_SetBits(LED_GREEN_GPIO, PIN_LED_GREEN_PIN);
    } else {
        GPIO_ResetBits(LED_GREEN_GPIO, PIN_LED_GREEN_PIN);
    }
}
