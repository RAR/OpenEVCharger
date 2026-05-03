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
    uint8_t  pad1[3];
    uint8_t  reserved[16];
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

#endif
