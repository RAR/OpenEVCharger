#include "bl0939.h"
#include "../core/pin_map.h"
#include "../hal/uart.h"
#include "../persist/calibration.h"
#include "gd32f20x.h"

/* Per-bit delay loop. ~600 NOPs at 120 MHz ≈ 5 µs → SCLK ~100 kHz
 * (well below the 900 kHz datasheet max). Tune up later once we've
 * validated the wiring. */
static inline void bb_delay(void)
{
    for (volatile int i = 0; i < 600; ++i) { __asm__ volatile (""); }
}

#define BL0939_HDR_READ   0x55u
#define BL0939_HDR_WRITE  0xA5u

void bl0939_init(void)
{
    /* SCLK idle LOW (mode 1). */
    gpio_bit_reset(PIN_BL0939_SCLK_PORT, PIN_BL0939_SCLK_PIN);
    gpio_init(PIN_BL0939_SCLK_PORT, GPIO_MODE_OUT_PP,
              GPIO_OSPEED_50MHZ, PIN_BL0939_SCLK_PIN);

    /* SDI idle HIGH (line is high during inter-frame). */
    gpio_bit_set(PIN_BL0939_SDI_PORT, PIN_BL0939_SDI_PIN);
    gpio_init(PIN_BL0939_SDI_PORT, GPIO_MODE_OUT_PP,
              GPIO_OSPEED_50MHZ, PIN_BL0939_SDI_PIN);

    /* SDO is hi-Z when BL0939 isn't driving (datasheet § 3.1.4).
     * Internal pull-up keeps the line high in those windows. */
    gpio_bit_set(PIN_BL0939_SDO_PORT, PIN_BL0939_SDO_PIN);
    gpio_init(PIN_BL0939_SDO_PORT, GPIO_MODE_IPU,
              GPIO_OSPEED_50MHZ, PIN_BL0939_SDO_PIN);
}

/* SPI mode 1 byte transfer: shift `out` MSB-first; sample SDO on
 * each falling SCLK. Returns the captured byte. */
static uint8_t bl0939_xfer(uint8_t out)
{
    uint8_t in = 0;
    for (int i = 7; i >= 0; --i) {
        if ((out >> i) & 1u) {
            gpio_bit_set(PIN_BL0939_SDI_PORT, PIN_BL0939_SDI_PIN);
        } else {
            gpio_bit_reset(PIN_BL0939_SDI_PORT, PIN_BL0939_SDI_PIN);
        }
        gpio_bit_set(PIN_BL0939_SCLK_PORT, PIN_BL0939_SCLK_PIN);
        bb_delay();
        gpio_bit_reset(PIN_BL0939_SCLK_PORT, PIN_BL0939_SCLK_PIN);
        if (gpio_input_bit_get(PIN_BL0939_SDO_PORT,
                               PIN_BL0939_SDO_PIN) == SET) {
            in |= (uint8_t)(1u << i);
        }
        bb_delay();
    }
    /* Hold SDI HIGH between frames. */
    gpio_bit_set(PIN_BL0939_SDI_PORT, PIN_BL0939_SDI_PIN);
    return in;
}

uint8_t bl0939_xfer_byte(uint8_t out) { return bl0939_xfer(out); }

/* Datasheet § 3.1.2: checksum = ~((header + ADDR + 3 data bytes) & 0xFF).
 * "Reversed bit by bit" in the (translated) datasheet means bitwise
 * NOT — confirmed by the equation form in the doc. */
static uint8_t bl0939_checksum(uint8_t header, uint8_t addr,
                               uint8_t dh, uint8_t dm, uint8_t dl)
{
    uint8_t s = (uint8_t)(header + addr + dh + dm + dl);
    return (uint8_t)~s;
}

void bl0939_soft_reset(void)
{
    /* § 3.1.5: 6 × 0xFF resets the SPI state machine. SDO discarded. */
    for (int i = 0; i < 6; ++i) (void)bl0939_xfer(0xFF);
}

int bl0939_read_register(uint8_t addr, uint32_t *val)
{
    /* Phase 1 (16 SCLKs): master sends header + addr; SDO is hi-Z.
     * Phase 2 (32 SCLKs): BL0939 drives DATA_H, DATA_M, DATA_L,
     * CHECKSUM out. Master keeps clocking; SDI doesn't matter
     * (chip ignores it after the address is latched). */
    (void)bl0939_xfer(BL0939_HDR_READ);
    (void)bl0939_xfer(addr);
    uint8_t dh = bl0939_xfer(0xFF);
    uint8_t dm = bl0939_xfer(0xFF);
    uint8_t dl = bl0939_xfer(0xFF);
    uint8_t cs = bl0939_xfer(0xFF);
    uint8_t ck = bl0939_checksum(BL0939_HDR_READ, addr, dh, dm, dl);
    if (val) *val = ((uint32_t)dh << 16) | ((uint32_t)dm << 8) | (uint32_t)dl;
    return (cs == ck) ? 0 : -1;
}

int bl0939_write_register(uint8_t addr, uint32_t v)
{
    uint8_t dh = (uint8_t)((v >> 16) & 0xFF);
    uint8_t dm = (uint8_t)((v >> 8) & 0xFF);
    uint8_t dl = (uint8_t)(v & 0xFF);
    uint8_t ck = bl0939_checksum(BL0939_HDR_WRITE, addr, dh, dm, dl);
    (void)bl0939_xfer(BL0939_HDR_WRITE);
    (void)bl0939_xfer(addr);
    (void)bl0939_xfer(dh);
    (void)bl0939_xfer(dm);
    (void)bl0939_xfer(dl);
    (void)bl0939_xfer(ck);
    return 0;
}

/* --- Periodic poll cache ----------------------------------------- */

/* Sign-extend a 24-bit two's complement value to int32_t. A_WATT and
 * B_WATT are signed per datasheet § 3.2 (active power flows in either
 * direction); the rest of the register set is unsigned. */
static int32_t bl0939_sx24(uint32_t v)
{
    return (v & 0x00800000u) ? (int32_t)(v | 0xFF000000u) : (int32_t)v;
}

static struct bl0939_readings s_readings;

void bl0939_poll(void)
{
    uint32_t v = 0, ia = 0, ib = 0, aw = 0, tp = 0;
    int rc_v  = bl0939_read_register(BL0939_REG_V_RMS,  &v);
    int rc_ia = bl0939_read_register(BL0939_REG_IA_RMS, &ia);
    int rc_ib = bl0939_read_register(BL0939_REG_IB_RMS, &ib);
    int rc_aw = bl0939_read_register(BL0939_REG_A_WATT, &aw);
    int rc_tp = bl0939_read_register(BL0939_REG_TPS1,   &tp);

    /* Build a local snapshot; copy to s_readings under IRQ disable so
     * readers (other tasks) never see a partial update. */
    struct bl0939_readings snap = s_readings;
    snap.poll_count++;
    if (rc_v == 0)  snap.v_rms  = v;       else snap.checksum_fail++;
    if (rc_ia == 0) snap.ia_rms = ia;      else snap.checksum_fail++;
    if (rc_ib == 0) snap.ib_rms = ib;      else snap.checksum_fail++;
    if (rc_aw == 0) snap.a_watt = bl0939_sx24(aw); else snap.checksum_fail++;
    if (rc_tp == 0) {
        snap.v_period = tp;
        /* TPS calibration: f_line × 10 = const / TPS. The BL0939's
         * internal period reference is ~27.19 kHz (not the 4 MHz MCLK
         * the datasheet implies); the exact value drifts unit-to-unit
         * with the on-die RC oscillator. Bench unit lands at 271900
         * (TPS≈453 at 60.02 Hz, Fluke 2026-05-04); other units differ
         * 10–20 %. Per-chassis cal via calibration_bl0939_freq_const()
         * (cal v3); falls back to CAL_DEFAULT_BL0939_FREQ_CONST when
         * uncalibrated. Guard divide-by-zero / nonsense values; TPS
         * < 50 would imply f > 540 Hz (not a power-line frequency). */
        int32_t fc = calibration_bl0939_freq_const();
        snap.v_freq_hz_x10 = (tp >= 50u && tp <= 0x00FFFFFFu && fc > 0)
            ? (uint16_t)((uint32_t)fc / tp) : 0u;
    } else {
        snap.checksum_fail++;
    }
    if ((rc_v | rc_ia | rc_ib | rc_aw | rc_tp) == 0) snap.valid = 1;

    /* The struct is 28 B; copy under IRQ disable is < 1 µs. */
    __asm__ volatile ("cpsid i" ::: "memory");
    s_readings = snap;
    __asm__ volatile ("cpsie i" ::: "memory");
}

void bl0939_get_readings(struct bl0939_readings *out)
{
    if (out == 0) return;
    __asm__ volatile ("cpsid i" ::: "memory");
    *out = s_readings;
    __asm__ volatile ("cpsie i" ::: "memory");
}

void bl0939_smoke_test(void)
{
    bl0939_init();
    bl0939_soft_reset();

    struct { uint8_t addr; const char *name; } regs[] = {
        { BL0939_REG_IA_FAST_RMS_CTRL, "IA_FAST_RMS_CTRL" },  /* def 0x00FFFF */
        { BL0939_REG_IB_FAST_RMS_CTRL, "IB_FAST_RMS_CTRL" },  /* def 0x00FFFF */
        { BL0939_REG_TPS_CTRL,         "TPS_CTRL"         },  /* def 0x0007FF */
        { BL0939_REG_V_RMS,            "V_RMS"            },
        { BL0939_REG_IA_RMS,           "IA_RMS"           },
        { BL0939_REG_IB_RMS,           "IB_RMS"           },
        { BL0939_REG_A_WATT,           "A_WATT"           },
        { BL0939_REG_B_WATT,           "B_WATT"           },
    };
    /* Note: our printk doesn't support width specifiers like %-20s,
     * so keep the format simple — each spec consumes exactly one arg
     * and we don't rely on alignment. */
    printk("bl0939: smoke (after soft reset):\n");
    for (size_t i = 0; i < sizeof(regs)/sizeof(regs[0]); ++i) {
        uint32_t v = 0;
        int rc = bl0939_read_register(regs[i].addr, &v);
        printk("  reg 0x%02x %s: rc=%d val=0x%06x%s\n",
               regs[i].addr, regs[i].name, rc, (unsigned)v,
               (rc == 0) ? "" : " (CHECKSUM FAIL)");
    }
    printk("bl0939: smoke done\n");
}
