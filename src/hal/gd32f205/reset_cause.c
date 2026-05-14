/* GD32F205 reset-cause HAL.
 *
 * Decodes RCU_RSTSCK (the GD32 reset-source / clock-stabilisation
 * register) into the chip-neutral reset_cause_t, then clears the flags
 * via RSTFC so the next boot sees a clean register.
 *
 * Priority order matches the old inline logic in crash_state.c: a
 * watchdog flag wins over low-power, which wins over software, NRST
 * pin, and finally power-on. The GD32 FWDGT/WWDGT sub-distinction is
 * intentionally collapsed into one RESET_CAUSE_WATCHDOG — the caller
 * only needs "was this a crash". */

#include "hal/reset_cause.h"
#include "gd32f20x.h"

reset_cause_t reset_cause_get_and_clear(void)
{
    uint32_t rstsck = RCU_RSTSCK;

    reset_cause_t cause = RESET_CAUSE_UNKNOWN;
    if      (rstsck & RCU_RSTSCK_FWDGTRSTF) cause = RESET_CAUSE_WATCHDOG;
    else if (rstsck & RCU_RSTSCK_WWDGTRSTF) cause = RESET_CAUSE_WATCHDOG;
    else if (rstsck & RCU_RSTSCK_LPRSTF)    cause = RESET_CAUSE_LOW_POWER;
    else if (rstsck & RCU_RSTSCK_SWRSTF)    cause = RESET_CAUSE_SOFTWARE;
    else if (rstsck & RCU_RSTSCK_EPRSTF)    cause = RESET_CAUSE_PIN;
    else if (rstsck & RCU_RSTSCK_PORRSTF)   cause = RESET_CAUSE_POWER_ON;

    RCU_RSTSCK |= RCU_RSTSCK_RSTFC;
    return cause;
}

const char *reset_cause_str(reset_cause_t c)
{
    switch (c) {
    case RESET_CAUSE_POWER_ON:  return "power-on";
    case RESET_CAUSE_PIN:       return "NRST pin";
    case RESET_CAUSE_SOFTWARE:  return "software (deliberate)";
    case RESET_CAUSE_WATCHDOG:  return "watchdog";
    case RESET_CAUSE_LOW_POWER: return "low-power";
    case RESET_CAUSE_UNKNOWN:
    default:                    return "unknown";
    }
}
