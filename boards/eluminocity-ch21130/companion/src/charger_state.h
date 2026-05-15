/* Normalized charger state: the hardware-independent struct the northbound
 * adapters publish. charger_state_read() pulls bytes from `shmem` and applies
 * scaling — ALL scaling lives in charger_state.c so the layout doc
 * (docs/06-shmem-RE-from-binaries.md) is the only spec source.
 *
 * v0.2: full rewrite against the RE-recovered shmem layout. The old single
 * `evse_state` enum (idle/connected/charging/fault) was a coarsening of three
 * independent bytes — pilot_state (raw cable physics), pri_state (Pri_Comm
 * digested EVSE state), user_state (OCPP/RFID-authorised user state). They
 * are now exposed separately.
 */
#ifndef CHARGER_STATE_H
#define CHARGER_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include "shmem.h"

enum pilot_state {
    PILOT_A         = 0,   /* 12 V, no plug */
    PILOT_B         = 1,   /* 9 V,  plug, not charging */
    PILOT_C         = 2,   /* 6 V,  charging */
    PILOT_D         = 3,   /* 3 V,  vent */
    PILOT_TRANSIENT = 4,   /* CP voltage out of all defined ranges */
    PILOT_F         = 5,   /* -12 V fault */
    PILOT_UNKNOWN   = 6,   /* raw byte > 5 (defensive) */
};

/* 32 alarm-bit name slots — see docs/06 §1 (AlarmMessage[32] table). */
#define CHARGER_MAX_FAULTS 32

struct charger_state {
    /* Metering (measured) */
    float    voltage_v;          /* VRMS_MEAS u16 LE / 10.0 */
    float    current_a;          /* IRMS_MEAS u16 LE / 10.0 */
    float    power_w;            /* POWER_MEAS u32 LE / 1000.0 */

    /* J1772 PWM + ampacity */
    uint8_t  pilot_duty_pct;     /* PILOT_DUTY raw u8 */
    uint8_t  rated_amps;         /* RATED_AMPS raw u8 */

    /* States — independent meanings, see docs/06 §3 */
    enum pilot_state pilot_state;
    uint8_t  pri_state;          /* PRI_STATE raw u8 (opaque enum) */
    uint8_t  user_state;         /* USER_STATE raw u8 (0=idle,1=auth,2=charging) */
    uint8_t  red_led;            /* RED_LED raw u8   (0=off,1=solid,2=flash) */

    /* STM32 link health */
    uint8_t  stm32_fault_raw;    /* OFF_STM32_FAULT raw byte */
    bool     stm32_link_ok;      /* (stm32_fault_raw & 0x10) == 0 */

    /* Alarm bitmap */
    uint32_t fault_bits;         /* OFF_ALARM_BITMAP raw u32 LE */
};

void charger_state_init(struct charger_state *cs);
void charger_state_read(struct charger_state *cs, const struct shmem *sm);

/* "A" / "B" / "C" / "D" / "transient" / "F" / "unknown" */
const char *pilot_state_str(enum pilot_state s);

/* Dirty-flag bits returned by charger_state_diff(). One bit per independently
 * publishable field. */
#define CS_DIRTY_VOLTAGE      (1u << 0)
#define CS_DIRTY_CURRENT      (1u << 1)
#define CS_DIRTY_POWER        (1u << 2)
#define CS_DIRTY_PILOT_DUTY   (1u << 3)
#define CS_DIRTY_RATED_AMPS   (1u << 4)
#define CS_DIRTY_PILOT_STATE  (1u << 5)
#define CS_DIRTY_PRI_STATE    (1u << 6)
#define CS_DIRTY_USER_STATE   (1u << 7)
#define CS_DIRTY_RED_LED      (1u << 8)
#define CS_DIRTY_STM32_LINK   (1u << 9)
#define CS_DIRTY_FAULTS       (1u << 10)

/* Returns the OR of CS_DIRTY_* bits for every field that differs between
 * `prev` and `cur`. 0 means identical. Floats compare with an epsilon
 * (0.05 for V/I, 1.0 for W) so 0.01-level jitter isn't republished. */
unsigned int charger_state_diff(const struct charger_state *prev,
                                const struct charger_state *cur);

/* Name of alarm bit `i` (0..CHARGER_MAX_FAULTS-1). Out-of-range returns
 * "UNKNOWN". Names taken verbatim from AlarmMessage[32] in Pri_Comm. */
const char *charger_fault_name(int i);

#endif /* CHARGER_STATE_H */
