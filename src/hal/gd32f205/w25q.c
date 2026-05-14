#include "hal/w25q.h"
#include "hal/spi3.h"

#define W25Q_CMD_READ_JEDEC      0x9F
#define W25Q_CMD_READ_STATUS_1   0x05
#define W25Q_CMD_WRITE_ENABLE    0x06
#define W25Q_CMD_READ_DATA       0x03
#define W25Q_CMD_SECTOR_ERASE    0x20
#define W25Q_CMD_PAGE_PROGRAM    0x02

#define STATUS_BUSY              0x01

#define ERASE_TIMEOUT_LOOPS    5000000U
#define PROGRAM_TIMEOUT_LOOPS   100000U

static uint32_t s_jedec_id = 0;

static uint8_t read_status1(void)
{
    spi3_cs_assert();
    spi3_xfer(W25Q_CMD_READ_STATUS_1);
    uint8_t st = spi3_xfer(0xFF);
    spi3_cs_deassert();
    return st;
}

static int wait_not_busy(uint32_t max_loops)
{
    while (max_loops--) {
        if (!(read_status1() & STATUS_BUSY)) return 0;
    }
    return -1;
}

static void write_enable(void)
{
    spi3_cs_assert();
    spi3_xfer(W25Q_CMD_WRITE_ENABLE);
    spi3_cs_deassert();
}

int w25q_init(void)
{
    spi3_cs_deassert();

    spi3_cs_assert();
    spi3_xfer(W25Q_CMD_READ_JEDEC);
    uint8_t mfr = spi3_xfer(0xFF);
    uint8_t typ = spi3_xfer(0xFF);
    uint8_t cap = spi3_xfer(0xFF);
    spi3_cs_deassert();

    s_jedec_id = ((uint32_t)mfr << 16) | ((uint32_t)typ << 8) | cap;

    if (s_jedec_id == 0x000000 || s_jedec_id == 0xFFFFFF) {
        s_jedec_id = 0;
        return -1;
    }
    return 0;
}

uint32_t w25q_jedec_id(void)
{
    return s_jedec_id;
}

void w25q_read(uint32_t addr, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    spi3_cs_assert();
    spi3_xfer(W25Q_CMD_READ_DATA);
    spi3_xfer((uint8_t)(addr >> 16));
    spi3_xfer((uint8_t)(addr >>  8));
    spi3_xfer((uint8_t)(addr));
    while (len--) {
        *p++ = spi3_xfer(0xFF);
    }
    spi3_cs_deassert();
}

int w25q_erase_sector(uint32_t addr)
{
    write_enable();

    spi3_cs_assert();
    spi3_xfer(W25Q_CMD_SECTOR_ERASE);
    spi3_xfer((uint8_t)(addr >> 16));
    spi3_xfer((uint8_t)(addr >>  8));
    spi3_xfer((uint8_t)(addr));
    spi3_cs_deassert();

    return wait_not_busy(ERASE_TIMEOUT_LOOPS);
}

int w25q_program(uint32_t addr, const void *buf, size_t len)
{
    if (len == 0 || len > W25Q_PAGE_SIZE) return -1;

    write_enable();

    const uint8_t *p = (const uint8_t *)buf;
    spi3_cs_assert();
    spi3_xfer(W25Q_CMD_PAGE_PROGRAM);
    spi3_xfer((uint8_t)(addr >> 16));
    spi3_xfer((uint8_t)(addr >>  8));
    spi3_xfer((uint8_t)(addr));
    while (len--) {
        spi3_xfer(*p++);
    }
    spi3_cs_deassert();

    return wait_not_busy(PROGRAM_TIMEOUT_LOOPS);
}
