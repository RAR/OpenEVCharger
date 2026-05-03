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
int  relay_main_sense_closed(void);  /* PB12 read: 1 = closed, 0 = open */

void relay_aux_open(void);
void relay_aux_close(void);
int  relay_aux_commanded(void);

#endif /* OPENBHZD_HAL_RELAY_H */
