#include "hal/relay.h"
#include "pin_map.h"
#include "hal/adc_scan.h"
#include "gd32f20x.h"

/* PE12 polarity (re-confirmed 2026-05-03 afternoon): HIGH = closed,
 * LOW = open. Single audible LOUD click on each transition.
 *
 * PB12 is a HARDWARE FORCE-OPEN LATCH per UL2231-style redundancy:
 * driving PB12 HIGH while PE12 HIGH forces the main contactor open
 * via a latch, ignoring PE12. The latch only resets when PE12 goes
 * LOW. OpenEVCharger leaves PB12 LOW (no assert) for now; future fault
 * paths can call relay_force_open_latch() for redundant open even if
 * PE12 driver fails. See pin_map.h for the full bench logbook.
 *
 * relay_main_sense_closed(): there is currently NO reliable
 * closed-feedback signal. PB0/NTC2 was tentatively the candidate
 * but bench data shows it stays in the 565-686 raw band regardless
 * of contactor state. The function returns 0 (treat as open) until a
 * real sense is identified. Sense-dependent detectors (M6.2 weld,
 * M7.5 stuck-open) are gated off via OPENEVCHARGER_RELAY_FEEDBACK_KNOWN. */

static int s_main_cmd = 0;
static int s_aux_cmd  = 0;
static int s_force_open = 0;

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
    return 0;   /* no reliable closed-feedback yet — see header comment */
}

uint16_t relay_main_sense_raw(void)
{
    return adc_scan_rank(ADC_RANK_NTC2);   /* AC-presence sense, not relay state */
}

void relay_force_open_latch(void)
{
    gpio_bit_set(PIN_RELAY_FORCE_OPEN_PORT, PIN_RELAY_FORCE_OPEN_PIN);
    s_force_open = 1;
}

void relay_force_open_release(void)
{
    gpio_bit_reset(PIN_RELAY_FORCE_OPEN_PORT, PIN_RELAY_FORCE_OPEN_PIN);
    s_force_open = 0;
}

int relay_force_open_active(void)
{
    return s_force_open;
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
