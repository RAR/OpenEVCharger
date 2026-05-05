#include "gfci.h"
#include "../core/pin_map.h"
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
 * the asserted window and return HIGH after restore.
 *
 * Timing: spec calls for ~50 ms pulse. Sample PE2 every 5 ms over a
 * 60 ms window so we tolerate slight CAL-→-sense propagation lag.
 * After de-assert, give the module 100 ms to settle before declaring
 * stuck-low. */
#define GFCI_CAL_PULSE_MS         60U
#define GFCI_CAL_POLL_INTERVAL_MS  5U
#define GFCI_CAL_RECOVER_MS      100U

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
    if (idle_level) {
        gpio_bit_reset(PIN_GFCI_CAL_PORT, PIN_GFCI_CAL_PIN);
    } else {
        gpio_bit_set(PIN_GFCI_CAL_PORT, PIN_GFCI_CAL_PIN);
    }

    int saw_assert = 0;
    for (uint32_t t = 0; t < GFCI_CAL_PULSE_MS;
         t += GFCI_CAL_POLL_INTERVAL_MS) {
        busy_wait_us(GFCI_CAL_POLL_INTERVAL_MS * 1000U);
        if (gfci_fault_active()) { saw_assert = 1; break; }
    }

    /* Restore CAL to idle regardless of result. */
    if (idle_level) {
        gpio_bit_set(PIN_GFCI_CAL_PORT, PIN_GFCI_CAL_PIN);
    } else {
        gpio_bit_reset(PIN_GFCI_CAL_PORT, PIN_GFCI_CAL_PIN);
    }

    if (!saw_assert) return -1;

    /* Wait for PE2 to settle back to idle HIGH. */
    for (uint32_t t = 0; t < GFCI_CAL_RECOVER_MS;
         t += GFCI_CAL_POLL_INTERVAL_MS) {
        busy_wait_us(GFCI_CAL_POLL_INTERVAL_MS * 1000U);
        if (!gfci_fault_active()) return 0;
    }
    return -2;
}
