#include "ota_stage.h"
#include "boot_config.h"
#include "../hal/w25q.h"

#include <string.h>

/* --- streaming CRC32 ---------------------------------------------------
 *
 * Same polynomial / parameters as crc.c's one-shot crc32(): IEEE 802.3
 * reflected (0xEDB88320), seed 0xFFFFFFFF, final XOR 0xFFFFFFFF. The
 * "state" carried in ota_crc_ctx is the running pre-final-XOR value, so
 * `ota_crc_finalize()` is a single ~complement.
 *
 * No table — the rolling bit-by-bit form keeps code size minimal and is
 * called at chunk granularity (256 B at a time, < 1 % of the OTA wall
 * time at 115200 baud). */
void ota_crc_init(struct ota_crc_ctx *ctx)
{
    ctx->state = 0xFFFFFFFFu;
}

void ota_crc_update(struct ota_crc_ctx *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = ctx->state;
    while (len--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; ++i) {
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
        }
    }
    ctx->state = crc;
}

uint32_t ota_crc_finalize(const struct ota_crc_ctx *ctx)
{
    return ~ctx->state;
}

/* --- stage-region writes ----------------------------------------------- */

int ota_stage_begin(uint32_t image_size)
{
    if (image_size == 0) return -3;
    if (image_size > OTA_STAGE_MAX_IMAGE_SIZE) return -2;

    /* Round up to 4 KB sector granularity. */
    uint32_t end = OTA_STAGE_REGION_BASE + image_size;
    uint32_t end_aligned = (end + (W25Q_SECTOR_SIZE - 1U))
                         & ~(uint32_t)(W25Q_SECTOR_SIZE - 1U);

    for (uint32_t addr = OTA_STAGE_REGION_BASE;
         addr < end_aligned;
         addr += W25Q_SECTOR_SIZE) {
        int rc = w25q_erase_sector(addr);
        if (rc < 0) return -1;
    }
    return 0;
}

int ota_stage_write(uint32_t offset, const void *data, size_t len)
{
    if (data == NULL || len == 0) return -4;
    if (offset & (W25Q_PAGE_SIZE - 1U)) return -2;
    if ((uint64_t)offset + (uint64_t)len > (uint64_t)OTA_STAGE_REGION_SIZE) {
        return -3;
    }

    const uint8_t *src = (const uint8_t *)data;
    uint32_t flash_addr = OTA_STAGE_REGION_BASE + offset;
    while (len > 0) {
        size_t this_page = (len < W25Q_PAGE_SIZE) ? len : W25Q_PAGE_SIZE;
        int rc = w25q_program(flash_addr, src, this_page);
        if (rc < 0) return -1;
        flash_addr += this_page;
        src        += this_page;
        len        -= this_page;
    }
    return 0;
}

uint32_t ota_stage_compute_crc(uint32_t image_size)
{
    struct ota_crc_ctx ctx;
    ota_crc_init(&ctx);

    uint8_t buf[256];
    uint32_t addr = OTA_STAGE_REGION_BASE;
    uint32_t remaining = image_size;
    while (remaining > 0) {
        size_t this_chunk = (remaining < sizeof buf) ? remaining : sizeof buf;
        w25q_read(addr, buf, this_chunk);
        ota_crc_update(&ctx, buf, this_chunk);
        addr      += this_chunk;
        remaining -= this_chunk;
    }
    return ota_crc_finalize(&ctx);
}

int ota_stage_verify(uint32_t image_size, uint32_t expected_crc32)
{
    return (ota_stage_compute_crc(image_size) == expected_crc32) ? 0 : -1;
}

int ota_stage_mark_pending(uint32_t image_size, uint32_t image_crc32)
{
    return boot_config_set_pending_ota(1, image_size, image_crc32);
}

int ota_stage_clear_pending(void)
{
    return boot_config_set_pending_ota(0, 0, 0);
}
