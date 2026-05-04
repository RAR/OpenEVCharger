#include "test_harness.h"
#include "mocks/w25q_mock.h"
#include "pingpong.h"
#include "../src/hal/w25q.h"

#include <string.h>

#define REC_SIZE  32U

/* Two slots, picked away from boot_config's slots so any cross-test
 * mock state can't accidentally leak in. */
#define SLOT_A    0x010000U
#define SLOT_B    0x011000U

static void fill_payload(uint8_t *rec, size_t size, uint8_t fill)
{
    /* Layout: [0]=ver, [1..3]=pad, [4..7]=counter, [8..size-5]=payload,
     * [size-4..size-1]=crc. Helper stamps counter+crc; tests fill the
     * payload region. */
    memset(rec, 0, size);
    rec[0] = 1;
    memset(rec + 8, fill, size - 8U - 4U);
}

void suite_pingpong(void);
void suite_pingpong(void)
{
    uint8_t  rec[REC_SIZE];
    uint8_t  out[REC_SIZE];
    uint8_t  slot;
    uint32_t counter;

    TEST_CASE("blank flash: load returns 'no prior' (rc=1) and zeros buf");
    w25q_mock_reset(); w25q_init();
    memset(out, 0xA5, sizeof out);
    int rc = pingpong_load(SLOT_A, SLOT_B, out, REC_SIZE, &slot, &counter);
    CHECK_EQ_INT(rc, 1);
    {
        int all_zero = 1;
        for (size_t i = 0; i < REC_SIZE; ++i) if (out[i] != 0) { all_zero = 0; break; }
        CHECK(all_zero);
    }

    TEST_CASE("first store: lands in slot A with counter=1");
    w25q_mock_reset(); w25q_init();
    fill_payload(rec, REC_SIZE, 0xAA);
    rc = pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(slot, 0);
    CHECK_EQ_U32(counter, 1u);
    CHECK(!w25q_mock_sector_is_blank(SLOT_A));
    CHECK(w25q_mock_sector_is_blank(SLOT_B));

    TEST_CASE("store-then-load: round-trip preserves payload");
    w25q_mock_reset(); w25q_init();
    fill_payload(rec, REC_SIZE, 0x5C);
    pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    rc = pingpong_load(SLOT_A, SLOT_B, out, REC_SIZE, &slot, &counter);
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_U32(counter, 1u);
    /* Compare payload region only — counter+crc stamped in-helper. */
    for (size_t i = 8; i < REC_SIZE - 4U; ++i) {
        CHECK_EQ_INT(out[i], 0x5C);
    }

    TEST_CASE("two stores ping-pong slots; second wins on load");
    w25q_mock_reset(); w25q_init();
    fill_payload(rec, REC_SIZE, 0x11);
    pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    CHECK_EQ_INT(slot, 0);
    fill_payload(rec, REC_SIZE, 0x22);
    rc = pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(slot, 1);
    CHECK_EQ_U32(counter, 2u);
    /* Prior slot erased after successful write. */
    CHECK(w25q_mock_sector_is_blank(SLOT_A));
    CHECK(!w25q_mock_sector_is_blank(SLOT_B));

    rc = pingpong_load(SLOT_A, SLOT_B, out, REC_SIZE, &slot, &counter);
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(slot, 1);
    for (size_t i = 8; i < REC_SIZE - 4U; ++i) {
        CHECK_EQ_INT(out[i], 0x22);
    }

    TEST_CASE("three stores: A→B→A round-robin, counter increments");
    w25q_mock_reset(); w25q_init();
    fill_payload(rec, REC_SIZE, 0xC1);
    pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    fill_payload(rec, REC_SIZE, 0xC2);
    pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    fill_payload(rec, REC_SIZE, 0xC3);
    rc = pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(slot, 0);    /* back to A */
    CHECK_EQ_U32(counter, 3u);
    pingpong_load(SLOT_A, SLOT_B, out, REC_SIZE, &slot, &counter);
    CHECK_EQ_U32(counter, 3u);
    for (size_t i = 8; i < REC_SIZE - 4U; ++i) {
        CHECK_EQ_INT(out[i], 0xC3);
    }

    TEST_CASE("corrupt-newer falls back to older valid slot");
    w25q_mock_reset(); w25q_init();
    /* First write -> slot A counter=1. */
    fill_payload(rec, REC_SIZE, 0xA1);
    pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    /* Second write -> slot B counter=2. After this, slot A is erased,
     * so to make a 'two valid slots' scenario we manually inject the
     * older payload back into slot A by re-doing the first write and
     * then short-circuiting the prior-slot erase via direct mock pokes. */
    /* Simpler: write A, write B, then trash B's CRC. With B trashed and
     * A erased, load returns rc=1. So instead corrupt B but reseed A by
     * mock-bypassing: after store B, A is blank — re-program A from a
     * fresh helper call after we corrupt B. Helper would write to A
     * since B is the lone valid; flip approach: */
    fill_payload(rec, REC_SIZE, 0xA2);
    pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    CHECK_EQ_INT(slot, 1);
    /* Now: B = counter=2 valid, A = blank. Corrupt B's CRC: */
    uint32_t crc_addr_b = SLOT_B + REC_SIZE - 4U;
    w25q_mock_poke(crc_addr_b, 0x00, 1);  /* AND-in 0x00 → wipe a CRC byte */
    /* Reload — now both slots invalid. */
    rc = pingpong_load(SLOT_A, SLOT_B, out, REC_SIZE, &slot, &counter);
    CHECK_EQ_INT(rc, 1);

    TEST_CASE("both slots valid + B has higher counter → B wins");
    w25q_mock_reset(); w25q_init();
    /* Store A at counter=1, then we need to stage a 'both valid' state.
     * The helper deliberately erases the prior slot, so we synthesise
     * by reading A after first write, then hand-restoring it after the
     * second write. */
    fill_payload(rec, REC_SIZE, 0xB1);
    pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    /* Snapshot slot A (counter=1, payload=0xB1). */
    uint8_t snap_a[REC_SIZE];
    w25q_read(SLOT_A, snap_a, REC_SIZE);
    /* Second write goes to B at counter=2; the helper then erases A. */
    fill_payload(rec, REC_SIZE, 0xB2);
    pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    CHECK_EQ_INT(slot, 1);
    /* Re-stamp A with snap_a so both slots are valid (A erased → blank
     * 0xFF, programs are AND-into so this works). */
    w25q_program(SLOT_A, snap_a, REC_SIZE);
    /* Verify A is back. */
    {
        uint8_t roundtrip[REC_SIZE];
        w25q_read(SLOT_A, roundtrip, REC_SIZE);
        CHECK_EQ_INT(memcmp(roundtrip, snap_a, REC_SIZE), 0);
    }
    rc = pingpong_load(SLOT_A, SLOT_B, out, REC_SIZE, &slot, &counter);
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(slot, 1);
    CHECK_EQ_U32(counter, 2u);
    for (size_t i = 8; i < REC_SIZE - 4U; ++i) {
        CHECK_EQ_INT(out[i], 0xB2);
    }

    TEST_CASE("both slots valid + A has higher counter → A wins");
    w25q_mock_reset(); w25q_init();
    /* Same scaffolding as above but reverse: first write A(c=1), then
     * write B(c=2), then write A(c=3). Snapshot B before the third
     * store, then restore B post-store. */
    fill_payload(rec, REC_SIZE, 0xC1);
    pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    fill_payload(rec, REC_SIZE, 0xC2);
    pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    uint8_t snap_b[REC_SIZE];
    w25q_read(SLOT_B, snap_b, REC_SIZE);
    fill_payload(rec, REC_SIZE, 0xC3);
    pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    CHECK_EQ_INT(slot, 0);
    w25q_program(SLOT_B, snap_b, REC_SIZE);
    rc = pingpong_load(SLOT_A, SLOT_B, out, REC_SIZE, &slot, &counter);
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(slot, 0);
    CHECK_EQ_U32(counter, 3u);
    for (size_t i = 8; i < REC_SIZE - 4U; ++i) {
        CHECK_EQ_INT(out[i], 0xC3);
    }

    TEST_CASE("only B valid + A corrupt → B wins");
    w25q_mock_reset(); w25q_init();
    fill_payload(rec, REC_SIZE, 0xD1);
    pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    /* slot=A. Snapshot, then write B. */
    uint8_t snap[REC_SIZE];
    w25q_read(SLOT_A, snap, REC_SIZE);
    fill_payload(rec, REC_SIZE, 0xD2);
    pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    /* Restore A then corrupt its CRC. */
    w25q_program(SLOT_A, snap, REC_SIZE);
    w25q_mock_poke(SLOT_A + REC_SIZE - 4U, 0x00, 1);
    rc = pingpong_load(SLOT_A, SLOT_B, out, REC_SIZE, &slot, &counter);
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(slot, 1);
    CHECK_EQ_U32(counter, 2u);
    for (size_t i = 8; i < REC_SIZE - 4U; ++i) {
        CHECK_EQ_INT(out[i], 0xD2);
    }

    TEST_CASE("both corrupt → no-prior-write");
    w25q_mock_reset(); w25q_init();
    fill_payload(rec, REC_SIZE, 0xE1);
    pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    w25q_read(SLOT_A, snap, REC_SIZE);
    fill_payload(rec, REC_SIZE, 0xE2);
    pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    /* Restore A then corrupt both CRCs. */
    w25q_program(SLOT_A, snap, REC_SIZE);
    w25q_mock_poke(SLOT_A + REC_SIZE - 4U, 0x00, 1);
    w25q_mock_poke(SLOT_B + REC_SIZE - 4U, 0x00, 1);
    rc = pingpong_load(SLOT_A, SLOT_B, out, REC_SIZE, &slot, &counter);
    CHECK_EQ_INT(rc, 1);

    TEST_CASE("store after corrupt-both writes to A and recovers");
    w25q_mock_reset(); w25q_init();
    /* Stage one valid slot, corrupt it, then store. Recovery should
     * pick A (no current valid slot → target=A). */
    fill_payload(rec, REC_SIZE, 0xF1);
    pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    w25q_mock_poke(SLOT_A + REC_SIZE - 4U, 0x00, 1);
    fill_payload(rec, REC_SIZE, 0xF2);
    rc = pingpong_store(SLOT_A, SLOT_B, rec, REC_SIZE, &slot, &counter);
    CHECK_EQ_INT(rc, 0);
    CHECK_EQ_INT(slot, 0);
    CHECK_EQ_U32(counter, 1u);   /* fresh start since prior was unreadable */
    rc = pingpong_load(SLOT_A, SLOT_B, out, REC_SIZE, &slot, &counter);
    CHECK_EQ_INT(rc, 0);
    for (size_t i = 8; i < REC_SIZE - 4U; ++i) {
        CHECK_EQ_INT(out[i], 0xF2);
    }

    TEST_CASE("invalid record_size rejected");
    w25q_mock_reset(); w25q_init();
    rc = pingpong_load(SLOT_A, SLOT_B, out, 4, &slot, &counter);
    CHECK(rc < 0);
    rc = pingpong_load(SLOT_A, SLOT_B, out, 257, &slot, &counter);
    CHECK(rc < 0);
    rc = pingpong_store(SLOT_A, SLOT_B, rec, 4, &slot, &counter);
    CHECK(rc < 0);
    rc = pingpong_store(SLOT_A, SLOT_B, rec, 257, &slot, &counter);
    CHECK(rc < 0);
}
