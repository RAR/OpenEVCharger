#include <string.h>
#include "test_harness.h"
#include "shmem.h"
#include "charger_state.h"

int main(void)
{
    struct shmem sm;
    CHECK_EQ(shmem_load_file(&sm, "test/fixtures/shmem_snapshot.bin"), 0);

    struct charger_state cs;
    charger_state_init(&cs);
    charger_state_read(&cs, &sm);

    /* Fixture: VRMS raw 0x78 (120), IRMS raw 0x10 (16). v1 scaling is
     * identity-with-units until bench-tuned (M1) — see charger_state.c. */
    CHECK_EQ(cs.voltage_v,   120);
    CHECK_EQ(cs.current_a,   16);
    CHECK_EQ(cs.stm32_link,  1);
    CHECK_EQ(cs.evse_state,  EVSE_STATE_CONNECTED);   /* connector byte 0x02 */
    CHECK_EQ(cs.heartbeat,   0x55);

    /* unknown connector byte -> EVSE_STATE_UNKNOWN, never crashes */
    struct charger_state cs2;
    charger_state_init(&cs2);
    /* directly exercise the decoder via a hand-built shmem buffer */
    unsigned char raw[0x40000];
    memset(raw, 0, sizeof(raw));
    raw[0x0a00] = 0xEE;
    struct shmem sm2 = { .base = raw, .size = sizeof(raw), .shmid = -1 };
    charger_state_read(&cs2, &sm2);
    CHECK_EQ(cs2.evse_state, EVSE_STATE_UNKNOWN);

    shmem_release(&sm);
    TEST_MAIN_END();
}
