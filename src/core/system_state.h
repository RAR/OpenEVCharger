#ifndef OPENBHZD_CORE_SYSTEM_STATE_H
#define OPENBHZD_CORE_SYSTEM_STATE_H

#include <stdint.h>

/* Snapshot exported by safety_task at the end of every 20 ms tick.
 * Comms / UI tasks can read it; safety_task is the sole writer.
 * No mutex — fields are independent enough that a brief stale read
 * during a tick boundary is acceptable for telemetry. The struct is
 * laid out word-aligned so each LDR is atomic.
 *
 * Layout matches spec § 5 system_state_t with fields trimmed to what
 * we currently track. Unfilled fields stay 0. */
struct __attribute__((packed)) openbhzd_state {
    uint8_t  j1772_state;       /* 'A'/'B'/'C'/'E'/'F' as enum value */
    uint8_t  evse_state;        /* see core/evse_state.h */
    uint8_t  advertised_amps;
    uint8_t  contactor_cmd;     /* 1 = closed, 0 = open */
    int16_t  cp_high_mv;
    int16_t  cp_low_mv;         /* 0 until M9-era diode check lands */
    uint16_t active_amps_x10;   /* 0 until CT calibration */
    uint16_t ntc1_dC;           /* 0 — not populated on bench */
    uint16_t ntc2_dC;           /* 0 — not populated on bench */
    uint16_t cc_max_amps;       /* 0 — CC sense pending */
    uint8_t  ac_present;        /* 0/1 */
    uint8_t  pad;
    uint32_t fault_active_bits; /* fault_state.active_bits */
    uint32_t first_fault_id;    /* fault_state.first_raised */
    uint32_t session_mwh;       /* 0 until M7 CT integration */
    uint16_t ac_adc_raw;        /* PA2 ADC rank 0 (12-bit). Scale to V
                                 * on the consumer side — calibration
                                 * is YAML-side via filter multiply.
                                 * Phase-1 voltage proxy until proper
                                 * RMS metering lands. */
    uint16_t ntc1_adc_raw;      /* PA3 ADC rank 1 (12-bit). Phase-1
                                 * raw exposure — °C conversion is
                                 * YAML-side / TBD until thermistor
                                 * part number is identified on a
                                 * production unit. Bench has no NTC
                                 * populated, reads ~1.7 V floating. */
    uint16_t ntc2_adc_raw;      /* PB0 ADC rank 7. Same caveat. */
};

void                  system_state_publish(const struct openbhzd_state *s);
struct openbhzd_state system_state_snapshot(void);

#endif /* OPENBHZD_CORE_SYSTEM_STATE_H */
