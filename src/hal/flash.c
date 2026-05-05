#include "flash.h"
#include "../persist/boot_config.h"
#include "../persist/ota_stage.h"
#include "../persist/crc.h"
#include "../hal/uart.h"
#include "../hal/w25q.h"
#include "gd32f20x.h"

#include <string.h>

/* --- linker symbols (defined in gd32f205vc.ld) ------------------------- */
extern uint32_t _siramfunc;   /* load address in flash */
extern uint32_t _sramfunc;    /* runtime address in RAM */
extern uint32_t _eramfunc;    /* runtime end in RAM */

void flash_copy_ramfunc_to_ram(void)
{
    /* Mirrors the .data copy that startup_gd32f20x_cl.S does. Word-by-word
     * copy is fine — the section is 4-byte aligned by the linker. */
    uint32_t *src = &_siramfunc;
    uint32_t *dst = &_sramfunc;
    while (dst < &_eramfunc) {
        *dst++ = *src++;
    }
}

/* --- RAM-resident FMC primitives --------------------------------------- *
 *
 * Everything between this line and the closing "<<< RAM-RESIDENT END >>>"
 * comment runs from RAM. Constraints:
 *   - No calls to flash-resident functions (no printk, no SPL, no memcpy
 *     unless the toolchain inlines it).
 *   - No reads from flash (rodata strings, etc).
 *   - Inline-only constants and direct register pokes.
 *
 * The vendor SPL's bank-aware fmc_page_erase / fmc_word_program reach
 * FMC_BANK0_SIZE and FMC_SIZE in flash, which is exactly what we can't
 * touch — so we re-implement the bank0-only path inline. The OpenBHZD
 * image lives entirely in bank0 (lower 512 KB of a 1 MB GD32F205VG). */

#define FLASH_FMC_KEY1   0x45670123U
#define FLASH_FMC_KEY2   0xCDEF89ABU

#define RAMFUNC __attribute__((section(".ramfunc"), noinline))

/* Spin until BUSY clears or a hard timeout. We don't have a timer in
 * RAM-resident scope — just a deterministic loop count. Sized for
 * ~300 ms at 120 MHz: page erase is typically 30 ms (datasheet
 * max ~60 ms), word program is < 100 µs. The full overwrite is
 * called per-op ~10 000+ times for a 48 KB image, so an oversized
 * timeout dominates real wall-clock if anything actually stalls. */
static RAMFUNC int flash_wait_idle_ram(void)
{
    volatile uint32_t timeout = 5000000U;
    while ((FMC_STAT0 & FMC_STAT0_BUSY) && timeout) {
        timeout--;
    }
    if (FMC_STAT0 & FMC_STAT0_BUSY) return -1;
    if (FMC_STAT0 & (FMC_STAT0_PGERR | FMC_STAT0_WPERR)) return -2;
    return 0;
}

static RAMFUNC void flash_clear_status_ram(void)
{
    /* These bits are W1C — write-1-to-clear. */
    FMC_STAT0 = FMC_STAT0_PGERR | FMC_STAT0_WPERR | FMC_STAT0_ENDF;
}

static RAMFUNC int flash_unlock_ram(void)
{
    /* Already unlocked? LK=0 means unlocked. */
    if ((FMC_CTL0 & FMC_CTL0_LK) == 0u) return 0;
    FMC_KEY0 = FLASH_FMC_KEY1;
    FMC_KEY0 = FLASH_FMC_KEY2;
    return ((FMC_CTL0 & FMC_CTL0_LK) == 0u) ? 0 : -1;
}

static RAMFUNC void flash_lock_ram(void)
{
    FMC_CTL0 |= FMC_CTL0_LK;
}

/* Inner helpers below assume FMC_STAT0_BUSY is already clear on entry —
 * the orchestrator does an explicit wait_idle once at the top before
 * the first op, and each helper's trailing wait keeps the invariant.
 * Skipping the redundant pre-op wait is a 2× win on word programming
 * (12 KB image = ~3000 word programs; 48 KB = ~12 000). */
static RAMFUNC int flash_erase_page_ram(uint32_t page_addr)
{
    flash_clear_status_ram();
    FMC_CTL0 |= FMC_CTL0_PER;
    FMC_ADDR0 = page_addr;
    FMC_CTL0 |= FMC_CTL0_START;
    int rc = flash_wait_idle_ram();
    FMC_CTL0 &= ~FMC_CTL0_PER;
    return rc;
}

static RAMFUNC int flash_program_word_ram(uint32_t addr, uint32_t data)
{
    flash_clear_status_ram();
    FMC_CTL0 |= FMC_CTL0_PG;
    *(volatile uint32_t *)addr = data;
    int rc = flash_wait_idle_ram();
    FMC_CTL0 &= ~FMC_CTL0_PG;
    /* Verify the write. The flash bus is idle now so a read returns the
     * just-programmed word. */
    if (rc == 0 && *(volatile uint32_t *)addr != data) rc = -3;
    return rc;
}

/* The dangerous routine. Erases pages 0..ceil(size / 2048) of bank0 and
 * programs them word-by-word from `src`. Returns 0 on success, <0 on
 * any FMC error. Caller has already disabled IRQs and verified that
 * `src` is RAM-resident.
 *
 * "src" is sized to a 4-byte aligned multiple — the apply orchestrator
 * pads the staged image up to a 4 B boundary before the RAM read so we
 * can program in pure 32-bit chunks here. */
static RAMFUNC int flash_overwrite_bank0_from_ram(const uint32_t *src,
                                                  uint32_t size_bytes)
{
    if (size_bytes == 0u || size_bytes > FLASH_BANK0_SIZE) return -10;
    if (size_bytes & 0x3u) return -11;

    int rc = flash_unlock_ram();
    if (rc != 0) return rc;
    /* One-shot pre-flight wait so each subsequent helper can skip its
     * leading wait_idle (entry-precondition: BUSY = 0). */
    rc = flash_wait_idle_ram();
    if (rc != 0) goto out;

    /* Erase covering the full image, page-aligned upward. */
    uint32_t end = FLASH_BANK0_BASE + size_bytes;
    uint32_t end_aligned = (end + (FLASH_PAGE_SIZE - 1U))
                         & ~(uint32_t)(FLASH_PAGE_SIZE - 1U);
    for (uint32_t page = FLASH_BANK0_BASE; page < end_aligned;
         page += FLASH_PAGE_SIZE) {
        rc = flash_erase_page_ram(page);
        if (rc != 0) goto out;
    }

    /* Word-program. */
    uint32_t words = size_bytes >> 2;
    uint32_t addr  = FLASH_BANK0_BASE;
    for (uint32_t i = 0; i < words; ++i) {
        rc = flash_program_word_ram(addr, src[i]);
        if (rc != 0) goto out;
        addr += 4U;
    }

out:
    flash_lock_ram();
    return rc;
}

/* <<< RAM-RESIDENT END >>> */

/* --- flash-resident orchestrator --------------------------------------- */

/* Drains any UART output before we cut the cord on flash. Best-effort —
 * if the printk ring is bigger than what we can flush in this loop, the
 * rest is lost; that's fine because we'll print again on the next boot. */
static void brief_uart_drain(void)
{
    for (volatile int i = 0; i < 600000; ++i) { __asm__ volatile (""); }
}

int flash_apply_pending_ota_image(void)
{
    if (!boot_config_pending_ota_flag()) return -1;

    uint32_t img_size = boot_config_staged_image_size();
    uint32_t img_crc  = boot_config_staged_image_crc32();

    if (img_size == 0u || img_size > FLASH_OTA_APPLY_MAX_SIZE) {
        printk("flash-ota: pending size %u out of range — clearing flag\n",
               (unsigned)img_size);
        (void)ota_stage_clear_pending();
        return -2;
    }

    /* If the running image already matches the staged CRC, the previous
     * apply succeeded and the only thing pending is the flag-clear.
     * Skip the re-overwrite; just clear and continue boot. */
    uint32_t running_crc = crc32((const void *)FLASH_BANK0_BASE, img_size);
    if (running_crc == img_crc) {
        printk("flash-ota: running image already matches staged CRC=0x%08x — "
               "clearing pending flag\n", (unsigned)img_crc);
        (void)ota_stage_clear_pending();
        return 0;
    }

    /* Re-verify the staged CRC32 from W25Q — independent of the
     * COMMIT-time check, catches W25Q bit-rot or wrong boot_config
     * record. Reads via the still-intact flash-resident SPI3/W25Q
     * drivers. */
    uint32_t staged_crc = ota_stage_compute_crc(img_size);
    if (staged_crc != img_crc) {
        printk("flash-ota: staged CRC drift (W25Q=0x%08x boot_cfg=0x%08x) — "
               "clearing pending flag, keeping running image\n",
               (unsigned)staged_crc, (unsigned)img_crc);
        (void)ota_stage_clear_pending();
        return -3;
    }

    /* Stack-allocated buffer: the apply path is only reached pre-FreeRTOS
     * with the main stack to itself, so a 64 KB local is safe. The 4 B
     * padding ensures word-aligned size for the RAM-resident programmer. */
    uint8_t buf[FLASH_OTA_APPLY_MAX_SIZE + 4U] __attribute__((aligned(4)));
    uint32_t padded_size = (img_size + 3U) & ~3U;
    /* Pad-bytes start as 0xFF so a partial last word doesn't get "0"
     * stamped into a region that fmc_word_program would refuse anyway. */
    memset(buf, 0xFF, padded_size);

    /* Stream from W25Q into RAM in 256 B chunks. ota_stage region
     * already holds the verified image; this is a pure read, no W25Q
     * writes. */
    uint8_t  *dst = buf;
    uint32_t  remaining = img_size;
    uint32_t  src_addr  = OTA_STAGE_REGION_BASE;
    while (remaining) {
        size_t this_chunk = (remaining < 256U) ? remaining : 256U;
        w25q_read(src_addr, dst, this_chunk);
        dst       += this_chunk;
        src_addr  += this_chunk;
        remaining -= this_chunk;
    }

    /* Sanity: confirm the RAM copy CRC also matches before pulling the
     * trigger. Out-of-band guard against a buggy w25q_read or DMA. */
    uint32_t ram_crc = crc32(buf, img_size);
    if (ram_crc != img_crc) {
        printk("flash-ota: RAM-copy CRC mismatch (got=0x%08x want=0x%08x) — "
               "aborting; clearing flag\n",
               (unsigned)ram_crc, (unsigned)img_crc);
        (void)ota_stage_clear_pending();
        return -3;
    }

    printk("flash-ota: applying staged image: size=%u crc=0x%08x "
           "— point of no return\n",
           (unsigned)img_size, (unsigned)img_crc);
    brief_uart_drain();

    /* IRQs OFF, dive into RAM. The pending_ota_flag stays set — the
     * next boot's flash_apply_pending_ota_image() will see "running
     * image already matches staged CRC" and clear the flag. This makes
     * the apply phase idempotent on power-loss mid-overwrite: a half-
     * written image won't match staged CRC, so the next boot re-applies
     * from W25Q automatically. */
    __asm__ volatile ("cpsid i" ::: "memory");

    int rc = flash_overwrite_bank0_from_ram((const uint32_t *)buf, padded_size);

    if (rc != 0) {
        /* Half-written image. Don't re-enable IRQs (running code may be
         * inconsistent). Halt loud — bench operator power-cycles, the
         * next boot resumes from W25Q. */
        for (;;) { __asm__ volatile ("nop"); }
    }

    /* Successful overwrite. Trigger a CPU reset; new image starts fresh,
     * sees pending flag, recognises the match-via-CRC, clears flag. */
    *(volatile uint32_t *)0xE000ED0Cu = 0x05FA0004u;   /* AIRCR: SYSRESETREQ */
    for (;;) { __asm__ volatile ("nop"); }
}
