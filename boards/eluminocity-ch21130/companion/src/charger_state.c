#include "charger_state.h"
#include "shmem_offsets.h"
#include <assert.h>
#include <string.h>

void charger_state_init(struct charger_state *cs)
{
    memset(cs, 0, sizeof(*cs));
    cs->evse_state = EVSE_STATE_UNKNOWN;
}

/* Connector-state byte -> evse_state. Values from decode_sharemem.py /docs/02;
 * BENCH-VERIFY-PENDING (M0). Unknown bytes map to EVSE_STATE_UNKNOWN. */
static enum evse_state decode_connector(unsigned char b)
{
    switch (b) {
    case 0x00: return EVSE_STATE_IDLE;
    case 0x01: return EVSE_STATE_IDLE;        /* available, no cable */
    case 0x02: return EVSE_STATE_CONNECTED;   /* cable, not charging */
    case 0x03: return EVSE_STATE_CHARGING;
    case 0x04: return EVSE_STATE_FAULT;
    default:   return EVSE_STATE_UNKNOWN;
    }
}

void charger_state_read(struct charger_state *cs, const struct shmem *sm)
{
    /* v1 scaling is identity-with-units: the raw bytes are published as-is in
     * V / A. Real scale factors (and whether the per-unit Gain calibration
     * applies) are tuned against a multimeter at milestone M1 — when that
     * happens, only the two lines below change. */
    cs->voltage_v  = shmem_u8(sm, OFF_VRMS);
    cs->current_a  = shmem_u8(sm, OFF_IRMS);

    cs->stm32_link = shmem_u8(sm, OFF_STM32_LINK) ? 1 : 0;
    cs->heartbeat  = shmem_u8(sm, OFF_HEARTBEAT);
    cs->evse_state = decode_connector(shmem_u8(sm, OFF_CONNECTOR_STATE));

    unsigned char alarm[ALARM_BITMAP_LEN];
    shmem_copy(sm, OFF_ALARM_BITMAP, alarm, ALARM_BITMAP_LEN);
    cs->fault_bits = 0;
    static_assert(ALARM_BITMAP_LEN <= 32,
        "fault_bits must widen if ALARM_BITMAP_LEN grows past 32");
    for (int i = 0; i < ALARM_BITMAP_LEN; i++)
        if (alarm[i])
            cs->fault_bits |= (1u << i);
}

const char *evse_state_str(enum evse_state s)
{
    switch (s) {
    case EVSE_STATE_IDLE:      return "idle";
    case EVSE_STATE_CONNECTED: return "connected";
    case EVSE_STATE_CHARGING:  return "charging";
    case EVSE_STATE_FAULT:     return "fault";
    default:                   return "unknown";
    }
}
