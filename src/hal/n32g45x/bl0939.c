/* boards/nexcyber/hal/bl0939.c — M3 BL0939 metering driver.
 *
 * Protocol layer is a near-verbatim port of src/hal/bl0939.c on
 * the rippleon target — same frame format (0x55/0xA5 + addr + 3
 * data bytes + ~sum checksum), same register map. The only changes
 * are transport (hardware SPI2 via spi2_xfer instead of bit-bang)
 * and a hardcoded freq-cal constant until persist/calibration ports
 * across.
 */

#include "hal/bl0939.h"
#include "spi2.h"
#include "hal/uart.h"
#include "n32g45x.h"

#define BL0939_HDR_READ   0x55u
#define BL0939_HDR_WRITE  0xA5u

/* Bench-cal frequency constant — same fallback rippleon uses when
 * calibration storage is empty. Replace with a per-chassis calibrated
 * value once persist/calibration arrives on the nexcyber port. */
#define BL0939_FREQ_CONST_DEFAULT  271900

/* SPI mode 1 byte transfer, CS-bracketed. spi2_xfer handles the
 * actual wire toggle; this just owns CS framing per BL0939 frame. */
static uint8_t bl0939_xfer(uint8_t out)
{
    return spi2_xfer(out);
}

/* Datasheet § 3.1.2: checksum = ~((header + ADDR + 3 data bytes) & 0xFF). */
static uint8_t bl0939_checksum(uint8_t header, uint8_t addr,
                               uint8_t dh, uint8_t dm, uint8_t dl)
{
    uint8_t s = (uint8_t)(header + addr + dh + dm + dl);
    return (uint8_t)~s;
}

void bl0939_soft_reset(void)
{
    /* § 3.1.5: 6 × 0xFF resets the chip's SPI state machine.
     * Bracketed in CS — the BL0939 needs CS held LOW for the entire
     * reset frame to count as one transaction. */
    spi2_cs_assert();
    for (int i = 0; i < 6; ++i) (void)bl0939_xfer(0xFF);
    spi2_cs_deassert();
}

int bl0939_read_register(uint8_t addr, uint32_t *val)
{
    /* CS asserted for the whole 6-byte frame: header + addr + 3 data
     * bytes + checksum (BL0939 drives SDO on the latter four). */
    spi2_cs_assert();
    (void)bl0939_xfer(BL0939_HDR_READ);
    (void)bl0939_xfer(addr);
    uint8_t dh = bl0939_xfer(0xFF);
    uint8_t dm = bl0939_xfer(0xFF);
    uint8_t dl = bl0939_xfer(0xFF);
    uint8_t cs = bl0939_xfer(0xFF);
    spi2_cs_deassert();

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

    spi2_cs_assert();
    (void)bl0939_xfer(BL0939_HDR_WRITE);
    (void)bl0939_xfer(addr);
    (void)bl0939_xfer(dh);
    (void)bl0939_xfer(dm);
    (void)bl0939_xfer(dl);
    (void)bl0939_xfer(ck);
    spi2_cs_deassert();
    return 0;
}

/* --- Periodic poll cache ----------------------------------------- */

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

    struct bl0939_readings snap = s_readings;
    snap.poll_count++;
    if (rc_v == 0)  snap.v_rms  = v;       else snap.checksum_fail++;
    if (rc_ia == 0) snap.ia_rms = ia;      else snap.checksum_fail++;
    if (rc_ib == 0) snap.ib_rms = ib;      else snap.checksum_fail++;
    if (rc_aw == 0) snap.a_watt = bl0939_sx24(aw); else snap.checksum_fail++;
    if (rc_tp == 0) {
        snap.v_period = tp;
        snap.v_freq_hz_x10 = (tp >= 50u && tp <= 0x00FFFFFFu)
            ? (uint16_t)((uint32_t)BL0939_FREQ_CONST_DEFAULT / tp) : 0u;
    } else {
        snap.checksum_fail++;
    }
    if ((rc_v | rc_ia | rc_ib | rc_aw | rc_tp) == 0) snap.valid = 1;

    __disable_irq();
    s_readings = snap;
    __enable_irq();
}

void bl0939_get_readings(struct bl0939_readings *out)
{
    if (out == 0) return;
    __disable_irq();
    *out = s_readings;
    __enable_irq();
}

void bl0939_smoke_test(void)
{
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
    printk("bl0939: smoke (after soft reset):\n");
    for (unsigned i = 0; i < sizeof(regs)/sizeof(regs[0]); ++i) {
        uint32_t val = 0;
        int rc = bl0939_read_register(regs[i].addr, &val);
        printk("  reg 0x%x %s: rc=%d val=0x%x%s\n",
               regs[i].addr, regs[i].name, rc, (unsigned)val,
               (rc == 0) ? "" : " (CHECKSUM FAIL)");
    }
    printk("bl0939: smoke done\n");
}
