#ifndef OPENBHZD_PERSIST_OTA_STAGE_H
#define OPENBHZD_PERSIST_OTA_STAGE_H

#include <stddef.h>
#include <stdint.h>

/* OTA staging region in W25Q (8 MB GD25Q64).
 *
 * Layout — leaves the lower half untouched (boot_config, calibration,
 * event_log, session_log, rfid_authlist, crash_state all sit in the
 * first ~64 KB):
 *
 *   0x000000 .. 0x3FFFFF   in-use persistence records (~4 MB headroom)
 *   0x400000 .. 0x4FFFFF   OTA stage region (1 MB cap, page-aligned writes)
 *   0x500000 .. 0x7FFFFF   reserved (future: previous-image rollback copy)
 *
 * Internal flash on a GD32F205VG is 1 MB but the v1 image is < 64 KB and
 * the spec does not anticipate exceeding 512 KB, so a 1 MB stage region
 * has plenty of slack. */
#define OTA_STAGE_REGION_BASE     0x400000U
#define OTA_STAGE_REGION_SIZE     0x100000U   /* 1 MB */
#define OTA_STAGE_MAX_IMAGE_SIZE  OTA_STAGE_REGION_SIZE

/* Streaming CRC32 context (IEEE 802.3, same parameters as crc.h's
 * one-shot crc32()). Initial state matches crc32()'s pre-XOR seed. */
struct ota_crc_ctx {
    uint32_t state;
};

void     ota_crc_init(struct ota_crc_ctx *ctx);
void     ota_crc_update(struct ota_crc_ctx *ctx,
                        const void *data, size_t len);
uint32_t ota_crc_finalize(const struct ota_crc_ctx *ctx);

/* Erase enough 4 KB sectors at the head of the stage region to hold an
 * image of `image_size` bytes. Idempotent — re-running on the same
 * region is fine, the erase is a no-op on already-blank sectors.
 *   0   → success
 *  -1   → W25Q erase error (propagated from w25q_erase_sector)
 *  -2   → image_size > OTA_STAGE_MAX_IMAGE_SIZE
 *  -3   → image_size == 0 */
int ota_stage_begin(uint32_t image_size);

/* Write `len` bytes at `offset` (relative to the stage region base).
 * `offset` must be page-aligned (256 B). `len` is split internally into
 * 256 B page programs as needed. The destination sectors must already
 * have been erased by ota_stage_begin() — this helper does NOT erase
 * before write.
 *   0   → success
 *  -1   → W25Q program error
 *  -2   → offset not page-aligned
 *  -3   → write extends past OTA_STAGE_REGION_SIZE
 *  -4   → null buffer / zero len */
int ota_stage_write(uint32_t offset, const void *data, size_t len);

/* Read back the staged image (size bytes from base) and compute its
 * CRC32. */
uint32_t ota_stage_compute_crc(uint32_t image_size);

/* Returns 0 if the staged image's CRC32 over `image_size` bytes equals
 * `expected_crc32`, otherwise -1. */
int ota_stage_verify(uint32_t image_size, uint32_t expected_crc32);

/* Mark a staged image as ready for activation on next boot. Wraps
 * boot_config_set_pending_ota(1, image_size, image_crc32). Returns 0
 * on success, <0 on persist error. */
int ota_stage_mark_pending(uint32_t image_size, uint32_t image_crc32);

/* Clear the pending-OTA flag. Used after a successful stage→flash copy
 * (in main()) and to abort a staged image. Wraps
 * boot_config_set_pending_ota(0, 0, 0). */
int ota_stage_clear_pending(void);

#endif
