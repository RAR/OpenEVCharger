#ifndef OPENEVCHARGER_TASKS_FC41D_FLASH_HELPER_H
#define OPENEVCHARGER_TASKS_FC41D_FLASH_HELPER_H

/* "FC41D flash mode" helper task. Created in place of comms_task when
 * DIP4 is held LOW at boot. Powers up the FC41D (VEN high, CEN high)
 * so its serial bootloader runs, then watches the internal PC9
 * button: on each press, pulses CEN LOW for 50 ms to reset the
 * module back into bootloader-ready state. Pairs with ltchiptool
 * listening on the same UART. */
void fc41d_flash_helper_create(void);

#endif
