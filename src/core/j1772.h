#ifndef OPENBHZD_CORE_J1772_H
#define OPENBHZD_CORE_J1772_H

#include <stdint.h>

typedef enum {
    J1772_STATE_INVALID = 0,
    J1772_STATE_A,
    J1772_STATE_B,
    J1772_STATE_C,
    J1772_STATE_D,
    J1772_STATE_E,
    J1772_STATE_F,
} j1772_state_t;

typedef struct {
    j1772_state_t committed;
    j1772_state_t candidate;
    uint8_t       streak;
} j1772_ctx_t;

void j1772_init(j1772_ctx_t *c);
j1772_state_t j1772_step(j1772_ctx_t *c, int32_t cp_mv, uint8_t debounce_n);
const char *j1772_state_name(j1772_state_t s);

#endif
