#include "bl0939.h"
#include "../core/pin_map.h"
#include "../hal/uart.h"
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
    printk("BL0939 smoke (after soft reset):\n");
    for (size_t i = 0; i < sizeof(regs)/sizeof(regs[0]); ++i) {
        uint32_t v = 0;
        int rc = bl0939_read_register(regs[i].addr, &v);
        printk("  reg 0x%02x %-20s rc=%d val=0x%06x %s\n",
               regs[i].addr, regs[i].name, rc, (unsigned)v,
               (rc == 0) ? "" : "(CHECKSUM FAIL)");
    }
    printk("BL0939 smoke: done\n");
}
