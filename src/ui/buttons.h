#ifndef OPENEVCHARGER_UI_BUTTONS_H
#define OPENEVCHARGER_UI_BUTTONS_H

typedef enum {
    BTN_NONE = 0,
    BTN_TOP  = 1,
    BTN_MID  = 2,
    BTN_BOT  = 3,
    BTN_PC9  = 4,
} button_id_t;

/* Call once at boot (any time after adc_scan_init()). */
void buttons_init(void);

/* Poll once per io_task tick (50 ms recommended). Emits printk lines on
 * press and release transitions. */
void buttons_poll(void);

/* One-shot press consumer. Returns the most recently committed press
 * (BTN_TOP / BTN_MID / BTN_BOT / BTN_PC9) since the last call, then
 * clears it. Returns BTN_NONE if nothing was pressed since last drain.
 * Press-only — release transitions are ignored. */
button_id_t buttons_consume_event(void);

#endif
