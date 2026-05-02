#include "j1772.h"

static j1772_state_t classify_strict(int32_t cp_mv)
{
    if (cp_mv >=  10500) return J1772_STATE_A;
    if (cp_mv >=   7500) return J1772_STATE_B;
    if (cp_mv >=   4500) return J1772_STATE_C;
    if (cp_mv >=   1500) return J1772_STATE_D;
    if (cp_mv >=  -1500) return J1772_STATE_E;
    return J1772_STATE_F;
}

void j1772_init(j1772_ctx_t *c)
{
    c->committed = J1772_STATE_INVALID;
    c->candidate = J1772_STATE_INVALID;
    c->streak    = 0;
}

j1772_state_t j1772_step(j1772_ctx_t *c, int32_t cp_mv, uint8_t debounce_n)
{
    j1772_state_t s = classify_strict(cp_mv);
    if (s == c->candidate) {
        if (c->streak < 0xFF) ++c->streak;
    } else {
        c->candidate = s;
        c->streak    = 1;
    }
    if (c->streak >= debounce_n && c->committed != c->candidate) {
        c->committed = c->candidate;
    }
    return c->committed;
}

const char *j1772_state_name(j1772_state_t s)
{
    switch (s) {
    case J1772_STATE_A: return "A";
    case J1772_STATE_B: return "B";
    case J1772_STATE_C: return "C";
    case J1772_STATE_D: return "D";
    case J1772_STATE_E: return "E";
    case J1772_STATE_F: return "F";
    default:            return "?";
    }
}
