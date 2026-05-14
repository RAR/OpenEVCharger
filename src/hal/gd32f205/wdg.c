#include "hal/wdg.h"
#include "gd32f20x.h"

/* LSI nominal = 40 kHz. With prescaler /32 and reload counter = 1250:
 *   timeout = 1250 * 32 / 40000 = 1.0 second
 *
 * That gives safety_task plenty of margin: it ticks every 20 ms, so
 * the watchdog has 50× safety margin on a healthy run.
 */
#define WDG_PRESCALER  FWDGT_PSC_DIV32
#define WDG_RELOAD     1250U

void wdg_init(void)
{
    /* Enable LSI (IRC40K) — required clock source for FWDGT */
    rcu_osci_on(RCU_IRC40K);
    while (rcu_osci_stab_wait(RCU_IRC40K) == ERROR) {
        /* spin until LSI is stable; should be < 1 ms */
    }

    /* Configure prescaler and reload */
    fwdgt_write_enable();
    (void)fwdgt_config(WDG_RELOAD, WDG_PRESCALER);
    fwdgt_write_disable();

    /* Halt the watchdog when the core is halted by SWD — keeps debugger
     * sessions from triggering resets. DBG_CTL bit 8 = FWDGT_HOLD. */
    DBG_CTL |= DBG_CTL_FWDGT_HOLD;

    fwdgt_enable();
}

void wdg_kick(void)
{
    fwdgt_counter_reload();
}

int wdg_was_last_reset(void)
{
    if (RCU_RSTSCK & RCU_RSTSCK_FWDGTRSTF) {
        return 1;
    }
    return 0;
}
