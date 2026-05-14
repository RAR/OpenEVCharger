/* boards/nexcyber/hal/spi2.c — M3 hardware SPI2 transport for BL0939.
 *
 * Direct port of rippleon's src/hal/spi3.c (which uses SPL "SPI2",
 * i.e. the same SPI2 peripheral) — the surface and config are the
 * same, only the SPL spelling differs (GD32 → Nations) and the SPI
 * mode here is mode 1 instead of mode 0 (BL0939 datasheet requires
 * shift on rising / sample on falling).
 *
 * NSS is software-managed: PB12 was configured as OUT_PP in M2 GPIO
 * HAL with safe-low init, then driven HIGH by the bus owner here to
 * idle the chip-select.
 *
 * Single-owner rule (same as rippleon): only one task at a time
 * drives the BL0939 transactions. No locking inside this file.
 */

#include "spi2.h"
#include "pin_map.h"
#include "n32g45x.h"

/* PIN_BL0939_NSS_PORT is uint32_t-typed (see pin_map.h Task 12 note);
 * the Nations SPL GPIO_Set/ResetBits() want a GPIO_Module *. */
#define BL0939_NSS_GPIO  ((GPIO_Module *)PIN_BL0939_NSS_PORT)

void spi2_init(void)
{
    /* SPI2 lives on APB1; the GPIOB pads are on APB2. Both are
     * already on by the time gpio_init_all() / uart_init() have
     * run — enabling here keeps the function idempotent. */
    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_SPI2, ENABLE);
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOB, ENABLE);

    SPI_InitType cfg;
    SPI_InitStruct(&cfg);
    cfg.DataDirection = SPI_DIR_DOUBLELINE_FULLDUPLEX;
    cfg.SpiMode       = SPI_MODE_MASTER;
    cfg.DataLen       = SPI_DATA_SIZE_8BITS;
    cfg.CLKPOL        = SPI_CLKPOL_LOW;          /* idle LOW (mode 1) */
    cfg.CLKPHA        = SPI_CLKPHA_SECOND_EDGE;  /* sample on falling (mode 1) */
    cfg.NSS           = SPI_NSS_SOFT;
    cfg.BaudRatePres  = SPI_BR_PRESCALER_64;     /* APB1 / 64 ≈ 562 kHz */
    cfg.FirstBit      = SPI_FB_MSB;
    cfg.CRCPoly       = 7;                       /* unused; default */
    SPI_Init(SPI2, &cfg);

    /* Internal NSS soft-high so SPI doesn't auto-MODF when we
     * leave NSS unmanaged — only matters when NSS is software. */
    SPI_SetNssLevel(SPI2, SPI_NSS_HIGH);
    SPI_Enable(SPI2, ENABLE);

    /* NSS line (PB12) idle HIGH = chip de-asserted. M2 gpio_init_all()
     * already drives it LOW as a "safe state"; immediately raise it
     * here once the SPI side is ready to talk. */
    GPIO_SetBits(BL0939_NSS_GPIO, PIN_BL0939_NSS_PIN);
}

uint8_t spi2_xfer(uint8_t tx)
{
    while (SPI_I2S_GetStatus(SPI2, SPI_I2S_TE_FLAG) == RESET) { }
    SPI_I2S_TransmitData(SPI2, (uint16_t)tx);
    while (SPI_I2S_GetStatus(SPI2, SPI_I2S_RNE_FLAG) == RESET) { }
    return (uint8_t)SPI_I2S_ReceiveData(SPI2);
}

void spi2_cs_assert(void)
{
    GPIO_ResetBits(BL0939_NSS_GPIO, PIN_BL0939_NSS_PIN);
}

void spi2_cs_deassert(void)
{
    GPIO_SetBits(BL0939_NSS_GPIO, PIN_BL0939_NSS_PIN);
}
