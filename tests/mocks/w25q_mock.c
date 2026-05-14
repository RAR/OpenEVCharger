#include "w25q_mock.h"
#include "../../src/drivers/w25q.h"

#include <string.h>

static uint8_t s_flash[W25Q_MOCK_FLASH_SIZE];
static int     s_inited = 0;

void w25q_mock_reset(void)
{
    memset(s_flash, 0xFF, sizeof s_flash);
    s_inited = 0;
}

void w25q_mock_poke(uint32_t addr, uint8_t value, int nor_semantics)
{
    if (addr >= W25Q_MOCK_FLASH_SIZE) return;
    if (nor_semantics) {
        s_flash[addr] &= value;
    } else {
        s_flash[addr] = value;
    }
}

uint8_t w25q_mock_peek(uint32_t addr)
{
    if (addr >= W25Q_MOCK_FLASH_SIZE) return 0xFF;
    return s_flash[addr];
}

int w25q_mock_sector_is_blank(uint32_t addr)
{
    uint32_t base = addr & ~(uint32_t)(W25Q_SECTOR_SIZE - 1U);
    if (base + W25Q_SECTOR_SIZE > W25Q_MOCK_FLASH_SIZE) return 0;
    for (uint32_t i = 0; i < W25Q_SECTOR_SIZE; ++i) {
        if (s_flash[base + i] != 0xFF) return 0;
    }
    return 1;
}

/* --- hal/w25q.h implementation ----------------------------------------- */

int w25q_init(void)
{
    s_inited = 1;
    return 0;
}

uint32_t w25q_jedec_id(void)
{
    return s_inited ? 0x00EF4017u : 0u;
}

void w25q_read(uint32_t addr, void *buf, size_t len)
{
    if (addr >= W25Q_MOCK_FLASH_SIZE) {
        memset(buf, 0xFF, len);
        return;
    }
    size_t avail = W25Q_MOCK_FLASH_SIZE - addr;
    size_t n = (len < avail) ? len : avail;
    memcpy(buf, &s_flash[addr], n);
    if (n < len) memset((uint8_t *)buf + n, 0xFF, len - n);
}

int w25q_erase_sector(uint32_t addr)
{
    if (addr >= W25Q_MOCK_FLASH_SIZE) return -1;
    uint32_t base = addr & ~(uint32_t)(W25Q_SECTOR_SIZE - 1U);
    memset(&s_flash[base], 0xFF, W25Q_SECTOR_SIZE);
    return 0;
}

int w25q_program(uint32_t addr, const void *buf, size_t len)
{
    if (len == 0) return 0;
    if (len > W25Q_PAGE_SIZE) return -1;
    /* No page-crossing: addr+len must stay within the same 256 B page. */
    uint32_t page_base = addr & ~(uint32_t)(W25Q_PAGE_SIZE - 1U);
    if (addr + len > page_base + W25Q_PAGE_SIZE) return -1;
    if (addr + len > W25Q_MOCK_FLASH_SIZE) return -1;

    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < len; ++i) {
        /* NOR programs are bit-clear-only: existing & incoming. */
        s_flash[addr + i] &= src[i];
    }
    return 0;
}
