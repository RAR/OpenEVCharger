#include "charger_state.h"
#include "shmem_offsets.h"
#include <math.h>
#include <string.h>

void charger_state_init(struct charger_state *cs)
{
    memset(cs, 0, sizeof(*cs));
    cs->pilot_state    = PILOT_UNKNOWN;
    cs->stm32_link_ok  = true;     /* timeout bit clear = link healthy */
}

static enum pilot_state decode_pilot(unsigned char b)
{
    switch (b) {
    case 0: return PILOT_A;
    case 1: return PILOT_B;
    case 2: return PILOT_C;
    case 3: return PILOT_D;
    case 4: return PILOT_TRANSIENT;
    case 5: return PILOT_F;
    default: return PILOT_UNKNOWN;
    }
}

void charger_state_read(struct charger_state *cs, const struct shmem *sm)
{
    /* Metering — read the cooked V/I/P/E our meter personality wrote at
     * 0x0500..0x050f. The old code read OFF_VRMS_MEAS / OFF_IRMS_MEAS /
     * OFF_POWER_MEAS directly at 0x0000/0x0004/0x000c, which was wrong
     * on three counts:
     *   1. 0x0004 holds (vrms/Vgain)/10 — voltage-derived, not current.
     *      "current" displayed as voltage/10 (e.g., 71 A on no load).
     *   2. 0x0000's scaling was an inferred /100 that didn't match the
     *      chip's actual centi-volts-after-Vgain scale (off by ~1.7×).
     *   3. POWER_MEAS at 0x000c is centi-units of `power_raw/100`, not
     *      watts directly.
     * Now meter personality does Vgain/Wgain math + empirical /60 V
     * scale (bench-fit 2026-05-16) and publishes integer fixed-point
     * units we just /100, /1000 here. See docs/13 §4.4. */
    cs->voltage_v       = (float)shmem_u32_le(sm, OFF_BRIDGE_VOLTAGE_CV) / 100.0f;
    cs->current_a       = (float)shmem_u32_le(sm, OFF_BRIDGE_CURRENT_MA) / 1000.0f;
    cs->power_w         = (float)shmem_u32_le(sm, OFF_BRIDGE_POWER_W);

    cs->pilot_duty_pct  = shmem_u8(sm, OFF_PILOT_DUTY);
    cs->rated_amps      = shmem_u8(sm, OFF_RATED_AMPS);

    cs->pilot_state     = decode_pilot(shmem_u8(sm, OFF_PILOT_STATE));
    cs->pri_state       = shmem_u8(sm, OFF_PRI_STATE);
    cs->user_state      = shmem_u8(sm, OFF_USER_STATE);
    cs->red_led         = shmem_u8(sm, OFF_RED_LED);

    cs->stm32_fault_raw = shmem_u8(sm, OFF_STM32_FAULT);
    cs->stm32_link_ok   = (cs->stm32_fault_raw & 0x10) == 0;

    cs->fault_bits      = shmem_u32_le(sm, OFF_ALARM_BITMAP);
}

const char *pilot_state_str(enum pilot_state s)
{
    switch (s) {
    case PILOT_A:         return "A";
    case PILOT_B:         return "B";
    case PILOT_C:         return "C";
    case PILOT_D:         return "D";
    case PILOT_TRANSIENT: return "transient";
    case PILOT_F:         return "F";
    default:              return "unknown";
    }
}

static int float_differs(float a, float b, float eps)
{
    return fabsf(a - b) > eps;
}

unsigned int charger_state_diff(const struct charger_state *prev,
                                const struct charger_state *cur)
{
    unsigned int d = 0;
    if (float_differs(prev->voltage_v, cur->voltage_v, 0.05f))  d |= CS_DIRTY_VOLTAGE;
    if (float_differs(prev->current_a, cur->current_a, 0.05f))  d |= CS_DIRTY_CURRENT;
    if (float_differs(prev->power_w,   cur->power_w,   1.0f))   d |= CS_DIRTY_POWER;
    if (prev->pilot_duty_pct != cur->pilot_duty_pct)            d |= CS_DIRTY_PILOT_DUTY;
    if (prev->rated_amps     != cur->rated_amps)                d |= CS_DIRTY_RATED_AMPS;
    if (prev->pilot_state    != cur->pilot_state)               d |= CS_DIRTY_PILOT_STATE;
    if (prev->pri_state      != cur->pri_state)                 d |= CS_DIRTY_PRI_STATE;
    if (prev->user_state     != cur->user_state)                d |= CS_DIRTY_USER_STATE;
    /* "authorized" is a thresholded view (>=1) of user_state; only mark
     * dirty when that boolean transitions, so HA isn't spammed when
     * user_state cycles 1↔2 (auth↔charging). */
    if ((prev->user_state >= 1) != (cur->user_state >= 1))       d |= CS_DIRTY_AUTHORIZE;
    if (prev->red_led        != cur->red_led)                   d |= CS_DIRTY_RED_LED;
    if (prev->stm32_link_ok  != cur->stm32_link_ok)             d |= CS_DIRTY_STM32_LINK;
    if (prev->fault_bits     != cur->fault_bits)                d |= CS_DIRTY_FAULTS;
    return d;
}

/* Alarm-bit -> name. Verbatim from AlarmMessage[32] in Pri_Comm
 * (docs/06-shmem-RE-from-binaries.md §1). Bit 0 = LSB of the LE u32 at
 * OFF_ALARM_BITMAP. */
static const char *const FAULT_NAMES[CHARGER_MAX_FAULTS] = {
    /*  0 */ "OVP",
    /*  1 */ "OCP",
    /*  2 */ "Ambient_OTP",
    /*  3 */ "EMGSTOP",
    /*  4 */ "RCD",
    /*  5 */ "WELDING",
    /*  6 */ "UVP",
    /*  7 */ "RA_CPU",
    /*  8 */ "RA_WATCHDOG",
    /*  9 */ "RA_CLOCK",
    /* 10 */ "RA_DATA",
    /* 11 */ "RA_FLASH",
    /* 12 */ "RA_RAM",
    /* 13 */ "RA_INTERRUPT",
    /* 14 */ "RA_TIMING",
    /* 15 */ "RA_IO",
    /* 16 */ "RA_ADC",
    /* 17 */ "RCDTRIP",
    /* 18 */ "GMI",
    /* 19 */ "PILOTERROR",
    /* 20 */ "INITIAL",
    /* 21 */ "Ambient_NTC_fail",
    /* 22 */ "Plug_OTP",
    /* 23 */ "Plug_NTC_fail",
    /* 24 */ "RCDLOCK",
    /* 25 */ "AC_drop",
    /* 26 */ "FW_upgrade_fail",
    /* 27 */ "PILOTERROR_Negative",
    /* 28 */ "Relay_driver_fault",
    /* 29 */ "Pri_MCU_Lost",
    /* 30 */ "WiFi_module_fail",
    /* 31 */ "RFID_module_fail",
};

const char *charger_fault_name(int i)
{
    if (i < 0 || i >= CHARGER_MAX_FAULTS)
        return "UNKNOWN";
    return FAULT_NAMES[i];
}
