#include "spi3.h"
#include "pin_map.h"
#include "gd32f20x.h"

void spi3_init(void)
{
    rcu_periph_clock_enable(RCU_SPI2);

    spi_parameter_struct cfg;
    spi_struct_para_init(&cfg);
    cfg.trans_mode           = SPI_TRANSMODE_FULLDUPLEX;
    cfg.device_mode          = SPI_MASTER;
    cfg.frame_size           = SPI_FRAMESIZE_8BIT;
    cfg.clock_polarity_phase = SPI_CK_PL_LOW_PH_1EDGE;   /* mode 0 */
    cfg.nss                  = SPI_NSS_SOFT;
    cfg.prescale             = SPI_PSC_2;                /* APB1/2 = 15 MHz */
    cfg.endian               = SPI_ENDIAN_MSB;
    spi_init(SPI2, &cfg);

    spi_nss_internal_high(SPI2);
    spi_enable(SPI2);
}

uint8_t spi3_xfer(uint8_t tx)
{
    while (RESET == spi_i2s_flag_get(SPI2, SPI_FLAG_TBE)) { }
    spi_i2s_data_transmit(SPI2, (uint16_t)tx);
    while (RESET == spi_i2s_flag_get(SPI2, SPI_FLAG_RBNE)) { }
    return (uint8_t)spi_i2s_data_receive(SPI2);
}

void spi3_cs_assert(void)
{
    gpio_bit_reset(PIN_W25Q_CS_PORT, PIN_W25Q_CS_PIN);
}

void spi3_cs_deassert(void)
{
    gpio_bit_set(PIN_W25Q_CS_PORT, PIN_W25Q_CS_PIN);
}
