#ifndef OPENBHZD_PERSIST_BOOT_CONFIG_H
#define OPENBHZD_PERSIST_BOOT_CONFIG_H

#include <stdint.h>

#define BOOT_CONFIG_SLOT_A   0x000000U
#define BOOT_CONFIG_SLOT_B   0x001000U
#define BOOT_CONFIG_VERSION  1U

/* 32 bytes total. Layout matches spec § 6 with the addition of a
 * `monotonic_counter` field at offset 4 (managed by pingpong helper). */
struct __attribute__((packed)) boot_config {
    uint8_t  version;                   /* 1 */
    uint8_t  pad0[3];
    uint32_t monotonic_counter;         /* helper-managed */
    uint8_t  fc41d_advertised_amps;     /* 0 = unset → fall back to DIP1 */
    uint8_t  require_rfid_auth;         /* 0 = charge on plug-in (default);
                                         * 1 = gate charging behind a
                                         * matched-tag swipe per session */
    uint8_t  pending_ota_flag;          /* 0 = no pending; 1 = stage region
                                         * holds a verified image — main()
                                         * pre-FreeRTOS will copy it over
                                         * the running flash on next boot */
    uint8_t  pad1;
    uint32_t staged_image_size;         /* bytes — meaningful iff
                                         * pending_ota_flag = 1 */
    uint32_t staged_image_crc32;        /* CRC32 of the staged image */
    uint8_t  reserved[8];
    uint32_t crc32;                     /* helper-managed */
};
_Static_assert(sizeof(struct boot_config) == 32, "boot_config must be 32 B");

/* Load the current boot_config into the in-RAM cache. If both slots are
 * invalid, writes a defaults record to slot A. Call once at boot before
 * any other boot_config_* function. Returns 0 on success, <0 on error. */
int boot_config_load(void);

/* Accessor. Returns 0 (= unset) until boot_config_load() runs. */
uint8_t boot_config_advertised_amps(void);

/* Update advertised amps and ping-pong-write to W25Q. Idempotent if the
 * value is unchanged. Returns 0 on success, <0 on error. */
int boot_config_set_advertised_amps(uint8_t amps);

/* Whether charging requires a matched RFID tag per session. */
uint8_t boot_config_require_rfid_auth(void);

/* Toggle the require-auth flag and persist. Idempotent if unchanged. */
int boot_config_set_require_rfid_auth(uint8_t enable);

/* OTA staging accessors. Meaningful only after boot_config_load() runs. */
uint8_t  boot_config_pending_ota_flag(void);
uint32_t boot_config_staged_image_size(void);
uint32_t boot_config_staged_image_crc32(void);

/* Set OTA staging state and persist. Pass pending=1 with size+crc to mark
 * a staged image ready for activation on next boot; pending=0 (size and
 * crc ignored) to clear the flag after a successful copy. Returns 0 on
 * success. Idempotent if all three fields are unchanged. */
int boot_config_set_pending_ota(uint8_t pending,
                                uint32_t image_size,
                                uint32_t image_crc32);

#endif
