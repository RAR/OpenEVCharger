#include "gfci.h"
#include "../core/pin_map.h"
#include "../hal/uart.h"
#include "gd32f20x.h"

/* PE2 is configured as input pull-up by gpio_init_all() (it appeared
 * in the legacy strap-list under `PIN_STRAP_PE2_*`; the role-rename
 * to `PIN_GFCI_SENSE_*` happened after the bench wiggle confirmed
 * the function — pin_map.h has both spellings until the strap macros
 * are deprecated). gfci_init() is therefore a no-op on the GPIO; it
 * exists for symmetry with other HAL inits and as a hook for future
 * CAL self-test scheduling. */
void gfci_init(void)
{
    /* Defensive: re-assert input pull-up in case some earlier init
     * touched the pad. Idempotent. */
    gpio_bit_set(PIN_GFCI_SENSE_PORT, PIN_GFCI_SENSE_PIN);
    gpio_init(PIN_GFCI_SENSE_PORT, GPIO_MODE_IPU,
              GPIO_OSPEED_50MHZ, PIN_GFCI_SENSE_PIN);
}

int gfci_fault_active(void)
{
    /* Active-low: module pulls PE2 LOW on fault, idle HIGH. */
    return (gpio_input_bit_get(PIN_GFCI_SENSE_PORT, PIN_GFCI_SENSE_PIN)
            == RESET) ? 1 : 0;
}

/* Polarity-agnostic CAL self-test. The bench-traced wiring
 * description (MCU LOW = CAL asserted, vs the inline comment claiming
 * idle-low) is contradictory in pin_map.h; we side-step it by
 * sampling PE3's current state (whatever init_outputs_safe_low left
 * it at) as "idle", driving the OPPOSITE level for the pulse window,
 * then restoring. PE2 (active-low sense) should fall to LOW during
 * the asserted window OR during the post-release recover window
 * (some GFCI chips integrate imbalance and only latch the comparator
 * after CAL release).
 *
 * Timing: bench 2026-05-06 first attempt with 60 ms pulse failed —
 * scope confirmed PE3 went low for 60 ms but PE2 never asserted.
 * Bumped to 500 ms (typical RV4145 / FAN4147 / similar GFCI chip
 * integration time). Recover bumped to 200 ms for chips that latch
 * post-release. Per-poll diagnostic logging emitted on every PE2
 * edge so subsequent bench iterations can pinpoint whether PE2 is
 * moving at all during the window. Total budget 700 ms; IWDG is
 * 1000 ms and wdg_kick fires at the top of safety_task tick before
 * drain_inbox so the budget is safe. */
#ifndef GFCI_CAL_PULSE_MS
#define GFCI_CAL_PULSE_MS        500U
#endif
#ifndef GFCI_CAL_POLL_INTERVAL_MS
#define GFCI_CAL_POLL_INTERVAL_MS 10U
#endif
#ifndef GFCI_CAL_RECOVER_MS
#define GFCI_CAL_RECOVER_MS      200U
#endif

static void busy_wait_us(uint32_t us)
{
    /* Coarse busy-wait sized for boot-time use only. The system
     * runs at 120 MHz with REAL_PLL; a NOP is ~8 ns so 125 NOPs ≈ 1 µs.
     * Self-test runs once at boot before the scheduler so a tight
     * loop here is acceptable. */
    volatile uint32_t loops = us * 15U;
    while (loops--) __asm__ volatile ("nop");
}

int gfci_self_test(void)
{
    /* If PE2 is already asserted before we touch CAL the module is
     * either tripped from a prior cycle or wired wrong — bail before
     * compounding the situation. */
    if (gfci_fault_active()) {
        return -3;
    }

    /* Snapshot the idle level of PE3 and drive the inverse for the
     * pulse window. */
    int idle_level = (gpio_input_bit_get(PIN_GFCI_CAL_PORT,
                                         PIN_GFCI_CAL_PIN) == SET) ? 1 : 0;
    printk("gfci-cal: PE3 idle=%d, asserting opposite for %u ms\n",
           idle_level, (unsigned)GFCI_CAL_PULSE_MS);
    if (idle_level) {
        gpio_bit_reset(PIN_GFCI_CAL_PORT, PIN_GFCI_CAL_PIN);
    } else {
        gpio_bit_set(PIN_GFCI_CAL_PORT, PIN_GFCI_CAL_PIN);
    }

    /* Phase 1: PE2 watch during CAL pulse. Log every state edge so the
     * bench operator can compare scope-vs-firmware. */
    int last_pe2 = 0;   /* gfci_fault_active() returned 0 above */
    int saw_assert_during_pulse = 0;
    for (uint32_t t = 0; t < GFCI_CAL_PULSE_MS;
         t += GFCI_CAL_POLL_INTERVAL_MS) {
        busy_wait_us(GFCI_CAL_POLL_INTERVAL_MS * 1000U);
        int pe2 = gfci_fault_active();
        if (pe2 != last_pe2) {
            printk("gfci-cal: t=%u ms PE2 %d -> %d (during pulse)\n",
                   (unsigned)(t + GFCI_CAL_POLL_INTERVAL_MS),
                   last_pe2, pe2);
            last_pe2 = pe2;
        }
        if (pe2) saw_assert_during_pulse = 1;
    }

    /* Restore CAL to idle. */
    if (idle_level) {
        gpio_bit_set(PIN_GFCI_CAL_PORT, PIN_GFCI_CAL_PIN);
    } else {
        gpio_bit_reset(PIN_GFCI_CAL_PORT, PIN_GFCI_CAL_PIN);
    }
    printk("gfci-cal: pulse end (saw_assert=%d), watching recover %u ms\n",
           saw_assert_during_pulse, (unsigned)GFCI_CAL_RECOVER_MS);

    /* Phase 2: recover window. Some GFCI chips latch the comparator
     * AFTER the CAL pulse releases (slow integrator that crosses the
     * trip threshold during release). So watch for both:
     *   - if it asserted during the pulse, expect it to release here
     *   - if it didn't assert during the pulse, accept a late assert
     *     here as PASS too. */
    int saw_assert_post = 0;
    int saw_release = 0;
    for (uint32_t t = 0; t < GFCI_CAL_RECOVER_MS;
         t += GFCI_CAL_POLL_INTERVAL_MS) {
        busy_wait_us(GFCI_CAL_POLL_INTERVAL_MS * 1000U);
        int pe2 = gfci_fault_active();
        if (pe2 != last_pe2) {
            printk("gfci-cal: t=%u ms PE2 %d -> %d (recover)\n",
                   (unsigned)(GFCI_CAL_PULSE_MS + t + GFCI_CAL_POLL_INTERVAL_MS),
                   last_pe2, pe2);
            last_pe2 = pe2;
        }
        if (pe2) saw_assert_post = 1;
        else if (saw_assert_during_pulse || saw_assert_post) saw_release = 1;
    }

    if (!saw_assert_during_pulse && !saw_assert_post) {
        return -1;
    }
    if ((saw_assert_during_pulse || saw_assert_post) && !saw_release) {
        return -2;
    }
    return 0;
}
