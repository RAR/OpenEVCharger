#include "boot_config.h"
#include "pingpong.h"
#include "../hal/uart.h"
#include <string.h>

static struct boot_config s_cfg;

int boot_config_load(void)
{
    uint8_t  slot = 0;
    uint32_t counter = 0;
    int rc = pingpong_load(BOOT_CONFIG_SLOT_A, BOOT_CONFIG_SLOT_B,
                           &s_cfg, sizeof s_cfg, &slot, &counter);
    if (rc < 0) {
        printk("boot_config: pingpong_load FAIL rc=%d\n", rc);
        return rc;
    }
    if (rc == 1) {
        memset(&s_cfg, 0, sizeof s_cfg);
        s_cfg.version = BOOT_CONFIG_VERSION;
        s_cfg.fc41d_advertised_amps = 0;

        rc = pingpong_store(BOOT_CONFIG_SLOT_A, BOOT_CONFIG_SLOT_B,
                            &s_cfg, sizeof s_cfg, &slot, &counter);
        if (rc < 0) {
            printk("boot_config: defaults write FAIL rc=%d\n", rc);
            return rc;
        }
        printk("boot_config: defaults written -> slot %c (counter=%u, advertised_amps=%u)\n",
               'A' + slot, (unsigned)counter,
               (unsigned)s_cfg.fc41d_advertised_amps);
        return 0;
    }

    if (s_cfg.version != BOOT_CONFIG_VERSION) {
        printk("boot_config: unknown version=%u, using as-is\n",
               (unsigned)s_cfg.version);
    }
    printk("boot_config: loaded from slot %c (counter=%u, advertised_amps=%u)\n",
           'A' + slot, (unsigned)counter,
           (unsigned)s_cfg.fc41d_advertised_amps);
    return 0;
}

uint8_t boot_config_advertised_amps(void)
{
    return s_cfg.fc41d_advertised_amps;
}

static int store(const char *what)
{
    s_cfg.version = BOOT_CONFIG_VERSION;
    uint8_t  slot = 0;
    uint32_t counter = 0;
    int rc = pingpong_store(BOOT_CONFIG_SLOT_A, BOOT_CONFIG_SLOT_B,
                            &s_cfg, sizeof s_cfg, &slot, &counter);
    if (rc < 0) {
        printk("boot_config: %s store FAIL rc=%d\n", what, rc);
        return rc;
    }
    printk("boot_config: stored -> slot %c (counter=%u, "
           "advertised_amps=%u, require_rfid_auth=%u) [%s]\n",
           'A' + slot, (unsigned)counter,
           (unsigned)s_cfg.fc41d_advertised_amps,
           (unsigned)s_cfg.require_rfid_auth, what);
    return 0;
}

int boot_config_set_advertised_amps(uint8_t amps)
{
    if (s_cfg.fc41d_advertised_amps == amps) return 0;
    s_cfg.fc41d_advertised_amps = amps;
    return store("advertised_amps");
}

uint8_t boot_config_require_rfid_auth(void)
{
    return s_cfg.require_rfid_auth;
}

int boot_config_set_require_rfid_auth(uint8_t enable)
{
    enable = enable ? 1u : 0u;
    if (s_cfg.require_rfid_auth == enable) return 0;
    s_cfg.require_rfid_auth = enable;
    return store("require_rfid_auth");
}

uint8_t boot_config_pending_ota_flag(void)
{
    return s_cfg.pending_ota_flag;
}

uint32_t boot_config_staged_image_size(void)
{
    return s_cfg.staged_image_size;
}

uint32_t boot_config_staged_image_crc32(void)
{
    return s_cfg.staged_image_crc32;
}

int boot_config_set_pending_ota(uint8_t pending,
                                uint32_t image_size,
                                uint32_t image_crc32)
{
    pending = pending ? 1u : 0u;
    if (!pending) {
        image_size = 0;
        image_crc32 = 0;
    }
    if (s_cfg.pending_ota_flag    == pending    &&
        s_cfg.staged_image_size   == image_size &&
        s_cfg.staged_image_crc32  == image_crc32) {
        return 0;
    }
    s_cfg.pending_ota_flag   = pending;
    s_cfg.staged_image_size  = image_size;
    s_cfg.staged_image_crc32 = image_crc32;
    return store("pending_ota");
}
