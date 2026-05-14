/* Normalized charger state: the hardware-independent struct the northbound
 * adapters publish. charger_state_read() pulls raw bytes from `shmem` and
 * applies scaling — ALL scaling lives in charger_state.c so bench-tuning (M1)
 * is a one-file change. */
#ifndef CHARGER_STATE_H
#define CHARGER_STATE_H

#include "shmem.h"

enum evse_state {
    EVSE_STATE_UNKNOWN = 0,
    EVSE_STATE_IDLE,
    EVSE_STATE_CONNECTED,
    EVSE_STATE_CHARGING,
    EVSE_STATE_FAULT,
};

#define CHARGER_MAX_FAULTS 32   /* one name slot per alarm-bitmap byte */

struct charger_state {
    int             voltage_v;     /* line voltage, volts */
    int             current_a;     /* line current, amps */
    int             stm32_link;    /* 1 = inter-MCU link OK */
    int             heartbeat;     /* Pri_Comm heartbeat counter */
    enum evse_state evse_state;
    /* faults: bitmap of which alarm slots are active (bit i = byte i nonzero) */
    unsigned int    fault_bits;
};

void charger_state_init(struct charger_state *cs);
void charger_state_read(struct charger_state *cs, const struct shmem *sm);

/* Human-readable EVSE-state string for publishing. */
const char *evse_state_str(enum evse_state s);

/* Dirty-flag bits returned by charger_state_diff(). */
#define CS_DIRTY_VOLTAGE    (1u << 0)
#define CS_DIRTY_CURRENT    (1u << 1)
#define CS_DIRTY_LINK       (1u << 2)
#define CS_DIRTY_HEARTBEAT  (1u << 3)
#define CS_DIRTY_EVSE_STATE (1u << 4)
#define CS_DIRTY_FAULTS     (1u << 5)

/* Returns the OR of CS_DIRTY_* bits for every field that differs between
 * `prev` and `cur`. 0 means identical. */
unsigned int charger_state_diff(const struct charger_state *prev,
                                const struct charger_state *cur);

/* Name of alarm slot `i` (0..CHARGER_MAX_FAULTS-1). Out-of-range returns
 * "UNKNOWN". Names are RE-derived; refine from docs/01 as bench work confirms. */
const char *charger_fault_name(int i);

#endif /* CHARGER_STATE_H */
