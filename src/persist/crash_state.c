#include "crash_state.h"
#include "pingpong.h"
#include "../hal/uart.h"
#include <string.h>

static struct crash_state s_cs;
static int                s_safe_fail = 0;

int crash_state_boot_increment(void)
{
    uint8_t  slot = 0;
    uint32_t counter = 0;
    int rc = pingpong_load(CRASH_STATE_SLOT_A, CRASH_STATE_SLOT_B,
                           &s_cs, sizeof s_cs, &slot, &counter);
    if (rc < 0) {
        printk("crash_state: pingpong_load FAIL rc=%d\n", rc);
        return rc;
    }
    if (rc == 1) {
        memset(&s_cs, 0, sizeof s_cs);
        s_cs.version = CRASH_STATE_VERSION;
        s_cs.fast_restart_count = 0;
    }

    if (s_cs.fast_restart_count < 0xFFU) s_cs.fast_restart_count++;
    s_safe_fail = (s_cs.fast_restart_count >= CRASH_LOOP_THRESHOLD);

    s_cs.version = CRASH_STATE_VERSION;
    rc = pingpong_store(CRASH_STATE_SLOT_A, CRASH_STATE_SLOT_B,
                        &s_cs, sizeof s_cs, &slot, &counter);
    if (rc < 0) {
        printk("crash_state: store FAIL rc=%d\n", rc);
        return rc;
    }

    if (s_safe_fail) {
        printk("crash_state: SAFE-FAIL ENTERED (fast_restart=%u)\n",
               (unsigned)s_cs.fast_restart_count);
    } else {
        printk("crash_state: fast_restart=%u (slot %c counter=%u)\n",
               (unsigned)s_cs.fast_restart_count,
               'A' + slot, (unsigned)counter);
    }
    return 0;
}

int crash_state_reset_alive(void)
{
    if (s_cs.fast_restart_count == 0 && !s_safe_fail) return 0;

    s_cs.version = CRASH_STATE_VERSION;
    s_cs.fast_restart_count = 0;

    uint8_t  slot = 0;
    uint32_t counter = 0;
    int rc = pingpong_store(CRASH_STATE_SLOT_A, CRASH_STATE_SLOT_B,
                            &s_cs, sizeof s_cs, &slot, &counter);
    if (rc < 0) {
        printk("crash_state: alive write FAIL rc=%d\n", rc);
        return rc;
    }
    s_safe_fail = 0;
    printk("crash_state: alive marker -> fast_restart=0 (slot %c counter=%u)\n",
           'A' + slot, (unsigned)counter);
    return 0;
}

int crash_state_is_safe_fail(void) { return s_safe_fail; }
