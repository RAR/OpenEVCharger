#ifndef OPENBHZD_HAL_RELAY_H
#define OPENBHZD_HAL_RELAY_H

#include <stdint.h>

/* Main DPDT contactor on PE12 + closed-feedback sense input on PB12.
 * Aux SPST relay on PE0 (purpose unknown — spec § 13 open item;
 * driven open by default). PE12/PE0 are configured as output PP idle
 * LOW by gpio_init_all() (M2); these helpers just toggle state +
 * track the last command.
 *
 * Single-writer rule (spec § 2 + § 4): only safety_task may call the
 * *_open/_close functions. Other tasks/ISRs are read-only. */

void relay_main_open(void);
void relay_main_close(void);
int  relay_main_commanded(void);     /* 1 = closed, 0 = open */

/* Closed-feedback: NOT YET KNOWN. PB0/NTC2 was tentatively assigned
 * but bench data showed it isn't actually relay-correlated (probably
 * AC-mains-presence). Both functions are kept for the eventual real
 * sense path; relay_main_sense_closed() returns 0 hardcoded until
 * we identify the right signal. */
int      relay_main_sense_closed(void);
uint16_t relay_main_sense_raw(void);  /* AC-presence reading, not relay state */

/* Hardware force-open latch (PB12). Spec § 4 redundant safety: any
 * fault condition can assert this to hardware-latch the contactor
 * open, even if the PE12 driver has stuck HIGH. To re-arm after a
 * latch, the caller must drop PE12 LOW (relay_main_open) first, then
 * release the latch, then re-issue close. */
void relay_force_open_latch(void);
void relay_force_open_release(void);
int  relay_force_open_active(void);

void relay_aux_open(void);
void relay_aux_close(void);
int  relay_aux_commanded(void);

#endif /* OPENBHZD_HAL_RELAY_H */
