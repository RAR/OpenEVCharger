#include <math.h>
#include <string.h>
#include "test_harness.h"
#include "shmem.h"
#include "shmem_offsets.h"
#include "charger_state.h"

#define APPROX(a, b, eps) (fabsf((float)(a) - (float)(b)) <= (float)(eps))

static void fill_le16(unsigned char *p, size_t off, unsigned v)
{
    p[off]     = (unsigned char)(v & 0xFF);
    p[off + 1] = (unsigned char)((v >> 8) & 0xFF);
}
static void fill_le32(unsigned char *p, size_t off, unsigned v)
{
    p[off]     = (unsigned char)(v & 0xFF);
    p[off + 1] = (unsigned char)((v >> 8) & 0xFF);
    p[off + 2] = (unsigned char)((v >> 16) & 0xFF);
    p[off + 3] = (unsigned char)((v >> 24) & 0xFF);
}

int main(void)
{
    /* Backing buffer reused across cases (cheaper than alloc per case). */
    static unsigned char raw[SHMEM_SIZE];

    /* ---------------- case 1: "rest" snapshot ----------------------------
     * Mostly zero; pilot=4 (transient noise), pri_state=0. Mirrors live
     * idle behaviour observed on the bench. */
    memset(raw, 0, sizeof(raw));
    raw[OFF_PILOT_STATE] = 4;            /* transient */
    raw[OFF_PRI_STATE]   = 0;
    struct shmem sm1 = { .base = raw, .size = sizeof(raw), .shmid = -1 };
    struct charger_state cs;
    charger_state_init(&cs);
    charger_state_read(&cs, &sm1);
    CHECK(APPROX(cs.voltage_v, 0.0f, 0.001f));
    CHECK(APPROX(cs.current_a, 0.0f, 0.001f));
    CHECK(APPROX(cs.power_w,   0.0f, 0.001f));
    CHECK_EQ(cs.pilot_state,    PILOT_TRANSIENT);
    CHECK_EQ(cs.pri_state,      0);
    CHECK_EQ(cs.user_state,     0);
    CHECK_EQ(cs.red_led,        0);
    CHECK_EQ(cs.pilot_duty_pct, 0);
    CHECK_EQ(cs.rated_amps,     0);
    CHECK_EQ(cs.stm32_fault_raw, 0);
    CHECK(cs.stm32_link_ok);
    CHECK_EQ(cs.fault_bits, 0u);

    /* ---------------- case 2: "normal charging" snapshot -----------------
     * Pilot=C, sensible V/I/P, link healthy, no alarms. */
    memset(raw, 0, sizeof(raw));
    fill_le16(raw, OFF_VRMS_MEAS,  2300);   /* 230.0 V */
    fill_le16(raw, OFF_IRMS_MEAS,   160);   /*  16.0 A */
    fill_le32(raw, OFF_POWER_MEAS, 3680000);/* 3680.0 W */
    raw[OFF_PILOT_STATE] = 0;               /* A */
    raw[OFF_PRI_STATE]   = 3;
    raw[OFF_USER_STATE]  = 2;
    raw[OFF_RED_LED]     = 0;
    raw[OFF_PILOT_DUTY]  = 50;
    raw[OFF_RATED_AMPS]  = 30;
    raw[OFF_STM32_FAULT] = 0;
    struct shmem sm2 = { .base = raw, .size = sizeof(raw), .shmid = -1 };
    charger_state_init(&cs);
    charger_state_read(&cs, &sm2);
    CHECK(APPROX(cs.voltage_v, 230.0f, 0.01f));
    CHECK(APPROX(cs.current_a,  16.0f, 0.01f));
    CHECK(APPROX(cs.power_w, 3680.0f, 0.5f));
    CHECK_EQ(cs.pilot_state,    PILOT_A);
    CHECK_EQ(cs.pri_state,      3);
    CHECK_EQ(cs.user_state,     2);
    CHECK_EQ(cs.red_led,        0);
    CHECK_EQ(cs.pilot_duty_pct, 50);
    CHECK_EQ(cs.rated_amps,     30);
    CHECK(cs.stm32_link_ok);
    CHECK_EQ(cs.fault_bits, 0u);

    /* ---------------- case 3: fault snapshot -----------------------------
     * Pilot=F, several alarm bits set, stm32 timeout bit set. */
    memset(raw, 0, sizeof(raw));
    raw[OFF_PILOT_STATE] = 5;                 /* F */
    raw[OFF_PRI_STATE]   = 5;
    raw[OFF_STM32_FAULT] = 0x10;              /* UART timeout bit */
    fill_le32(raw, OFF_ALARM_BITMAP,
              (1u << 0) | (1u << 4) | (1u << 19) | (1u << 31));
    struct shmem sm3 = { .base = raw, .size = sizeof(raw), .shmid = -1 };
    charger_state_init(&cs);
    charger_state_read(&cs, &sm3);
    CHECK_EQ(cs.pilot_state, PILOT_F);
    CHECK_EQ(cs.pri_state,   5);
    CHECK_EQ(cs.stm32_fault_raw, 0x10);
    CHECK(!cs.stm32_link_ok);
    CHECK_EQ(cs.fault_bits,
        (1u << 0) | (1u << 4) | (1u << 19) | (1u << 31));

    /* ---------------- case 4: pilot byte > 5 (defensive) ----------------- */
    memset(raw, 0, sizeof(raw));
    raw[OFF_PILOT_STATE] = 9;
    struct shmem sm4 = { .base = raw, .size = sizeof(raw), .shmid = -1 };
    charger_state_init(&cs);
    charger_state_read(&cs, &sm4);
    CHECK_EQ(cs.pilot_state, PILOT_UNKNOWN);

    /* ---------------- explicit little-endian byte order check ------------
     * Hard-codes the byte pattern at OFF_VRMS_MEAS to catch any future
     * accidental swap to big-endian. */
    memset(raw, 0, sizeof(raw));
    raw[OFF_VRMS_MEAS]     = 0x32;
    raw[OFF_VRMS_MEAS + 1] = 0x00;
    struct shmem sm5 = { .base = raw, .size = sizeof(raw), .shmid = -1 };
    charger_state_init(&cs);
    charger_state_read(&cs, &sm5);
    CHECK(APPROX(cs.voltage_v, 5.0f, 0.001f));         /* 0x0032 = 50 */

    raw[OFF_VRMS_MEAS]     = 0xff;
    raw[OFF_VRMS_MEAS + 1] = 0x00;
    charger_state_read(&cs, &sm5);
    CHECK(APPROX(cs.voltage_v, 25.5f, 0.001f));        /* 0x00ff = 255 */

    raw[OFF_VRMS_MEAS]     = 0x00;
    raw[OFF_VRMS_MEAS + 1] = 0x01;
    charger_state_read(&cs, &sm5);
    CHECK(APPROX(cs.voltage_v, 25.6f, 0.001f));        /* 0x0100 = 256 */

    /* ---------------- pilot_state_str ----------------------------------- */
    CHECK_STR(pilot_state_str(PILOT_A),         "A");
    CHECK_STR(pilot_state_str(PILOT_C),         "C");
    CHECK_STR(pilot_state_str(PILOT_F),         "F");
    CHECK_STR(pilot_state_str(PILOT_TRANSIENT), "transient");
    CHECK_STR(pilot_state_str(PILOT_UNKNOWN),   "unknown");

    /* ---------------- alarm-bit names ----------------------------------- */
    CHECK_STR(charger_fault_name(0),  "OVP");
    CHECK_STR(charger_fault_name(1),  "OCP");
    CHECK_STR(charger_fault_name(6),  "UVP");
    CHECK_STR(charger_fault_name(19), "PILOTERROR");
    CHECK_STR(charger_fault_name(31), "RFID_module_fail");
    CHECK_STR(charger_fault_name(32), "UNKNOWN");      /* boundary high */
    CHECK_STR(charger_fault_name(-1), "UNKNOWN");      /* boundary low  */

    /* ---------------- charger_state_diff -------------------------------- */
    struct charger_state a, b;
    charger_state_init(&a);
    charger_state_init(&b);
    CHECK_EQ(charger_state_diff(&a, &b), 0u);

    /* float epsilon: 0.01 should NOT cross the 0.05 V threshold */
    b.voltage_v = a.voltage_v + 0.01f;
    CHECK_EQ(charger_state_diff(&a, &b), 0u);
    b.voltage_v = a.voltage_v + 0.1f;
    CHECK_EQ(charger_state_diff(&a, &b) & CS_DIRTY_VOLTAGE, CS_DIRTY_VOLTAGE);
    b.voltage_v = a.voltage_v;

    b.current_a = a.current_a + 0.2f;
    CHECK_EQ(charger_state_diff(&a, &b) & CS_DIRTY_CURRENT, CS_DIRTY_CURRENT);
    b.current_a = a.current_a;

    b.power_w = a.power_w + 0.5f;             /* under 1 W eps */
    CHECK_EQ(charger_state_diff(&a, &b), 0u);
    b.power_w = a.power_w + 5.0f;
    CHECK_EQ(charger_state_diff(&a, &b) & CS_DIRTY_POWER, CS_DIRTY_POWER);
    b.power_w = a.power_w;

    b.pilot_state = PILOT_C;
    CHECK_EQ(charger_state_diff(&a, &b) & CS_DIRTY_PILOT_STATE, CS_DIRTY_PILOT_STATE);
    b.pilot_state = a.pilot_state;

    b.pri_state = 5;
    CHECK_EQ(charger_state_diff(&a, &b) & CS_DIRTY_PRI_STATE, CS_DIRTY_PRI_STATE);
    b.pri_state = a.pri_state;

    b.user_state = 2;
    CHECK_EQ(charger_state_diff(&a, &b) & CS_DIRTY_USER_STATE, CS_DIRTY_USER_STATE);
    b.user_state = a.user_state;

    b.red_led = 2;
    CHECK_EQ(charger_state_diff(&a, &b) & CS_DIRTY_RED_LED, CS_DIRTY_RED_LED);
    b.red_led = a.red_led;

    b.pilot_duty_pct = 75;
    CHECK_EQ(charger_state_diff(&a, &b) & CS_DIRTY_PILOT_DUTY, CS_DIRTY_PILOT_DUTY);
    b.pilot_duty_pct = a.pilot_duty_pct;

    b.rated_amps = 32;
    CHECK_EQ(charger_state_diff(&a, &b) & CS_DIRTY_RATED_AMPS, CS_DIRTY_RATED_AMPS);
    b.rated_amps = a.rated_amps;

    b.stm32_link_ok = !a.stm32_link_ok;
    CHECK_EQ(charger_state_diff(&a, &b) & CS_DIRTY_STM32_LINK, CS_DIRTY_STM32_LINK);
    b.stm32_link_ok = a.stm32_link_ok;

    b.fault_bits = 0x4;
    CHECK_EQ(charger_state_diff(&a, &b) & CS_DIRTY_FAULTS, CS_DIRTY_FAULTS);

    /* ---------------- fixture-file path (sanity end-to-end) -------------- */
    struct shmem sm_fx;
    CHECK_EQ(shmem_load_file(&sm_fx, "test/fixtures/shmem_snapshot.bin"), 0);
    charger_state_init(&cs);
    charger_state_read(&cs, &sm_fx);
    CHECK(APPROX(cs.voltage_v, 230.0f, 0.01f));
    CHECK(APPROX(cs.current_a,  16.0f, 0.01f));
    CHECK(APPROX(cs.power_w,     3.5f, 0.01f));
    CHECK_EQ(cs.pilot_state,    PILOT_C);
    CHECK_EQ(cs.user_state,     2);
    CHECK_EQ(cs.red_led,        2);
    CHECK_EQ(cs.pri_state,      3);
    CHECK_EQ(cs.pilot_duty_pct, 50);
    CHECK_EQ(cs.rated_amps,     30);
    CHECK_EQ(cs.fault_bits,     0x00000008u);    /* bit 3 = EMGSTOP */
    shmem_release(&sm_fx);

    TEST_MAIN_END();
}
