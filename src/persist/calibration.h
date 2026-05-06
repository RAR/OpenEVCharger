#ifndef OPENEVCHARGER_PERSIST_CALIBRATION_H
#define OPENEVCHARGER_PERSIST_CALIBRATION_H

#include <stdint.h>

#define CALIBRATION_SLOT_A   0x002000U
#define CALIBRATION_SLOT_B   0x003000U
#define CALIBRATION_VERSION  1U

/* Defaults committed M3.4.5 (bench Rippleon ROC001 2026-05-02).
 * Formula: cp_mv = (raw - anchor) * slope_num / slope_den + 12000 */
#define CAL_DEFAULT_CP_ANCHOR_RAW   1462
#define CAL_DEFAULT_CP_SLOPE_NUM    3540
#define CAL_DEFAULT_CP_SLOPE_DEN     459

/* 32 bytes total. Same envelope convention as boot_config (counter @ 4,
 * CRC @ end).
 *
 * BL0939 scales are *chassis* calibrations: BL0939 raw count → mains-
 * referenced engineering units. They fold the chip's internal datasheet
 * scale (V_pin = raw × Vref / 79793) with the OEM PCB's voltage divider
 * + CT ratio in one factor, so a single bench-cycle bench reference
 * (known mains V + clamp-meter A) lets us solve for them. Default 0 =
 * "uncalibrated" — the FC41D side leaves the engineering-unit entity
 * unpublished and exposes the raw count for diagnostic instead. */
struct __attribute__((packed)) calibration {
    uint8_t  version;                   /* 1 */
    uint8_t  pad0[3];
    uint32_t monotonic_counter;         /* helper-managed */
    int16_t  cp_anchor_raw;             /* raw ADC value at +12 V */
    int16_t  cp_slope_num;              /* mV/raw numerator */
    int16_t  cp_slope_den;              /* mV/raw denominator */
    int16_t  bl0939_v_uv_per_raw;       /* V_RMS raw × this = mains µV */
    int16_t  bl0939_ia_ua_per_raw;      /* IA_RMS raw × this = mains µA */
    int16_t  bl0939_ib_ua_per_raw;      /* IB_RMS raw × this = mains µA */
    int16_t  bl0939_pa_uw_per_raw;      /* A_WATT raw × this = mains µW
                                         * (matches v_uv_per_raw, ia_ua_per_raw).
                                         * May be NEGATIVE on boards where the
                                         * current-sense direction inverts
                                         * BL0939's expected polarity — sign
                                         * carries through; consumers gate on
                                         * != 0 (not > 0) and abs() the result
                                         * when accumulating positive-only
                                         * energy. */
    uint8_t  reserved[6];
    uint32_t crc32;                     /* helper-managed */
};
_Static_assert(sizeof(struct calibration) == 32, "calibration must be 32 B");

/* Load the calibration record into the in-RAM cache + publish to the
 * ISR-visible volatile cache. If both slots are invalid, writes
 * defaults to slot A. Returns 0 on success, <0 on error. */
int calibration_load(void);

/* Replace the CP anchor + slope and persist. Atomic w.r.t. the ADC EOC
 * ISR (interrupts disabled around the three field writes — < 1 µs).
 * Idempotent if all three fields match current state. Returns 0 on
 * success, <0 on error. */
int calibration_set_cp(int16_t anchor_raw, int16_t slope_num, int16_t slope_den);

/* ISR-side accessors. Read once per ISR; underlying storage is
 * volatile int32_t (single-word atomic on Cortex-M3). */
int32_t calibration_cp_anchor_raw(void);
int32_t calibration_cp_slope_num(void);
int32_t calibration_cp_slope_den(void);

/* BL0939 chassis-scale accessors. Return 0 if uncalibrated; consumers
 * treat 0 as "use raw count for diagnostic; don't compute engineering
 * units". */
int16_t calibration_bl0939_v_uv_per_raw(void);
int16_t calibration_bl0939_ia_ua_per_raw(void);
int16_t calibration_bl0939_ib_ua_per_raw(void);
int16_t calibration_bl0939_pa_uw_per_raw(void);

/* Replace the BL0939 chassis scales and persist. Idempotent if all
 * four fields match. Returns 0 on success, <0 on error. */
int calibration_set_bl0939(int16_t v_uv_per_raw,
                           int16_t ia_ua_per_raw,
                           int16_t ib_ua_per_raw,
                           int16_t pa_uw_per_raw);

#endif
