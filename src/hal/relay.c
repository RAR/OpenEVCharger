#include "relay.h"
#include "../core/pin_map.h"
#include "gd32f20x.h"

static int s_main_cmd = 0;
static int s_aux_cmd  = 0;

void relay_main_open(void)
{
    gpio_bit_reset(PIN_RELAY_MAIN_PORT, PIN_RELAY_MAIN_PIN);
    s_main_cmd = 0;
}

void relay_main_close(void)
{
    gpio_bit_set(PIN_RELAY_MAIN_PORT, PIN_RELAY_MAIN_PIN);
    s_main_cmd = 1;
}

int relay_main_commanded(void)
{
    return s_main_cmd;
}

int relay_main_sense_closed(void)
{
    return (gpio_input_bit_get(PIN_RELAY_SENSE_PORT,
                               PIN_RELAY_SENSE_PIN) == SET) ? 1 : 0;
}

void relay_aux_open(void)
{
    gpio_bit_reset(PIN_RELAY_AUX_PORT, PIN_RELAY_AUX_PIN);
    s_aux_cmd = 0;
}

void relay_aux_close(void)
{
    gpio_bit_set(PIN_RELAY_AUX_PORT, PIN_RELAY_AUX_PIN);
    s_aux_cmd = 1;
}

int relay_aux_commanded(void)
{
    return s_aux_cmd;
}
