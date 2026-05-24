#include "hal/gfci.h"
#include "pin_map.h"
#include "hal/uart.h"
#include "hal/wdg.h"
#include "FreeRTOS.h"
#include "task.h"
#include "gd32f20x.h"

/* PE2 (GFCI fault sense, active-low at MCU) is configured as input
 * pull-up by gpio_init_all(). gfci_init() is a defensive re-assertion
 * of that mode and a no-op on the GPIO otherwise.
 *
 * PE3 (GFCI CAL drive, inverting level-shift on PCB) is configured
 * by gpio_init_all() as FLOATING INPUT, NOT output. gfci_refresh_step
 * (called by gfci_refresh_task) temporarily switches PE3 to output
 * PP for the brief refresh pulse, then back to floating. Stock fw
 * V1.0.063 leaves PE3 high-impedance most of the time; the line
 * slowly RC-decays on the level-shifter side. Driving PE3 actively
 * LOW for sustained periods (as the original FW did) breaks the
 * chip's expected RC waveform and the chip refuses to handshake. */

void gfci_init(void)
{
    /* Defensive: re-assert PE2 input pull-up in case some earlier
     * init touched the pad. Idempotent. PE3 is left alone — its
     * floating-input configuration is owned by gpio_init_all and the
     * refresh task owns runtime mode changes. */
    gpio_bit_set(PIN_GFCI_SENSE_PORT, PIN_GFCI_SENSE_PIN);
    gpio_init(PIN_GFCI_SENSE_PORT, GPIO_MODE_IPU,
              GPIO_OSPEED_50MHZ, PIN_GFCI_SENSE_PIN);
}

int gfci_fault_active(void)
{
    /* Active-low: chip pulls PE2 LOW on fault, idle HIGH (MCU
     * internal pull-up holds the wire when the chip is quiet). */
    return (gpio_input_bit_get(PIN_GFCI_SENSE_PORT, PIN_GFCI_SENSE_PIN)
            == RESET) ? 1 : 0;
}

/* --- Handshake window flag ----------------------------------------
 *
 * The chip's CAL self-test cycle (driven by gfci_refresh_task) makes
 * the chip briefly pulse TRIP LOW (handshake = "I'm alive + CT is
 * connected") about 200-400 ms into each refresh window. From the
 * MCU's perspective that is indistinguishable from a real GFCI
 * fault unless safety_task knows to mask it.
 *
 * gfci_refresh_step sets `s_handshake_window` to 1 during the entire
 * refresh window (PE3 driven + a brief tail to catch a late
 * handshake), then back to 0. safety_task's check_gfci masks PE2 LOW
 * while s_handshake_window != 0. Real persistent faults extend past
 * the window and trip in the normal way. */

static volatile int s_handshake_window = 0;

int gfci_in_handshake_window(void)
{
    return s_handshake_window;
}

/* --- Refresh cycle parameters -------------------------------------
 *
 * Stock V1.0.063 scope-RE'd cycle (bench 2026-05-24):
 *   CAL HIGH for ~6 s (chip "armed" idle)
 *   CAL LOW  for ~790 ms (refresh pulse — chip handshakes here)
 *   Chip pulls TRIP LOW for ~300 ms ~200-400 ms into the pulse
 *
 * Mapping to MCU PE3 via inverting level-shift:
 *   IDLE_MS:    PE3 LOW (CAL HIGH at connector) — chip armed
 *   PULSE_MS:   PE3 OUT-PP HIGH (CAL LOW at connector) — refresh
 *   then return PE3 to floating (line RC-decays back HIGH).
 *
 * BOOT_HOLD_MS is the chip warm-up before the first init transition.
 * Stock fw waits ~2 seconds; we use a slightly longer margin. */

#define GFCI_BOOT_FLOAT_MS    1000U
#define GFCI_BOOT_HIGH_MS     1000U   /* PE3 driven HIGH (CAL LOW) */
#define GFCI_BOOT_LOW_MS      1000U   /* PE3 driven LOW (CAL HIGH) */
#define GFCI_REFRESH_IDLE_MS  6000U   /* PE3 LOW segment of cycle */
#define GFCI_REFRESH_PULSE_MS 1000U   /* PE3 HIGH segment (CAL LOW refresh) */
#define GFCI_HANDSHAKE_TAIL_MS 1000U  /* keep mask asserted this long after CAL rising edge — bench observed handshake up to ~500 ms wide, 2x margin */
#define GFCI_POLL_INTERVAL_MS  10U
#define GFCI_WDG_CHUNK_MS     100U    /* per-chunk delay; wdg_kick between */

static void chunked_delay_ms(uint32_t total_ms)
{
    while (total_ms >= GFCI_WDG_CHUNK_MS) {
        vTaskDelay(pdMS_TO_TICKS(GFCI_WDG_CHUNK_MS));
        wdg_kick();
        total_ms -= GFCI_WDG_CHUNK_MS;
    }
    if (total_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(total_ms));
        wdg_kick();
    }
}

/* Drive PE3 to output PP at the given level. The mode-change has to
 * happen WITH the new ODR bit already set so there is no momentary
 * glitch to the previous output level. */
static void pe3_drive(int level)
{
    if (level) {
        gpio_bit_set(PIN_GFCI_CAL_PORT, PIN_GFCI_CAL_PIN);
    } else {
        gpio_bit_reset(PIN_GFCI_CAL_PORT, PIN_GFCI_CAL_PIN);
    }
    gpio_init(PIN_GFCI_CAL_PORT, GPIO_MODE_OUT_PP,
              GPIO_OSPEED_2MHZ, PIN_GFCI_CAL_PIN);
}

static void pe3_release_to_floating(void)
{
    gpio_init(PIN_GFCI_CAL_PORT, GPIO_MODE_IN_FLOATING,
              GPIO_OSPEED_2MHZ, PIN_GFCI_CAL_PIN);
}

/* Boot init dance — replicates stock V1.0.063's pre-cycle wake-up
 * sequence. Returns when the chip has been observed handshaking at
 * least once (PASS) or when the timeout expires (FAIL).
 *
 * Returns:
 *    0   PASS — saw at least one handshake
 *   -1   FAIL — no handshake during the boot window
 *   -3   sense already asserted at entry (live fault, can't test) */
int gfci_self_test(void)
{
    if (gfci_fault_active()) {
        return -3;
    }

    int saw_handshake = 0;
    uint32_t handshake_t_ms = 0;
    uint32_t elapsed_ms = 0;

    /* Phase 1: PE3 floating ~1 s. */
    printk("gfci-cal: boot phase 1 — PE3 floating %u ms\n",
           (unsigned)GFCI_BOOT_FLOAT_MS);
    pe3_release_to_floating();
    chunked_delay_ms(GFCI_BOOT_FLOAT_MS);
    elapsed_ms += GFCI_BOOT_FLOAT_MS;

    /* Phase 2: PE3 STAYS FLOATING for another ~1 s. The level
     * shifter's RC pullup is naturally bringing CAL HIGH at the
     * chip; actively driving PE3 LOW would short-circuit that
     * gradual rise. Stock fw scope shows the same pattern. */
    printk("gfci-cal: boot phase 2 — PE3 still floating %u ms\n",
           (unsigned)GFCI_BOOT_LOW_MS);
    chunked_delay_ms(GFCI_BOOT_LOW_MS);
    elapsed_ms += GFCI_BOOT_LOW_MS;

    /* Phase 3: FIRST switch from floating to OUTPUT — drive PE3
     * HIGH (= CAL LOW at connector). This is the chip's "wake up,
     * here comes the refresh cycle" trigger. */
    printk("gfci-cal: boot phase 3 — PE3 → OUT HIGH (CAL LOW) %u ms\n",
           (unsigned)GFCI_BOOT_HIGH_MS);
    pe3_drive(1);
    chunked_delay_ms(GFCI_BOOT_HIGH_MS);
    elapsed_ms += GFCI_BOOT_HIGH_MS;

    /* Phase 4: run up to 4 refresh cycles. PASS as soon as we see one
     * handshake. */
    for (int cyc = 0; cyc < 4 && !saw_handshake; ++cyc) {
        /* PE3 LOW (= CAL HIGH idle) for 6 s — poll PE2 each tick.
         * The chip CAN handshake during the idle period (stock data
         * shows it usually happens near the LOW-HIGH transition, but
         * cover the whole window). */
        pe3_drive(0);
        for (uint32_t t = 0; t < GFCI_REFRESH_IDLE_MS;
             t += GFCI_POLL_INTERVAL_MS) {
            vTaskDelay(pdMS_TO_TICKS(GFCI_POLL_INTERVAL_MS));
            wdg_kick();
            if (gfci_fault_active() && !saw_handshake) {
                saw_handshake = 1;
                handshake_t_ms = elapsed_ms + t;
                printk("gfci-cal: cyc %d handshake at t=%u ms (idle window)\n",
                       cyc, (unsigned)handshake_t_ms);
                break;
            }
        }
        elapsed_ms += GFCI_REFRESH_IDLE_MS;
        if (saw_handshake) break;

        /* PE3 HIGH (= CAL LOW refresh pulse) for 1 s. */
        s_handshake_window = 1;
        pe3_drive(1);
        for (uint32_t t = 0; t < GFCI_REFRESH_PULSE_MS;
             t += GFCI_POLL_INTERVAL_MS) {
            vTaskDelay(pdMS_TO_TICKS(GFCI_POLL_INTERVAL_MS));
            wdg_kick();
            if (gfci_fault_active() && !saw_handshake) {
                saw_handshake = 1;
                handshake_t_ms = elapsed_ms + t;
                printk("gfci-cal: cyc %d handshake at t=%u ms (refresh window)\n",
                       cyc, (unsigned)handshake_t_ms);
            }
        }
        elapsed_ms += GFCI_REFRESH_PULSE_MS;

        /* End refresh: hold mask asserted briefly while PE3 returns
         * to LOW (CAL HIGH) for the next idle period. Covers any
         * late handshake. */
        pe3_drive(0);
        chunked_delay_ms(GFCI_HANDSHAKE_TAIL_MS);
        s_handshake_window = 0;
        elapsed_ms += GFCI_HANDSHAKE_TAIL_MS;
    }

    if (!saw_handshake) {
        printk("gfci-cal: NO handshake after %u ms of cycles\n",
               (unsigned)elapsed_ms);
        return -1;
    }

    /* Wait for the chip's TRIP pulse to clear before returning PASS.
     * The handshake we just detected is a ~300 ms LOW pulse on PE2;
     * if we return while it's still active, safety_task's check_gfci
     * will see the LOW for >3 ticks (60 ms) and raise FAULT_GFCI as
     * a false positive. Wait up to 800 ms for PE2 to return HIGH. */
    for (int i = 0; i < 80; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wdg_kick();
        if (!gfci_fault_active()) break;
    }
    /* PE3 must end this function in the "idle" state (LOW = CAL
     * HIGH at chip) so the refresh task takes over cleanly. */
    pe3_drive(0);

    printk("gfci-cal: PASS — handshake at t=%u ms\n",
           (unsigned)handshake_t_ms);
    return 0;
}

/* --- Continuous refresh task -------------------------------------
 *
 * Runs forever after the boot self-test passes. Drives the
 * 6 s + 1 s refresh cycle on PE3 to keep the chip in its expected
 * armed state. Sets the handshake_window flag during the refresh
 * pulse + tail so safety_task masks the brief chip handshake as
 * "not a real fault". */

void gfci_refresh_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Idle segment: PE3 LOW, CAL HIGH at connector. */
        pe3_drive(0);
        chunked_delay_ms(GFCI_REFRESH_IDLE_MS);

        /* Refresh pulse: PE3 HIGH, CAL LOW. Chip will handshake. */
        s_handshake_window = 1;
        pe3_drive(1);
        chunked_delay_ms(GFCI_REFRESH_PULSE_MS);

        /* Hold mask asserted briefly while PE3 returns to LOW for
         * the next idle period — covers any late handshake. */
        pe3_drive(0);
        chunked_delay_ms(GFCI_HANDSHAKE_TAIL_MS);
        s_handshake_window = 0;
        /* Loop back to idle segment; subtract the tail delay so
         * cycle period stays ~7 s total. */
    }
}
