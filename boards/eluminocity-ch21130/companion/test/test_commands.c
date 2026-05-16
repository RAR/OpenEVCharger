/* test_commands.c — exercise the shared cs_apply_* command handlers.
 *
 * These are the same helpers the MQTT adapter and the v0.4 HTTP API both
 * call. The MQTT-side behaviour is already covered by test_mqtt_adapter;
 * this file owns the cross-transport validation matrix so neither caller
 * has to re-test it. */
#include <string.h>
#include "test_harness.h"
#include "commands.h"
#include "shmem.h"
#include "shmem_offsets.h"

/* Bring up a writable shmem buffer backed by malloc (host-fixture mode).
 * Pre-seeds the bytes we care about so out-of-band defaults can't
 * accidentally satisfy a passing assertion. */
static void make_shm(struct shmem *sm, unsigned char *backing, size_t cap)
{
    memset(backing, 0, cap);
    memset(sm, 0, sizeof(*sm));
    sm->base     = backing;
    sm->size     = cap;
    sm->shmid    = -1;
    sm->writable = 1;
}

static void test_rated_amps(void)
{
    static unsigned char raw[SHMEM_SIZE];
    struct shmem sm;
    make_shm(&sm, raw, sizeof(raw));

    char err[64];
    long out = 0;

    /* In-band integer */
    CHECK_EQ(cs_apply_rated_amps_write(&sm, (const unsigned char *)"18", 2,
                                       &out, err, sizeof(err)), 0);
    CHECK_EQ(out, 18);
    CHECK_EQ(shmem_u8(&sm, OFF_RATED_AMPS), 18);

    /* Boundary low */
    CHECK_EQ(cs_apply_rated_amps_write(&sm, (const unsigned char *)"6", 1,
                                       &out, err, sizeof(err)), 0);
    CHECK_EQ(shmem_u8(&sm, OFF_RATED_AMPS), 6);

    /* Boundary high */
    CHECK_EQ(cs_apply_rated_amps_write(&sm, (const unsigned char *)"30", 2,
                                       &out, err, sizeof(err)), 0);
    CHECK_EQ(shmem_u8(&sm, OFF_RATED_AMPS), 30);

    /* Below range — must NOT mutate */
    err[0] = '\0';
    CHECK_EQ(cs_apply_rated_amps_write(&sm, (const unsigned char *)"5", 1,
                                       &out, err, sizeof(err)), -1);
    CHECK_EQ(shmem_u8(&sm, OFF_RATED_AMPS), 30);   /* unchanged */
    CHECK(strlen(err) > 0);

    /* Above range */
    CHECK_EQ(cs_apply_rated_amps_write(&sm, (const unsigned char *)"31", 2,
                                       &out, err, sizeof(err)), -1);
    CHECK_EQ(shmem_u8(&sm, OFF_RATED_AMPS), 30);

    /* Non-numeric */
    CHECK_EQ(cs_apply_rated_amps_write(&sm, (const unsigned char *)"abc", 3,
                                       &out, err, sizeof(err)), -1);
    CHECK_EQ(shmem_u8(&sm, OFF_RATED_AMPS), 30);

    /* Trailing whitespace is tolerated */
    CHECK_EQ(cs_apply_rated_amps_write(&sm, (const unsigned char *)"16\n", 3,
                                       &out, err, sizeof(err)), 0);
    CHECK_EQ(shmem_u8(&sm, OFF_RATED_AMPS), 16);

    /* Empty payload */
    CHECK_EQ(cs_apply_rated_amps_write(&sm, (const unsigned char *)"", 0,
                                       &out, err, sizeof(err)), -1);

    /* NULL shmem -> -2 */
    CHECK_EQ(cs_apply_rated_amps_write(NULL, (const unsigned char *)"15", 2,
                                       &out, err, sizeof(err)), -2);
}

static void test_authorize(void)
{
    static unsigned char raw[SHMEM_SIZE];
    struct shmem sm;
    make_shm(&sm, raw, sizeof(raw));

    char err[64];
    int  on = -1;

    /* ON */
    CHECK_EQ(cs_apply_authorize_write(&sm, (const unsigned char *)"ON", 2,
                                      &on, err, sizeof(err)), 0);
    CHECK_EQ(on, 1);
    CHECK_EQ(shmem_u8(&sm, OFF_USER_STATE), 1);

    /* OFF */
    CHECK_EQ(cs_apply_authorize_write(&sm, (const unsigned char *)"OFF", 3,
                                      &on, err, sizeof(err)), 0);
    CHECK_EQ(on, 0);
    CHECK_EQ(shmem_u8(&sm, OFF_USER_STATE), 0);

    /* Anything else — case mismatch, partial, garbage, empty: rejected */
    shmem_write_u8(&sm, OFF_USER_STATE, 0x55);    /* sentinel */
    CHECK_EQ(cs_apply_authorize_write(&sm, (const unsigned char *)"on", 2,
                                      &on, err, sizeof(err)), -1);
    CHECK_EQ(shmem_u8(&sm, OFF_USER_STATE), 0x55);

    CHECK_EQ(cs_apply_authorize_write(&sm, (const unsigned char *)"O", 1,
                                      &on, err, sizeof(err)), -1);
    CHECK_EQ(cs_apply_authorize_write(&sm, (const unsigned char *)"", 0,
                                      &on, err, sizeof(err)), -1);
    CHECK_EQ(cs_apply_authorize_write(&sm, (const unsigned char *)"true", 4,
                                      &on, err, sizeof(err)), -1);
    CHECK_EQ(shmem_u8(&sm, OFF_USER_STATE), 0x55);

    /* NULL shmem */
    CHECK_EQ(cs_apply_authorize_write(NULL, (const unsigned char *)"ON", 2,
                                      &on, err, sizeof(err)), -2);
}

static void test_clear_faults(void)
{
    static unsigned char raw[SHMEM_SIZE];
    struct shmem sm;
    make_shm(&sm, raw, sizeof(raw));

    char err[64];
    /* Seed a non-zero alarm bitmap. */
    shmem_write_u32_le(&sm, OFF_ALARM_BITMAP, 0xDEADBEEFu);
    CHECK_EQ(shmem_u32_le(&sm, OFF_ALARM_BITMAP), 0xDEADBEEFu);

    CHECK_EQ(cs_apply_clear_faults_write(&sm, err, sizeof(err)), 0);
    CHECK_EQ(shmem_u32_le(&sm, OFF_ALARM_BITMAP), 0u);

    /* Idempotent — second call leaves it zero. */
    CHECK_EQ(cs_apply_clear_faults_write(&sm, err, sizeof(err)), 0);
    CHECK_EQ(shmem_u32_le(&sm, OFF_ALARM_BITMAP), 0u);

    /* NULL shmem */
    CHECK_EQ(cs_apply_clear_faults_write(NULL, err, sizeof(err)), -2);
}

int main(void)
{
    test_rated_amps();
    test_authorize();
    test_clear_faults();
    TEST_MAIN_END();
}
