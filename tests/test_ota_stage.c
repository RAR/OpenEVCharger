#include "test_harness.h"
#include "mocks/w25q_mock.h"
#include "ota_stage.h"
#include "boot_config.h"
#include "crc.h"
#include "../src/hal/w25q.h"

#include <string.h>

static void fill_pattern(uint8_t *buf, size_t len, uint8_t seed)
{
    for (size_t i = 0; i < len; ++i) {
        buf[i] = (uint8_t)(seed + (i * 7u));
    }
}

void suite_ota_stage(void);
void suite_ota_stage(void)
{
    TEST_CASE("ota_crc streaming matches one-shot crc32 over concat");
    {
        uint8_t buf[1024];
        fill_pattern(buf, sizeof buf, 0x37);
        uint32_t one_shot = crc32(buf, sizeof buf);

        struct ota_crc_ctx ctx;
        ota_crc_init(&ctx);
        /* Split unevenly: 1 / 255 / 256 / 256 / 256 — covers byte and
         * page-sized updates. Total 1024. */
        ota_crc_update(&ctx, buf,         1);
        ota_crc_update(&ctx, buf + 1,     255);
        ota_crc_update(&ctx, buf + 256,   256);
        ota_crc_update(&ctx, buf + 512,   256);
        ota_crc_update(&ctx, buf + 768,   256);
        uint32_t streaming = ota_crc_finalize(&ctx);
        CHECK_EQ_U32(streaming, one_shot);
    }

    TEST_CASE("ota_crc empty stream finalizes to 0");
    {
        struct ota_crc_ctx ctx;
        ota_crc_init(&ctx);
        /* CRC32 of empty input is 0 by IEEE convention. */
        CHECK_EQ_U32(ota_crc_finalize(&ctx), 0u);
    }

    TEST_CASE("ota_stage_begin rejects invalid sizes");
    {
        w25q_mock_reset(); w25q_init();
        int rc = ota_stage_begin(0);
        CHECK_EQ_INT(rc, -3);
        rc = ota_stage_begin(OTA_STAGE_MAX_IMAGE_SIZE + 1U);
        CHECK_EQ_INT(rc, -2);
    }

    TEST_CASE("ota_stage_begin erases enough sectors to cover image_size");
    {
        w25q_mock_reset(); w25q_init();
        /* Pre-pollute the first stage sector to verify erase happens. */
        uint8_t junk[16];
        memset(junk, 0xA5, sizeof junk);
        /* AND-into a blank page — sets the bytes since blank is 0xFF. */
        w25q_program(OTA_STAGE_REGION_BASE, junk, sizeof junk);
        CHECK(!w25q_mock_sector_is_blank(OTA_STAGE_REGION_BASE));

        int rc = ota_stage_begin(5000);   /* spans 2 sectors (4096 + tail) */
        CHECK_EQ_INT(rc, 0);
        CHECK(w25q_mock_sector_is_blank(OTA_STAGE_REGION_BASE));
        CHECK(w25q_mock_sector_is_blank(OTA_STAGE_REGION_BASE +
                                        W25Q_SECTOR_SIZE));
        /* Third sector should be untouched (still blank from reset). */
        CHECK(w25q_mock_sector_is_blank(OTA_STAGE_REGION_BASE +
                                        2U * W25Q_SECTOR_SIZE));
    }

    TEST_CASE("ota_stage_write splits arbitrary offset across page boundary");
    {
        w25q_mock_reset(); w25q_init();
        ota_stage_begin(2048);
        /* Mirrors the FC41D's 48-byte chunk cadence: offsets 0, 48, 96,
         * ... none page-aligned past the first. The helper must accept
         * each one and split where needed. Pattern is per-byte
         * deterministic so we can verify the assembled image. */
        uint8_t scratch[OTA_STAGE_REGION_BASE > 0 ? 600 : 1];   /* 600 B */
        for (size_t i = 0; i < 600; ++i) scratch[i] = (uint8_t)(i & 0xFF);
        size_t off = 0;
        while (off < 600) {
            size_t take = (600 - off < 48) ? (600 - off) : 48;
            int rc = ota_stage_write((uint32_t)off, scratch + off, take);
            CHECK_EQ_INT(rc, 0);
            off += take;
        }
        uint8_t back[600];
        w25q_read(OTA_STAGE_REGION_BASE, back, sizeof back);
        CHECK_EQ_INT(memcmp(back, scratch, 600), 0);
    }

    TEST_CASE("ota_stage_write rejects past-end writes");
    {
        w25q_mock_reset(); w25q_init();
        ota_stage_begin(W25Q_PAGE_SIZE);
        uint8_t blob[256];
        fill_pattern(blob, sizeof blob, 0x22);
        int rc = ota_stage_write(OTA_STAGE_REGION_SIZE, blob, sizeof blob);
        CHECK_EQ_INT(rc, -3);
    }

    TEST_CASE("ota_stage_write rejects null buffer / zero length");
    {
        w25q_mock_reset(); w25q_init();
        ota_stage_begin(W25Q_PAGE_SIZE);
        uint8_t blob[1] = { 0xCC };
        int rc = ota_stage_write(0, NULL, 1);
        CHECK_EQ_INT(rc, -4);
        rc = ota_stage_write(0, blob, 0);
        CHECK_EQ_INT(rc, -4);
    }

    TEST_CASE("ota_stage_write splits large chunks across pages");
    {
        w25q_mock_reset(); w25q_init();
        ota_stage_begin(2048);
        uint8_t big[1024];
        fill_pattern(big, sizeof big, 0x40);
        int rc = ota_stage_write(0, big, sizeof big);
        CHECK_EQ_INT(rc, 0);
        /* Read each 256 B page back and verify content matches. */
        uint8_t back[1024];
        w25q_read(OTA_STAGE_REGION_BASE, back, sizeof back);
        CHECK_EQ_INT(memcmp(back, big, sizeof big), 0);
    }

    TEST_CASE("ota_stage_compute_crc matches one-shot over the staged bytes");
    {
        w25q_mock_reset(); w25q_init();
        const uint32_t image_size = 700;   /* not a multiple of 256 */
        ota_stage_begin(image_size);
        uint8_t img[700];
        fill_pattern(img, sizeof img, 0x9E);
        /* Stage write needs page-aligned offsets, so write the whole
         * thing as one call (helper splits internally). The trailing
         * bytes within the last page beyond image_size stay 0xFF (erase
         * default) and must NOT be folded into the CRC — only the
         * caller-declared image_size bytes are. */
        int rc = ota_stage_write(0, img, sizeof img);
        CHECK_EQ_INT(rc, 0);

        uint32_t expected = crc32(img, sizeof img);
        uint32_t got      = ota_stage_compute_crc(image_size);
        CHECK_EQ_U32(got, expected);
    }

    TEST_CASE("ota_stage_verify pass + mismatch");
    {
        w25q_mock_reset(); w25q_init();
        const uint32_t image_size = 256;
        ota_stage_begin(image_size);
        uint8_t img[256];
        fill_pattern(img, sizeof img, 0x10);
        ota_stage_write(0, img, sizeof img);

        uint32_t good = crc32(img, sizeof img);
        CHECK_EQ_INT(ota_stage_verify(image_size, good),       0);
        CHECK_EQ_INT(ota_stage_verify(image_size, good ^ 1u), -1);
    }

    TEST_CASE("mark_pending then clear_pending round-trip via boot_config");
    {
        w25q_mock_reset(); w25q_init();
        boot_config_load();
        CHECK_EQ_INT(boot_config_pending_ota_flag(), 0);
        CHECK_EQ_U32(boot_config_staged_image_size(),  0u);
        CHECK_EQ_U32(boot_config_staged_image_crc32(), 0u);

        int rc = ota_stage_mark_pending(45000u, 0xDEADBEEFu);
        CHECK_EQ_INT(rc, 0);
        CHECK_EQ_INT(boot_config_pending_ota_flag(), 1);
        CHECK_EQ_U32(boot_config_staged_image_size(),  45000u);
        CHECK_EQ_U32(boot_config_staged_image_crc32(), 0xDEADBEEFu);

        /* Reload from flash to confirm persistence (not just the cache). */
        rc = boot_config_load();
        CHECK_EQ_INT(rc, 0);
        CHECK_EQ_INT(boot_config_pending_ota_flag(), 1);
        CHECK_EQ_U32(boot_config_staged_image_size(),  45000u);
        CHECK_EQ_U32(boot_config_staged_image_crc32(), 0xDEADBEEFu);

        rc = ota_stage_clear_pending();
        CHECK_EQ_INT(rc, 0);
        CHECK_EQ_INT(boot_config_pending_ota_flag(), 0);
        CHECK_EQ_U32(boot_config_staged_image_size(),  0u);
        CHECK_EQ_U32(boot_config_staged_image_crc32(), 0u);

        rc = boot_config_load();
        CHECK_EQ_INT(rc, 0);
        CHECK_EQ_INT(boot_config_pending_ota_flag(), 0);
    }

    TEST_CASE("set_pending_ota: idempotent on unchanged values");
    {
        w25q_mock_reset(); w25q_init();
        boot_config_load();
        boot_config_set_pending_ota(1, 12345u, 0xCAFEF00Du);
        /* Now the live slot is whichever pingpong picked. Snapshot. */
        uint8_t pre_a = w25q_mock_peek(BOOT_CONFIG_SLOT_A + 32U - 4U);
        uint8_t pre_b = w25q_mock_peek(BOOT_CONFIG_SLOT_B + 32U - 4U);

        /* Re-call with identical values: no slot churn. */
        int rc = boot_config_set_pending_ota(1, 12345u, 0xCAFEF00Du);
        CHECK_EQ_INT(rc, 0);
        CHECK_EQ_INT(w25q_mock_peek(BOOT_CONFIG_SLOT_A + 32U - 4U), pre_a);
        CHECK_EQ_INT(w25q_mock_peek(BOOT_CONFIG_SLOT_B + 32U - 4U), pre_b);
    }

    TEST_CASE("set_pending_ota(0, ...) zeroes size + crc regardless of args");
    {
        w25q_mock_reset(); w25q_init();
        boot_config_load();
        boot_config_set_pending_ota(1, 9999u, 0xAAAA5555u);
        int rc = boot_config_set_pending_ota(0, 9999u, 0xAAAA5555u);
        CHECK_EQ_INT(rc, 0);
        CHECK_EQ_INT(boot_config_pending_ota_flag(), 0);
        CHECK_EQ_U32(boot_config_staged_image_size(),  0u);
        CHECK_EQ_U32(boot_config_staged_image_crc32(), 0u);
    }

    TEST_CASE("write spans full first sector; CRC over exactly that span");
    {
        w25q_mock_reset(); w25q_init();
        const uint32_t image_size = 4096;
        ota_stage_begin(image_size);
        uint8_t img[4096];
        fill_pattern(img, sizeof img, 0x55);
        int rc = ota_stage_write(0, img, sizeof img);
        CHECK_EQ_INT(rc, 0);
        uint32_t expected = crc32(img, sizeof img);
        CHECK_EQ_U32(ota_stage_compute_crc(image_size), expected);
    }

    TEST_CASE("multiple non-contiguous writes assemble correctly");
    {
        w25q_mock_reset(); w25q_init();
        ota_stage_begin(1024);
        uint8_t a[256], b[256], c[256], d[256];
        fill_pattern(a, sizeof a, 0x01);
        fill_pattern(b, sizeof b, 0x02);
        fill_pattern(c, sizeof c, 0x03);
        fill_pattern(d, sizeof d, 0x04);
        /* Write out of order — page-aligned offsets only. */
        ota_stage_write(512, c, sizeof c);
        ota_stage_write(0,   a, sizeof a);
        ota_stage_write(768, d, sizeof d);
        ota_stage_write(256, b, sizeof b);

        uint8_t back[1024];
        w25q_read(OTA_STAGE_REGION_BASE, back, sizeof back);
        CHECK_EQ_INT(memcmp(back,        a, 256), 0);
        CHECK_EQ_INT(memcmp(back + 256,  b, 256), 0);
        CHECK_EQ_INT(memcmp(back + 512,  c, 256), 0);
        CHECK_EQ_INT(memcmp(back + 768,  d, 256), 0);
    }
}
