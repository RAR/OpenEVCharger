#include "test_harness.h"
#include "shmem.h"
#include "shmem_offsets.h"

int main(void)
{
    struct shmem sm;
    CHECK_EQ(shmem_load_file(&sm, "test/fixtures/shmem_snapshot.bin"), 0);

    /* Byte-wise reads at named offsets (see make_shmem_fixture.py). */
    CHECK_EQ(shmem_u8(&sm, OFF_USER_STATE),  0x02);
    CHECK_EQ(shmem_u8(&sm, OFF_RED_LED),     0x02);
    CHECK_EQ(shmem_u8(&sm, OFF_PRI_STATE),   0x03);
    CHECK_EQ(shmem_u8(&sm, OFF_PILOT_STATE), 0x02);
    CHECK_EQ(shmem_u8(&sm, OFF_STM32_FAULT), 0x00);
    CHECK_EQ(shmem_u8(&sm, OFF_PILOT_DUTY),  0x32);
    CHECK_EQ(shmem_u8(&sm, OFF_RATED_AMPS),  0x1E);

    /* LE u16/u32 helpers */
    CHECK_EQ(shmem_u16_le(&sm, OFF_VRMS_MEAS), 23000); /* 230.00 V × 100 */
    CHECK_EQ(shmem_u16_le(&sm, OFF_IRMS_MEAS),   160); /*  16.0  A × 10  */
    CHECK_EQ(shmem_u32_le(&sm, OFF_POWER_MEAS), 3680u);/* 3680   W × 1   */
    CHECK_EQ(shmem_u32_le(&sm, OFF_ALARM_BITMAP), 0x00000008u);

    /* out-of-range offset returns 0 defensively, never crashes */
    CHECK_EQ(shmem_u8(&sm, SHMEM_SIZE + 100), 0);
    CHECK_EQ(shmem_u16_le(&sm, SHMEM_SIZE - 1), 0);   /* second byte OOB */
    CHECK_EQ(shmem_u32_le(&sm, SHMEM_SIZE - 2), 0);

    /* shmem_copy still works */
    unsigned char alarm[4];
    shmem_copy(&sm, OFF_ALARM_BITMAP, alarm, sizeof(alarm));
    CHECK_EQ(alarm[0], 0x08);
    CHECK_EQ(alarm[1], 0x00);

    shmem_release(&sm);
    TEST_MAIN_END();
}
