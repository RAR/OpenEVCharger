#ifndef OPENBHZD_HAL_FLASH_H
#define OPENBHZD_HAL_FLASH_H

#include <stddef.h>
#include <stdint.h>

/* RAM-resident GD32F20x bank0 erase + word-program primitives.
 *
 * Executing flash erase / program sequences from the same bank that's
 * being modified faults: the FMC controller holds the bus during the
 * 20-40 ms erase window, so any instruction fetch from that bank stalls
 * or returns garbage. The functions in this module live in the
 * `.ramfunc` linker section so they run entirely out of RAM once
 * flash_copy_ramfunc_to_ram() has executed at boot.
 *
 * Scope: GD32F205VG bank0 only (lower 512 KB). The OpenBHZD image lives
 * in this bank at 0x08000000+ and never crosses into bank1, so we don't
 * need bank1 helpers. Page granularity is 2 KB. */

#define FLASH_BANK0_BASE       0x08000000U
#define FLASH_BANK0_SIZE       0x00080000U   /* 512 KB */
#define FLASH_PAGE_SIZE        0x00000800U   /* 2 KB */

/* Hard cap on the OTA image size we'll attempt to apply. Driven by the
 * RAM buffer the apply path stack-allocates — see flash.c for the
 * arithmetic. Current OpenBHZD image is ~48 KB so 64 KB has plenty of
 * slack; bumping later means re-checking stack headroom. */
#define FLASH_OTA_APPLY_MAX_SIZE  (64U * 1024U)

/* Copy the .ramfunc section from its load address (in flash) to its
 * runtime address (in RAM). Called once early in main(). Idempotent;
 * subsequent calls re-copy without harm. */
void flash_copy_ramfunc_to_ram(void);

/* Read the pending-OTA flag from boot_config; if set and the staged
 * image's CRC32 verifies and image_size <= FLASH_OTA_APPLY_MAX_SIZE,
 * this function:
 *   1. Reads the staged image from W25Q into a stack-allocated RAM buffer
 *      (using flash-resident drivers — flash hasn't been touched yet).
 *   2. Disables interrupts.
 *   3. Jumps into the RAM-resident overwrite routine, which erases bank0
 *      page-by-page and word-programs the new image.
 *   4. Verifies the in-flash bytes match the buffer.
 *   5. Clears the pending-OTA flag and triggers a system reset.
 *
 * Returns: does NOT return on success (system reset). On any error
 * before the dangerous phase, clears the pending flag (since a bad
 * staged image is permanent until rewritten) and returns the failure
 * code so the caller can keep booting the existing image:
 *   -1  no pending flag set
 *   -2  staged image_size out of range / W25Q read failed
 *   -3  CRC mismatch — staged image corrupted between commit and apply
 *   -4  RAM-resident overwrite reported a verify mismatch (should never
 *       happen post-CRC; indicates an FMC fault — we trap into a halt) */
int flash_apply_pending_ota_image(void);

#endif
