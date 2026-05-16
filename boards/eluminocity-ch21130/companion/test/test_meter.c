/* test_meter — host tests for the meter personality. Covers:
 *   - meter_parse_response: LE assembly of 2/3/4-byte payloads from
 *     verbatim bytes captured in docs/13 trace
 *   - meter_load_cal: /Storage/Gain file parse incl. missing fields
 *   - meter_publish_shmem: shmem byte layout matches stock at the
 *     known offsets (0x0000..0x0005, 0x000c..0x000f, 0x015b..0x0167)
 *   - METER_STOCK_TERMIOS: byte-level invariants (c_cflag has CBAUD
 *     = B2400 + CS8 + CREAD + CLOCAL + HUPCL; VTIME at the right slot) */

#include "test_harness.h"
#include "meter.h"
#include "shmem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- parser: verbatim bytes from docs/13 trace ----------------- */

static void test_parse_responses(void)
{
    uint32_t v;

    /* 35 02 27 → resp `cb 59 26` (Vrms-ish, observed at t=21.154) */
    CHECK_EQ(meter_parse_response(0x02,
                                  (uint8_t[]){0xcb, 0x59, 0x26}, 3, &v), 0);
    CHECK_EQ(v, 0x002659cbu);                /* = 2,513,355 */

    /* 35 02 1c → resp `76 cb 37` (Irms-ish, observed at t=22.246) */
    CHECK_EQ(meter_parse_response(0x02,
                                  (uint8_t[]){0x76, 0xcb, 0x37}, 3, &v), 0);
    CHECK_EQ(v, 0x0037cb76u);                /* = 3,656,566 */

    /* 35 03 1a → resp `60 24 00 00` (Power, observed at t=22.114) */
    CHECK_EQ(meter_parse_response(0x03,
                                  (uint8_t[]){0x60, 0x24, 0x00, 0x00}, 4, &v), 0);
    CHECK_EQ(v, 0x00002460u);                /* = 9312 */

    /* 35 03 10 → resp `18 08 00 00` (Energy, observed at t=22.114) */
    CHECK_EQ(meter_parse_response(0x03,
                                  (uint8_t[]){0x18, 0x08, 0x00, 0x00}, 4, &v), 0);
    CHECK_EQ(v, 0x00000818u);                /* = 2072 */

    /* Chip ID (35 01 0e → resp 92 0e) */
    CHECK_EQ(meter_parse_response(0x01,
                                  (uint8_t[]){0x92, 0x0e}, 2, &v), 0);
    CHECK_EQ(v, 0x00000e92u);                /* = 3730 */

    /* Length mismatch → -1 */
    CHECK_EQ(meter_parse_response(0x03,
                                  (uint8_t[]){0x18, 0x08}, 2, &v), -1);
    /* Null out → -1 */
    CHECK_EQ(meter_parse_response(0x03,
                                  (uint8_t[]){0x18, 0x08, 0x00, 0x00}, 4, NULL),
             -1);
}

/* --- cal file parser ------------------------------------------- */

static void write_tmp(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "test setup: cannot write %s\n", path); exit(2); }
    fputs(content, f);
    fclose(f);
}

static void test_load_cal(void)
{
    struct meter_cal cal;
    const char *path = "/tmp/test_meter_gain.txt";

    /* Stock /Storage/Gain format (verbatim from docs/13 §2.1 trace). */
    write_tmp(path, "Vgain:342\nIgain:557\nWgain:3199\n\n");
    CHECK_EQ(meter_load_cal(path, &cal), 0);
    CHECK_EQ(cal.vgain, 342);
    CHECK_EQ(cal.igain, 557);
    CHECK_EQ(cal.wgain, 3199);

    /* Out-of-order is OK (we strstr for each key). */
    write_tmp(path, "Wgain:7\nIgain:5\nVgain:3\n");
    CHECK_EQ(meter_load_cal(path, &cal), 0);
    CHECK_EQ(cal.vgain, 3);
    CHECK_EQ(cal.igain, 5);
    CHECK_EQ(cal.wgain, 7);

    /* Missing key → still returns -1 but the field defaults to 1
     * (so /-by-Vgain doesn't blow up). */
    write_tmp(path, "Vgain:100\n");
    CHECK_EQ(meter_load_cal(path, &cal), -1);
    CHECK_EQ(cal.vgain, 100);
    CHECK_EQ(cal.igain, 1);                /* default */
    CHECK_EQ(cal.wgain, 1);                /* default */

    /* Zero gets clamped to 1 (no div-by-zero in publish path). */
    write_tmp(path, "Vgain:0\nIgain:0\nWgain:0\n");
    CHECK_EQ(meter_load_cal(path, &cal), 0);
    CHECK_EQ(cal.vgain, 1);
    CHECK_EQ(cal.igain, 1);
    CHECK_EQ(cal.wgain, 1);

    /* Missing file → all defaults, -1. */
    unlink(path);
    CHECK_EQ(meter_load_cal(path, &cal), -1);
    CHECK_EQ(cal.vgain, 1);
    CHECK_EQ(cal.igain, 1);
    CHECK_EQ(cal.wgain, 1);
}

/* --- shmem publish layout -------------------------------------- */

/* Build a writable shmem buffer that shmem_write_u* can operate on
 * (the "owned" path makes the buffer writable; matches what
 * shmem_load_file would set up for tests). */
static void mk_shm_buf(struct shmem *sm, size_t size)
{
    memset(sm, 0, sizeof *sm);
    sm->owned    = calloc(size, 1);
    sm->base     = sm->owned;
    sm->size     = size;
    sm->shmid    = -1;
    sm->writable = 1;
}

static void test_publish_shmem(void)
{
    struct shmem sm;
    mk_shm_buf(&sm, 0x2000);

    /* Pick readings that match the stock-bench means in docs/13. */
    struct meter_readings r = {
        .vrms_raw   = 2460566,             /* mean Vrms LE24 */
        .irms_raw   = 3659167,             /* mean Irms LE24 */
        .power_raw  = 12414,               /* mean P LE32 */
        .energy_raw = 2705,                /* mean E LE32 */
        .valid      = 1,
    };
    struct meter_cal cal = { .vgain = 342, .igain = 557, .wgain = 3199 };

    meter_publish_shmem(&sm, &r, &cal);

    /* 0x0000..0x0001: u16 LE = Vrms / Vgain.
     * 2460566 / 342 = 7194 = 0x1c1a (integer truncation) */
    CHECK_EQ(sm.owned[0x0000], 0x1a);
    CHECK_EQ(sm.owned[0x0001], 0x1c);

    /* 0x0004..0x0005: u16 LE = (Vrms / Vgain) / 10.
     * 7194 / 10 = 719 = 0x02cf */
    CHECK_EQ(sm.owned[0x0004], 0xcf);
    CHECK_EQ(sm.owned[0x0005], 0x02);

    /* 0x000c..0x000f: u32 LE = power_raw / 100.
     * 12414 / 100 = 124 = 0x0000007c */
    CHECK_EQ(sm.owned[0x000c], 0x7c);
    CHECK_EQ(sm.owned[0x000d], 0x00);
    CHECK_EQ(sm.owned[0x000e], 0x00);
    CHECK_EQ(sm.owned[0x000f], 0x00);

    /* 0x015b..0x015e: u32 LE = vrms_raw = 2460566 = 0x00258b96 */
    CHECK_EQ(sm.owned[0x015b], 0x96);
    CHECK_EQ(sm.owned[0x015c], 0x8b);
    CHECK_EQ(sm.owned[0x015d], 0x25);
    CHECK_EQ(sm.owned[0x015e], 0x00);

    /* 0x015f..0x0162: u32 LE = irms_raw = 3659167 = 0x0037d59f */
    CHECK_EQ(sm.owned[0x015f], 0x9f);
    CHECK_EQ(sm.owned[0x0160], 0xd5);
    CHECK_EQ(sm.owned[0x0161], 0x37);
    CHECK_EQ(sm.owned[0x0162], 0x00);

    /* 0x0163..0x0166: u32 LE = power_raw = 12414 = 0x0000307e */
    CHECK_EQ(sm.owned[0x0163], 0x7e);
    CHECK_EQ(sm.owned[0x0164], 0x30);
    CHECK_EQ(sm.owned[0x0165], 0x00);
    CHECK_EQ(sm.owned[0x0166], 0x00);

    /* 0x0167..0x016a: u32 LE = energy_raw = 2705 = 0x00000a91 */
    CHECK_EQ(sm.owned[0x0167], 0x91);
    CHECK_EQ(sm.owned[0x0168], 0x0a);
    CHECK_EQ(sm.owned[0x0169], 0x00);
    CHECK_EQ(sm.owned[0x016a], 0x00);

    /* Slots 4..9 (0x016b..0x017e) are left zero per design. */
    for (size_t off = 0x016b; off < 0x017f; off++)
        CHECK_EQ(sm.owned[off], 0x00);

    /* Defensive: r.valid=0 → no-op. */
    memset(sm.owned, 0xaa, sm.size);
    r.valid = 0;
    meter_publish_shmem(&sm, &r, &cal);
    CHECK_EQ(sm.owned[0x015b], 0xaa);     /* unchanged */

    /* Defensive: !sm.writable → no-op. */
    r.valid = 1;
    sm.writable = 0;
    memset(sm.owned, 0x55, sm.size);
    meter_publish_shmem(&sm, &r, &cal);
    CHECK_EQ(sm.owned[0x015b], 0x55);

    free(sm.owned);
}

/* --- termios sanity ------------------------------------------- */

static void test_stock_termios(void)
{
    /* c_cflag is u32 LE at positions 8..11. */
    uint32_t cflag = (uint32_t)METER_STOCK_TERMIOS[8]        |
                     (uint32_t)METER_STOCK_TERMIOS[9]  << 8  |
                     (uint32_t)METER_STOCK_TERMIOS[10] << 16 |
                     (uint32_t)METER_STOCK_TERMIOS[11] << 24;
    CHECK_EQ(cflag, 0x000008fcu);

    /* CBAUD nibble (lower 4 bits of low byte) = 0xc → B2400. */
    CHECK_EQ(cflag & 0x0000000fu, 0x0000000cu);

    /* CS8 = 0x30 (CSIZE bits set to b11). */
    CHECK_EQ((cflag & 0x00000030u), 0x00000030u);

    /* CREAD = 0x80, CLOCAL = 0x800 both set. */
    CHECK((cflag & 0x00000080u) != 0);
    CHECK((cflag & 0x00000800u) != 0);

    /* VTIME at c_cc[5] = position 22 = 0x0a (1s read timeout). */
    CHECK_EQ(METER_STOCK_TERMIOS[22], 0x0a);

    /* VMIN at c_cc[4] = position 21 should be 0 (non-blocking when no
     * data within VTIME). */
    CHECK_EQ(METER_STOCK_TERMIOS[21], 0x00);

    /* c_iflag, c_oflag, c_lflag, c_line all zero (raw mode). */
    for (size_t i = 0; i < 8; i++)
        CHECK_EQ(METER_STOCK_TERMIOS[i], 0x00);   /* c_iflag + c_oflag */
    for (size_t i = 12; i < 17; i++)
        CHECK_EQ(METER_STOCK_TERMIOS[i], 0x00);   /* c_lflag + c_line */
}

int main(void)
{
    test_parse_responses();
    test_load_cal();
    test_publish_shmem();
    test_stock_termios();
    TEST_MAIN_END();
}
