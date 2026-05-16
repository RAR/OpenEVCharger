/* test_adc — host tests for the adc personality. Covers:
 *   - ADC_INIT_CFG_1/2 byte layout matches rodata captured from
 *     /root/Adc disassembly
 *   - adc_classify_sample: 6 voltage classes + transient guard band,
 *     boundaries match the J1772 spec mapping
 *   - adc_window_state: majority vote ≥75%, bimodal rail-rail =
 *     TRANSIENT, ambiguous = hold previous (hysteresis)
 *   - bench-idle realistic data (mostly PS_F + a few PS_A) → TRANSIENT
 *     (matches docs/13 §3.2 stock behavior)
 */
#include "test_harness.h"
#include "adc.h"

#include <stdint.h>
#include <string.h>

/* --- ioctl arg byte layout --- */

static void test_init_cfg_bytes(void)
{
    /* From /root/Adc rodata at 0xabec — first 16 bytes. */
    CHECK_EQ(ADC_INIT_CFG_1[0],  0x01);
    CHECK_EQ(ADC_INIT_CFG_1[1],  0x00);
    CHECK_EQ(ADC_INIT_CFG_1[2],  0x00);
    CHECK_EQ(ADC_INIT_CFG_1[3],  0x00);
    CHECK_EQ(ADC_INIT_CFG_1[8],  0xc4);    /* 2500 LE32 byte 0 */
    CHECK_EQ(ADC_INIT_CFG_1[9],  0x09);    /* 2500 LE32 byte 1 */

    /* Second struct stock constructs in main(): {0,0,1,0}. */
    CHECK_EQ(ADC_INIT_CFG_2[0],  0x00);
    CHECK_EQ(ADC_INIT_CFG_2[4],  0x00);
    CHECK_EQ(ADC_INIT_CFG_2[8],  0x01);
    CHECK_EQ(ADC_INIT_CFG_2[9],  0x00);
    CHECK_EQ(ADC_INIT_CFG_2[12], 0x00);
}

/* --- single-sample classifier --- */

static void test_classify_rails(void)
{
    /* Bench-idle low cluster (CP -11.9V) → PS_F */
    CHECK_EQ(adc_classify_sample(94),  PS_F);
    CHECK_EQ(adc_classify_sample(97),  PS_F);
    CHECK_EQ(adc_classify_sample(99),  PS_F);
    CHECK_EQ(adc_classify_sample(104), PS_F);

    /* Bench-idle high cluster (CP near +12V) → PS_A */
    CHECK_EQ(adc_classify_sample(232), PS_A);
    CHECK_EQ(adc_classify_sample(236), PS_A);
    CHECK_EQ(adc_classify_sample(240), PS_A);
    CHECK_EQ(adc_classify_sample(255), PS_A);
}

static void test_classify_states_bcd(void)
{
    /* PS_B band: 207..231 */
    CHECK_EQ(adc_classify_sample(207), PS_B);
    CHECK_EQ(adc_classify_sample(220), PS_B);
    CHECK_EQ(adc_classify_sample(231), PS_B);

    /* PS_C band: 188..206 */
    CHECK_EQ(adc_classify_sample(188), PS_C);
    CHECK_EQ(adc_classify_sample(195), PS_C);
    CHECK_EQ(adc_classify_sample(206), PS_C);

    /* PS_D band: 170..187 */
    CHECK_EQ(adc_classify_sample(170), PS_D);
    CHECK_EQ(adc_classify_sample(180), PS_D);
    CHECK_EQ(adc_classify_sample(187), PS_D);
}

static void test_classify_transient(void)
{
    /* Between F (≤104) and D (≥170): 105..169 = TRANSIENT */
    CHECK_EQ(adc_classify_sample(105), PS_TRANSIENT);
    CHECK_EQ(adc_classify_sample(140), PS_TRANSIENT);
    CHECK_EQ(adc_classify_sample(169), PS_TRANSIENT);
    /* Edge: 0 → PS_F (much below -12V). */
    CHECK_EQ(adc_classify_sample(0),   PS_F);
}

/* --- window state decision --- */

static void test_window_dominant_class(void)
{
    /* 12 of 16 = 75% → state wins */
    enum pilot_state win[16];
    for (int i = 0; i < 12; i++) win[i] = PS_A;
    for (int i = 12; i < 16; i++) win[i] = PS_TRANSIENT;
    CHECK_EQ(adc_window_state(win, 16, PS_F), PS_A);

    /* 11 of 16 = 68% → does NOT win → holds prev */
    for (int i = 0; i < 11; i++) win[i] = PS_C;
    for (int i = 11; i < 16; i++) win[i] = PS_TRANSIENT;
    CHECK_EQ(adc_window_state(win, 16, PS_B), PS_B);

    /* Pure single-class window → wins easily */
    for (int i = 0; i < 16; i++) win[i] = PS_D;
    CHECK_EQ(adc_window_state(win, 16, PS_A), PS_D);
}

static void test_window_rail_bimodal(void)
{
    /* Bench-idle: 15× PS_F + 1× PS_A → bimodal rail-rail → TRANSIENT
     * (matches docs/13 §3.2 observation: stock publishes PILOT_STATE=4) */
    enum pilot_state win[16];
    for (int i = 0; i < 15; i++) win[i] = PS_F;
    win[15] = PS_A;
    CHECK_EQ(adc_window_state(win, 16, PS_A), PS_TRANSIENT);

    /* Pure PS_F (no spikes to A) — but no mids — should be PS_F (16/16 dom) */
    for (int i = 0; i < 16; i++) win[i] = PS_F;
    CHECK_EQ(adc_window_state(win, 16, PS_A), PS_F);

    /* 15× PS_F + 1× PS_C (mid noise sample) — dominant PS_F wins. The
     * single PS_C is too thin to matter; if it WERE a real C transition
     * it would persist across windows and eventually dominate. */
    win[15] = PS_C;
    CHECK_EQ(adc_window_state(win, 16, PS_B), PS_F);
}

static void test_window_holds_prev_when_ambiguous(void)
{
    /* Equal split B/C/D — no dominant, no rail-rail → hold prev */
    enum pilot_state win[16];
    for (int i = 0; i < 6; i++)  win[i] = PS_B;
    for (int i = 6; i < 11; i++) win[i] = PS_C;
    for (int i = 11; i < 16; i++) win[i] = PS_D;
    CHECK_EQ(adc_window_state(win, 16, PS_A), PS_A);
}

static void test_window_edge_cases(void)
{
    /* Empty window → hold prev */
    enum pilot_state none[1] = { PS_A };
    CHECK_EQ(adc_window_state(none, 0, PS_B), PS_B);
}

int main(void)
{
    test_init_cfg_bytes();
    test_classify_rails();
    test_classify_states_bcd();
    test_classify_transient();
    test_window_dominant_class();
    test_window_rail_bimodal();
    test_window_holds_prev_when_ambiguous();
    test_window_edge_cases();
    TEST_MAIN_END();
}
