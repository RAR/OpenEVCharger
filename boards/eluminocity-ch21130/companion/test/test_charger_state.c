#include <string.h>
#include "test_harness.h"
#include "shmem.h"
#include "shmem_offsets.h"
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
    CHECK_EQ(cs.fault_bits, 0x00000008u);   /* fixture alarm[3] nonzero -> bit 3 */

    /* unknown connector byte -> EVSE_STATE_UNKNOWN, never crashes */
    struct charger_state cs2;
    charger_state_init(&cs2);
    /* directly exercise the decoder via a hand-built shmem buffer */
    static unsigned char raw[0x40000];
    memset(raw, 0, sizeof(raw));
    raw[OFF_CONNECTOR_STATE] = 0xEE;
    struct shmem sm2 = { .base = raw, .size = sizeof(raw), .shmid = -1 };
    charger_state_read(&cs2, &sm2);
    CHECK_EQ(cs2.evse_state, EVSE_STATE_UNKNOWN);

    CHECK_STR(evse_state_str(EVSE_STATE_CHARGING), "charging");
    CHECK_STR(evse_state_str(EVSE_STATE_UNKNOWN), "unknown");

    /* --- change detection --- */
    struct charger_state a, b;
    charger_state_init(&a);
    charger_state_init(&b);
    a.voltage_v = 120; a.current_a = 16;
    b.voltage_v = 120; b.current_a = 16;
    CHECK_EQ(charger_state_diff(&a, &b), 0);          /* identical -> no dirty bits */
    b.current_a = 17;
    CHECK_EQ(charger_state_diff(&a, &b) & CS_DIRTY_CURRENT, CS_DIRTY_CURRENT);
    CHECK_EQ(charger_state_diff(&a, &b) & CS_DIRTY_VOLTAGE, 0);
    b.current_a = a.current_a;                          /* reset: only test one field at a time */
    b.evse_state = EVSE_STATE_CHARGING;                 /* a.evse_state is UNKNOWN from init */
    CHECK_EQ(charger_state_diff(&a, &b) & CS_DIRTY_EVSE_STATE, CS_DIRTY_EVSE_STATE);
    b.evse_state = a.evse_state;
    b.fault_bits = 0x4;
    CHECK_EQ(charger_state_diff(&a, &b) & CS_DIRTY_FAULTS, CS_DIRTY_FAULTS);

    /* --- fault names --- */
    CHECK_STR(charger_fault_name(0), "RCD");
    CHECK_STR(charger_fault_name(99), "UNKNOWN");      /* out-of-range high */
    CHECK_STR(charger_fault_name(-1), "UNKNOWN");      /* out-of-range low */

    shmem_release(&sm);
    TEST_MAIN_END();
}
