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

unsigned int charger_state_diff(const struct charger_state *prev,
                                const struct charger_state *cur)
{
    unsigned int d = 0;
    if (prev->voltage_v  != cur->voltage_v)  d |= CS_DIRTY_VOLTAGE;
    if (prev->current_a  != cur->current_a)  d |= CS_DIRTY_CURRENT;
    if (prev->stm32_link != cur->stm32_link) d |= CS_DIRTY_LINK;
    if (prev->heartbeat  != cur->heartbeat)  d |= CS_DIRTY_HEARTBEAT;
    if (prev->evse_state != cur->evse_state) d |= CS_DIRTY_EVSE_STATE;
    if (prev->fault_bits != cur->fault_bits) d |= CS_DIRTY_FAULTS;
    return d;
}

/* Alarm-slot names. RE-derived from docs/01 / docs/02 ("31-alarm fault catalog
 * confirmed from Pri_Comm .data"). Slots whose name is not yet confirmed ship
 * as "RESERVED_nn" — a real, shippable value, refined as bench work confirms.
 * BENCH-VERIFY-PENDING: also confirm byte-vs-bit semantics of the bitmap (M0). */
static const char *const FAULT_NAMES[CHARGER_MAX_FAULTS] = {
    "RCD",          "RCDTRIP",      "GMI",          "OVP",
    "UVP",          "OCP",          "WELDING",      "PILOTERROR",
    "AMBIENT_OTP",  "RA_WATCHDOG",  "RA_CPU",       "RA_RAM",
    "RESERVED_12",  "RESERVED_13",  "RESERVED_14",  "RESERVED_15",
    "RESERVED_16",  "RESERVED_17",  "RESERVED_18",  "RESERVED_19",
    "RESERVED_20",  "RESERVED_21",  "RESERVED_22",  "RESERVED_23",
    "RESERVED_24",  "RESERVED_25",  "RESERVED_26",  "RESERVED_27",
    "RESERVED_28",  "RESERVED_29",  "RESERVED_30",  "RESERVED_31",
};

const char *charger_fault_name(int i)
{
    if (i < 0 || i >= CHARGER_MAX_FAULTS)
        return "UNKNOWN";
    return FAULT_NAMES[i];
}
