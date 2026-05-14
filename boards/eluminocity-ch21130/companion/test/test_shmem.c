#include "test_harness.h"
#include "shmem.h"
#include "shmem_offsets.h"

int main(void)
{
    struct shmem sm;
    CHECK_EQ(shmem_load_file(&sm, "test/fixtures/shmem_snapshot.bin"), 0);

    CHECK_EQ(shmem_u8(&sm, OFF_CONNECTOR_STATE), 0x02);
    CHECK_EQ(shmem_u8(&sm, OFF_HEARTBEAT),       0x55);
    CHECK_EQ(shmem_u8(&sm, OFF_STM32_LINK),      0x01);
    CHECK_EQ(shmem_u8(&sm, OFF_VRMS),            0x78);
    CHECK_EQ(shmem_u8(&sm, OFF_IRMS),            0x10);
    CHECK_EQ(shmem_u8(&sm, OFF_FAULT_FLAGS),     0x42);
    CHECK_EQ(shmem_u8(&sm, OFF_FW_UPGRADE_GATE), 0x09);

    /* out-of-range offset returns 0 defensively, never crashes */
    CHECK_EQ(shmem_u8(&sm, SHMEM_SIZE + 100), 0);

    /* alarm bitmap copy */
    unsigned char alarm[ALARM_BITMAP_LEN];
    shmem_copy(&sm, OFF_ALARM_BITMAP, alarm, ALARM_BITMAP_LEN);
    CHECK_EQ(alarm[3], 0x01);
    CHECK_EQ(alarm[0], 0x00);

    shmem_release(&sm);
    TEST_MAIN_END();
}
