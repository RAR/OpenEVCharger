#include "calibration.h"
#include "pingpong.h"
#include "../hal/uart.h"
#include "gd32f20x.h"
#include <string.h>

static struct calibration s_cal;

/* ISR-visible cache. Updated by load() and set_cp() with IRQs masked. */
static volatile int32_t s_anchor_raw = CAL_DEFAULT_CP_ANCHOR_RAW;
static volatile int32_t s_slope_num  = CAL_DEFAULT_CP_SLOPE_NUM;
static volatile int32_t s_slope_den  = CAL_DEFAULT_CP_SLOPE_DEN;

int32_t calibration_cp_anchor_raw(void) { return s_anchor_raw; }
int32_t calibration_cp_slope_num(void)  { return s_slope_num; }
int32_t calibration_cp_slope_den(void)  { return s_slope_den; }

int16_t calibration_bl0939_v_uv_per_raw(void)
{
    return s_cal.bl0939_v_uv_per_raw;
}
int16_t calibration_bl0939_ia_ua_per_raw(void)
{
    return s_cal.bl0939_ia_ua_per_raw;
}
int16_t calibration_bl0939_ib_ua_per_raw(void)
{
    return s_cal.bl0939_ib_ua_per_raw;
}
int16_t calibration_bl0939_pa_mw_per_raw(void)
{
    return s_cal.bl0939_pa_mw_per_raw;
}

int calibration_set_bl0939(int16_t v_uv_per_raw,
                           int16_t ia_ua_per_raw,
                           int16_t ib_ua_per_raw,
                           int16_t pa_mw_per_raw)
{
    if (s_cal.bl0939_v_uv_per_raw  == v_uv_per_raw  &&
        s_cal.bl0939_ia_ua_per_raw == ia_ua_per_raw &&
        s_cal.bl0939_ib_ua_per_raw == ib_ua_per_raw &&
        s_cal.bl0939_pa_mw_per_raw == pa_mw_per_raw) {
        return 0;
    }

    s_cal.version              = CALIBRATION_VERSION;
    s_cal.bl0939_v_uv_per_raw  = v_uv_per_raw;
    s_cal.bl0939_ia_ua_per_raw = ia_ua_per_raw;
    s_cal.bl0939_ib_ua_per_raw = ib_ua_per_raw;
    s_cal.bl0939_pa_mw_per_raw = pa_mw_per_raw;

    uint8_t  slot = 0;
    uint32_t counter = 0;
    int rc = pingpong_store(CALIBRATION_SLOT_A, CALIBRATION_SLOT_B,
                            &s_cal, sizeof s_cal, &slot, &counter);
    if (rc < 0) {
        printk("calibration: BL0939 store FAIL rc=%d\n", rc);
        return rc;
    }
    printk("calibration: BL0939 stored -> slot %c (counter=%u, "
           "v=%d uV/raw, ia=%d uA/raw, ib=%d uA/raw, pa=%d mW/raw)\n",
           'A' + slot, (unsigned)counter,
           (int)v_uv_per_raw, (int)ia_ua_per_raw,
           (int)ib_ua_per_raw, (int)pa_mw_per_raw);
    return 0;
}

static void publish_to_isr(void)
{
    __disable_irq();
    s_anchor_raw = s_cal.cp_anchor_raw;
    s_slope_num  = s_cal.cp_slope_num;
    s_slope_den  = s_cal.cp_slope_den;
    __enable_irq();
}

int calibration_load(void)
{
    uint8_t  slot = 0;
    uint32_t counter = 0;
    int rc = pingpong_load(CALIBRATION_SLOT_A, CALIBRATION_SLOT_B,
                           &s_cal, sizeof s_cal, &slot, &counter);
    if (rc < 0) {
        printk("calibration: pingpong_load FAIL rc=%d\n", rc);
        return rc;
    }
    if (rc == 1) {
        memset(&s_cal, 0, sizeof s_cal);
        s_cal.version       = CALIBRATION_VERSION;
        s_cal.cp_anchor_raw = CAL_DEFAULT_CP_ANCHOR_RAW;
        s_cal.cp_slope_num  = CAL_DEFAULT_CP_SLOPE_NUM;
        s_cal.cp_slope_den  = CAL_DEFAULT_CP_SLOPE_DEN;

        rc = pingpong_store(CALIBRATION_SLOT_A, CALIBRATION_SLOT_B,
                            &s_cal, sizeof s_cal, &slot, &counter);
        if (rc < 0) {
            printk("calibration: defaults write FAIL rc=%d\n", rc);
            return rc;
        }
        printk("calibration: defaults written -> slot %c (counter=%u, anchor=%d, slope=%d/%d)\n",
               'A' + slot, (unsigned)counter,
               (int)s_cal.cp_anchor_raw,
               (int)s_cal.cp_slope_num,
               (int)s_cal.cp_slope_den);
        publish_to_isr();
        return 0;
    }

    if (s_cal.version != CALIBRATION_VERSION) {
        printk("calibration: unknown version=%u, using as-is\n",
               (unsigned)s_cal.version);
    }
    printk("calibration: loaded from slot %c (counter=%u, anchor=%d, slope=%d/%d)\n",
           'A' + slot, (unsigned)counter,
           (int)s_cal.cp_anchor_raw,
           (int)s_cal.cp_slope_num,
           (int)s_cal.cp_slope_den);
    publish_to_isr();
    return 0;
}

int calibration_set_cp(int16_t anchor_raw, int16_t slope_num, int16_t slope_den)
{
    if (slope_den == 0) return -1;

    if (s_cal.cp_anchor_raw == anchor_raw &&
        s_cal.cp_slope_num  == slope_num &&
        s_cal.cp_slope_den  == slope_den) {
        return 0;
    }

    s_cal.version       = CALIBRATION_VERSION;
    s_cal.cp_anchor_raw = anchor_raw;
    s_cal.cp_slope_num  = slope_num;
    s_cal.cp_slope_den  = slope_den;

    uint8_t  slot = 0;
    uint32_t counter = 0;
    int rc = pingpong_store(CALIBRATION_SLOT_A, CALIBRATION_SLOT_B,
                            &s_cal, sizeof s_cal, &slot, &counter);
    if (rc < 0) {
        printk("calibration: store FAIL rc=%d\n", rc);
        return rc;
    }
    printk("calibration: stored -> slot %c (counter=%u, anchor=%d, slope=%d/%d)\n",
           'A' + slot, (unsigned)counter,
           (int)anchor_raw, (int)slope_num, (int)slope_den);
    publish_to_isr();
    return 0;
}
