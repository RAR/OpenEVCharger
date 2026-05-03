#ifndef OPENBHZD_CORE_EVSE_STATE_H
#define OPENBHZD_CORE_EVSE_STATE_H

#include <stdint.h>

/* Top-level supervisor states. Numeric values match spec § 5. */
typedef enum {
    EVSE_BOOT          = 0,
    EVSE_SELF_TEST     = 1,
    EVSE_READY         = 2,
    EVSE_CHARGING      = 3,
    EVSE_USER_PAUSED   = 4,
    EVSE_COOLING_DOWN  = 5,
    EVSE_FAULT         = 6,
} evse_state_t;

static inline const char *evse_state_name(evse_state_t s)
{
    switch (s) {
    case EVSE_BOOT:         return "BOOT";
    case EVSE_SELF_TEST:    return "SELF_TEST";
    case EVSE_READY:        return "READY";
    case EVSE_CHARGING:     return "CHARGING";
    case EVSE_USER_PAUSED:  return "USER_PAUSED";
    case EVSE_COOLING_DOWN: return "COOLING_DOWN";
    case EVSE_FAULT:        return "FAULT";
    default:                return "?";
    }
}

#endif /* OPENBHZD_CORE_EVSE_STATE_H */
