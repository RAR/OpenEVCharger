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
    uint16_t ac_adc_raw;        /* PA2 ADC rank 0 (12-bit). Despite
                                 * the legacy "AC" name, this channel
                                 * is actually the GUN-cable NTC
                                 * thermistor — bench-confirmed by
                                 * grounding the front-block "NTC"
                                 * pin and watching this raw drop to
                                 * 0. β-model conversion FC41D-side. */
    uint16_t ntc1_adc_raw;      /* PA3 ADC rank 1 (12-bit). WALL-PLUG
                                 * end NTC thermistor (also bench-
                                 * confirmed via the matching front-
                                 * block pin). β-model conversion
                                 * FC41D-side. Both gun and wall NTCs
                                 * route through the front connector
                                 * for assembly convenience. */
    uint16_t ntc2_adc_raw;      /* PB0 ADC rank 7. NOT a thermistor —
                                 * see pin_map.h; likely AC-mains-
                                 * presence sense. OVER_TEMP detector
                                 * masks this channel via
                                 * OPENBHZD_NTC2_PRESENT (default 0). */

    /* BL0939 metering chip — raw 24-bit register reads. Engineering-
     * unit conversion (V, A, W) is done FC41D-side via per-chassis
     * calibration scales loaded from boot_config. Until the chassis
     * has been calibrated against a Fluke + clamp, these stay as raw
     * counts in HA. Polled every 400 ms by safety_task. */
    uint32_t bl0939_v_rms;      /* 0x06 — voltage RMS, unsigned 24-bit */
    uint32_t bl0939_ia_rms;     /* 0x04 — current A RMS, unsigned 24-bit */
    uint32_t bl0939_ib_rms;     /* 0x05 — current B RMS, unsigned 24-bit */
    int32_t  bl0939_a_watt;     /* 0x08 — channel A active power, sign-extended */
    uint8_t  bl0939_valid;      /* 1 once any read has succeeded */
    uint8_t  bl0939_pad[3];
};
/* Hits TLV_PAYLOAD_MAX (56 B) exactly — any future field addition needs
 * a payload-size bump on both sides, or a separate event/cmd. */
_Static_assert(sizeof(struct openbhzd_state) == 56,
               "openbhzd_state must equal TLV_PAYLOAD_MAX (56 B)");

void                  system_state_publish(const struct openbhzd_state *s);
struct openbhzd_state system_state_snapshot(void);

#endif /* OPENBHZD_CORE_SYSTEM_STATE_H */
