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

#endif /* CHARGER_STATE_H */
