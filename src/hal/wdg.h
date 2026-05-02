/* Watchdog HAL — independent watchdog (FWDGT in GD32 nomenclature, IWDG in
 * STM32). Owned by safety_task: no other code calls wdg_kick().
 */

#ifndef OPENBHZD_HAL_WDG_H
#define OPENBHZD_HAL_WDG_H

#include <stdint.h>

/* Initialise the watchdog with ~1 second timeout.
 * Once enabled, the watchdog cannot be disabled — it must be kicked
 * every < 1 s or the chip resets. Call this AFTER FreeRTOS is started
 * but BEFORE safety_task enters its main loop.
 */
void wdg_init(void);

/* Reset the watchdog countdown. Call from safety_task only, on every tick. */
void wdg_kick(void);

/* Returns 1 if the most recent reset was due to watchdog, 0 otherwise.
 * Read this once at boot before clearing the reset flags.
 */
int wdg_was_last_reset(void);

#endif /* OPENBHZD_HAL_WDG_H */
