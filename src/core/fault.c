#include "fault.h"

#define FID_OK(id)  ((id) > FAULT_NONE && (id) < FAULT_COUNT)

void fault_init(fault_state_t *s)
{
    s->active_bits  = 0u;
    s->first_raised = FAULT_NONE;
}

int fault_is_latched_kind(fault_id_t id)
{
    if (!FID_OK(id)) return 0;
    return (id < FAULT_FIRST_SELF_CLEARING) ? 1 : 0;
}

int fault_is_active(const fault_state_t *s, fault_id_t id)
{
    if (!FID_OK(id)) return 0;
    return ((s->active_bits >> (uint32_t)id) & 1u) ? 1 : 0;
}

int fault_any_active(const fault_state_t *s)
{
    return s->active_bits ? 1 : 0;
}

int fault_any_latched_active(const fault_state_t *s)
{
    uint32_t mask = 0u;
    for (uint32_t i = 1u; i < (uint32_t)FAULT_FIRST_SELF_CLEARING; ++i) {
        mask |= (1u << i);
    }
    return (s->active_bits & mask) ? 1 : 0;
}

int fault_raise(fault_state_t *s, fault_id_t id)
{
    if (!FID_OK(id)) return -1;
    uint32_t mask = 1u << (uint32_t)id;
    if (s->active_bits & mask) return 0;
    s->active_bits |= mask;
    if (s->first_raised == FAULT_NONE) s->first_raised = id;
    return 1;
}

int fault_clear(fault_state_t *s, fault_id_t id)
{
    if (!FID_OK(id)) return -1;
    if (id == FAULT_GFCI) return -1;
    uint32_t mask = 1u << (uint32_t)id;
    if (!(s->active_bits & mask)) return 0;
    s->active_bits &= ~mask;
    if (s->first_raised == id) {
        s->first_raised = FAULT_NONE;
        for (uint32_t i = 1u; i < (uint32_t)FAULT_COUNT; ++i) {
            if (s->active_bits & (1u << i)) {
                s->first_raised = (fault_id_t)i;
                break;
            }
        }
    }
    return 1;
}

int fault_clear_all_clearable(fault_state_t *s)
{
    int n = 0;
    for (uint32_t i = 1u; i < (uint32_t)FAULT_FIRST_SELF_CLEARING; ++i) {
        if ((fault_id_t)i == FAULT_GFCI) continue;
        if (s->active_bits & (1u << i)) {
            s->active_bits &= ~(1u << i);
            ++n;
        }
    }
    s->first_raised = FAULT_NONE;
    for (uint32_t i = 1u; i < (uint32_t)FAULT_COUNT; ++i) {
        if (s->active_bits & (1u << i)) {
            s->first_raised = (fault_id_t)i;
            break;
        }
    }
    return n;
}

const char *fault_name(fault_id_t id)
{
    switch (id) {
    case FAULT_NONE:                  return "none";
    case FAULT_GFCI:                  return "GFCI";
    case FAULT_RELAY_WELD:            return "RELAY_WELD";
    case FAULT_RELAY_STUCK_OPEN:      return "RELAY_STUCK_OPEN";
    case FAULT_PE_CONTINUITY:         return "PE_CONTINUITY";
    case FAULT_CP_NO_PILOT:           return "CP_NO_PILOT";
    case FAULT_DIODE_CHECK:           return "DIODE_CHECK";
    case FAULT_BOOT_SELF_TEST:        return "BOOT_SELF_TEST";
    case FAULT_GFCI_SELF_TEST:        return "GFCI_SELF_TEST";
    case FAULT_RELAY_WELD_AT_BOOT:    return "RELAY_WELD_AT_BOOT";
    case FAULT_RELAY_OPEN_AT_BOOT:    return "RELAY_OPEN_AT_BOOT";
    case FAULT_ADC_OUT_OF_RANGE:      return "ADC_OUT_OF_RANGE";
    case FAULT_HARD_OVER_CURRENT:     return "HARD_OVER_CURRENT";
    case FAULT_CRASH_LOOP_SAFE_FAIL:  return "CRASH_LOOP_SAFE_FAIL";
    case FAULT_OVER_TEMP:             return "OVER_TEMP";
    case FAULT_SOFT_OVER_CURRENT:     return "SOFT_OVER_CURRENT";
    case FAULT_CC_OUT_OF_RANGE:       return "CC_OUT_OF_RANGE";
    case FAULT_AC_ABSENT:             return "AC_ABSENT";
    case FAULT_CP_REGRESSION:         return "CP_REGRESSION";
    default:                          return "?";
    }
}
