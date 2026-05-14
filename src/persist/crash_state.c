#include "crash_state.h"
#include "pingpong.h"
#include "../hal/uart.h"
#include "../hal/reset_cause.h"
#include <string.h>

/* Read + clear the chip's reset-cause flags, returning non-zero if the
 * reset was caused by a watchdog or low-power event — the only
 * signatures we treat as a "real crash". Power-on, NRST pin, and
 * software-reset (openocd `reset run`, panic-driven NVIC SystemReset)
 * are all considered deliberate boots and do NOT increment
 * fast_restart_count.
 *
 * Hardfault recovery still trips fast_restart correctly: the trap
 * handlers spin with interrupts disabled, so IWDG keeps ticking and
 * fires after ~1 s, leaving the watchdog flag set on the next boot.
 *
 * The raw reset-cause register decode lives in the per-chip
 * src/hal/<chip>/reset_cause.c — this stays chip-neutral. */
static int read_and_clear_reset_was_crash(void)
{
    reset_cause_t cause = reset_cause_get_and_clear();
    int was_crash = (cause == RESET_CAUSE_WATCHDOG ||
                     cause == RESET_CAUSE_LOW_POWER) ? 1 : 0;

    printk("crash_state: reset cause = %s (%s)\n",
           reset_cause_str(cause), was_crash ? "counts" : "skipped");

    return was_crash;
}

static struct crash_state s_cs;
static int                s_safe_fail = 0;

int crash_state_boot_increment(void)
{
    int was_crash = read_and_clear_reset_was_crash();

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

    /* Only count as a fast restart if the reset was caused by the
     * watchdog (real crash signature). Power-on, NRST pin, and
     * software resets (openocd reflash, deliberate reboot) are
     * deliberate and don't bump the counter. A deliberate boot also
     * resets the count to 0 so a previously-fast-cycling bench
     * recovers as soon as you do a clean POR. */
    if (was_crash) {
        if (s_cs.fast_restart_count < 0xFFU) s_cs.fast_restart_count++;
    } else {
        s_cs.fast_restart_count = 0;
    }
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
