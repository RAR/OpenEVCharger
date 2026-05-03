#ifndef OPENBHZD_UI_BUZZER_H
#define OPENBHZD_UI_BUZZER_H

#include <stdint.h>

/* Software-toggled GPIO buzzer on PB2 per spec § 7. The pattern engine
 * is a simple state machine ticked from io_task at 50 Hz. Only one
 * pattern at a time; later one wins. */

typedef enum {
    BUZ_OFF = 0,
    BUZ_BOOT_PASS,        /* 1× 100 ms beep */
    BUZ_BOOT_FAIL,        /* 3× 100 ms beeps repeating every 2 s */
    BUZ_SESSION_START,    /* 1× 200 ms beep */
    BUZ_SESSION_END,      /* 2× 100 ms beeps */
    BUZ_FAULT_NON_GFCI,   /* 1 s on / 1 s off until cleared */
    BUZ_FAULT_GFCI,       /* 5 s on then 1 s on / 1 s off */
    BUZ_BUTTON,           /* 30 ms one-shot */
    BUZ_ONESHOT,          /* generic, length set by buzzer_set_oneshot() */
} buzzer_pattern_t;

void buzzer_init(void);
void buzzer_set_pattern(buzzer_pattern_t p);
void buzzer_set_oneshot(uint16_t ms);    /* enters BUZ_ONESHOT for ms */
void buzzer_tick(uint32_t now_ms);       /* called from io_task each 20 ms */

#endif /* OPENBHZD_UI_BUZZER_H */
