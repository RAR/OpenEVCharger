#include "bl0939.h"
#include "../core/pin_map.h"
#include "../hal/uart.h"
#include "gd32f20x.h"

/* Per-bit delay loop. At our SystemCoreClock = 120 MHz, ~10 NOPs ≈
 * 80 ns. We loop a few times to bring SCLK closer to 100 kHz — well
 * below the BL0939's 900 kHz max so we have plenty of margin while
 * we're still validating the wiring. Tune up later. */
static inline void bb_delay(void)
{
    for (volatile int i = 0; i < 600; ++i) { __asm__ volatile (""); }
}

void bl0939_init(void)
{
    /* SCLK idle LOW (SPI mode 1 starts low). */
    gpio_bit_reset(PIN_BL0939_SCLK_PORT, PIN_BL0939_SCLK_PIN);
    gpio_init(PIN_BL0939_SCLK_PORT, GPIO_MODE_OUT_PP,
              GPIO_OSPEED_50MHZ, PIN_BL0939_SCLK_PIN);

    /* SDI idle HIGH (datasheet shows quiescent line high). */
    gpio_bit_set(PIN_BL0939_SDI_PORT, PIN_BL0939_SDI_PIN);
    gpio_init(PIN_BL0939_SDI_PORT, GPIO_MODE_OUT_PP,
              GPIO_OSPEED_50MHZ, PIN_BL0939_SDI_PIN);

    /* SDO is BL0939 output (open-drain per datasheet). Internal
     * pull-up keeps the line HIGH when the chip isn't driving. */
    gpio_bit_set(PIN_BL0939_SDO_PORT, PIN_BL0939_SDO_PIN);
    gpio_init(PIN_BL0939_SDO_PORT, GPIO_MODE_IPU,
              GPIO_OSPEED_50MHZ, PIN_BL0939_SDO_PIN);
}

uint8_t bl0939_xfer_byte(uint8_t out)
{
    uint8_t in = 0;

    /* SPI mode 1: CPOL=0, CPHA=1 — clock idles low, data sampled
     * on falling edge, output changes on rising edge. MSB first.
     *
     * Sequence per bit:
     *   1. set SDI to next out bit
     *   2. SCLK rising edge — shift out (BL0939 sees data)
     *   3. half-bit delay
     *   4. SCLK falling edge — sample SDO into 'in'
     *   5. half-bit delay
     */
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
    /* Restore SDI HIGH for inter-frame idle. */
    gpio_bit_set(PIN_BL0939_SDI_PORT, PIN_BL0939_SDI_PIN);
    return in;
}

void bl0939_smoke_test(void)
{
    bl0939_init();

    /* Send a slow walking-1 pattern + read SDO. With nothing on the
     * wire we'd expect 0xFF (pulled up by the input PUPD). With the
     * BL0939 alive and possibly auto-streaming a packet, we should
     * see some non-rail value(s). The exact response without a valid
     * frame header is undefined — this is purely a "is there life
     * on the wires" test. Scope SCLK/SDI/SDO simultaneously to
     * confirm the bit-bang loop runs. */
    static const uint8_t out[] = { 0x55, 0x00, 0xFF, 0xAA };
    printk("BL0939 smoke: sending %zu bytes...\n", sizeof(out));
    for (size_t i = 0; i < sizeof(out); ++i) {
        uint8_t r = bl0939_xfer_byte(out[i]);
        printk("  out=0x%02x in=0x%02x\n", out[i], r);
    }
    printk("BL0939 smoke: done\n");
}
