#define _GNU_SOURCE

/* led — see led.h.
 *
 * Reuses stock's "fire system() to echo to sysfs" approach. Each LED
 * state-cache (`last_*`) prevents re-issuing the same value, so we
 * only shell out when the state actually changes — keeps system()
 * overhead bounded even at high poll rates. */
#include "led.h"

#include "shmem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Toggle every TOGGLE_MS milliseconds for flash mode. Stock = 1000 ms
 * (0.5 Hz, 50% duty) — see docs/19 §LED_control §"How it actually
 * drives the LEDs". */
#define TOGGLE_MS 1000

/* Poll shmem every POLL_MS — cheap when nothing changes (only memcmp
 * + integer compares). */
#define POLL_MS    250

/* ============================================================
 * Decision logic — pure function of shmem inputs.
 * ============================================================ */

/* Map a stock 0/1/2 state byte to an led_action. Codes 3+ are stock-
 * specific (Green2_State==3 invokes Green2_Wifi() which writes a custom
 * blink pattern); for our purposes we treat anything >=3 as solid so a
 * mistaken value doesn't go dark. */
static enum led_action action_for_state(unsigned char s)
{
    switch (s) {
    case 0:  return LED_OFF;
    case 1:  return LED_SOLID;
    case 2:  return LED_FLASH;
    default: return LED_SOLID;
    }
}

int led_decide(unsigned char user_state,
               unsigned char red_led,
               unsigned char green2_state,
               unsigned char fw_update,
               enum led_action *green_out,
               enum led_action *red_out,
               enum led_action *green2_out)
{
    /* Firmware-update override. Stock (docs/19 §LED_control "firmware
     * path") runs a 5-sec debounce + sleep pattern on shmem[0x0a71]
     * and uses shmem[0x0a72] as a self-managed internal flag. We
     * replicate only the visual: TOP green2 + MIDDLE green solid,
     * BOTTOM red off — which is what stock's `Green2_IOCtrl(1)` +
     * `Green_IOCtrl(1)` calls produce inside the path. */
    if (fw_update != 0) {
        *green_out  = LED_SOLID;
        *red_out    = LED_OFF;
        *green2_out = LED_SOLID;
        return 1;
    }
    /* Normal mapping — each shmem state byte drives one LED:
     *   shmem[0x0a00] USER_STATE   → green_out  → MIDDLE green
     *   shmem[0x0a01] RED_LED      → red_out    → BOTTOM red
     *   shmem[0x0a17] GREEN2_STATE → green2_out → TOP green2 */
    *green_out  = action_for_state(user_state);
    *red_out    = action_for_state(red_led);
    *green2_out = action_for_state(green2_state);
    return 0;
}

/* ============================================================
 * GPIO actuation
 * ============================================================
 * stock LED_control does:
 *   sprintf(buf, "echo %c > /sys/class/gpio/gpio55/value", '0' or '1');
 *   system(buf);
 * We do the same — slow but matches stock's behavior and works under
 * the same kernel setup. */

static void gpio_setup_one(int gpio_num, const char *dir)
{
    char buf[128];
    /* Export — may already be exported (returns -EBUSY); harmless. */
    snprintf(buf, sizeof buf, "echo %d > /sys/class/gpio/export 2>/dev/null",
             gpio_num);
    (void)!system(buf);     /* return value ignored intentionally */
    snprintf(buf, sizeof buf,
             "echo \"%s\" > /sys/class/gpio/gpio%d/direction 2>/dev/null",
             dir, gpio_num);
    (void)!system(buf);     /* return value ignored intentionally */
}

/* LED polarity — ACTIVE-HIGH on this PCB.
 *
 * Bench experiment 2026-05-16 (controlled per-GPIO toggle with the led
 * personality stopped, user reporting which physical LED lit): writing
 * '1' to /sys/class/gpio/gpioNN/value drives the corresponding LED ON,
 * '0' drives it OFF. All three are wired the same way. This matches
 * stock's bytes — stock's `Green_IOCtrl(1)` writes byte 1 which lights
 * its LED. M11.1's "active-low fix" was based on a misread of the
 * symptom (see docs/19 §"polarity history") and is reverted here.
 *
 * The pure helper is kept so the polarity stays test-pinned. */
int led_sysfs_byte_for(int logical_on)
{
    return logical_on ? 1 : 0;
}

static void gpio_write(int gpio_num, int logical_on)
{
    char buf[128];
    snprintf(buf, sizeof buf,
             "echo %d > /sys/class/gpio/gpio%d/value 2>/dev/null",
             led_sysfs_byte_for(logical_on), gpio_num);
    (void)!system(buf);     /* return value ignored intentionally */
}

/* ============================================================
 * Per-LED state tracker
 * ============================================================
 * Tracks (last_action, last_phys_value, last_toggle_ms) so we only
 * shell out when state actually changes, and flash phases are timed
 * coherently across LEDs (each has its own toggle timer). */
struct led_state {
    enum led_action last_action;
    int last_phys;                 /* 0/1 last written */
    long last_toggle_ms;           /* monotonic ms */
};

static long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void led_apply(int gpio_num,
                      struct led_state *st,
                      enum led_action want,
                      long now_ms)
{
    /* OFF: write 0 once on transition, never again. */
    if (want == LED_OFF) {
        if (st->last_action != LED_OFF || st->last_phys != 0) {
            gpio_write(gpio_num, 0);
            st->last_action = LED_OFF;
            st->last_phys   = 0;
        }
        return;
    }
    /* SOLID: write 1 once on transition, never again. */
    if (want == LED_SOLID) {
        if (st->last_action != LED_SOLID || st->last_phys != 1) {
            gpio_write(gpio_num, 1);
            st->last_action = LED_SOLID;
            st->last_phys   = 1;
        }
        return;
    }
    /* FLASH: toggle every TOGGLE_MS. */
    if (st->last_action != LED_FLASH) {
        /* Entering flash mode — start ON, set toggle timer. */
        gpio_write(gpio_num, 1);
        st->last_action  = LED_FLASH;
        st->last_phys    = 1;
        st->last_toggle_ms = now_ms;
        return;
    }
    if (now_ms - st->last_toggle_ms >= TOGGLE_MS) {
        st->last_phys = !st->last_phys;
        gpio_write(gpio_num, st->last_phys);
        st->last_toggle_ms = now_ms;
    }
}

/* ============================================================
 * Main loop
 * ============================================================ */

static void sleep_ms_stop(int ms, volatile int *stop)
{
    struct timespec ts;
    while (ms > 0 && !(*stop)) {
        int step = ms > 100 ? 100 : ms;
        ts.tv_sec  = step / 1000;
        ts.tv_nsec = (step % 1000) * 1000000L;
        nanosleep(&ts, NULL);
        ms -= step;
    }
}

int led_personality_run(volatile int *stop)
{
    fprintf(stderr, "led: starting\n");

    /* Defensive setup — stock LED_control might not have run, or
     * we might be running standalone. Best-effort; harmless if
     * already exported. */
    gpio_setup_one(55, "out");
    gpio_setup_one(56, "out");
    gpio_setup_one(57, "out");
    gpio_setup_one(82, "in");    /* Powerdtect input — read-only by stock */

    /* shmem attach RO (we never write LED state — that comes from
     * other daemons). */
    struct shmem sm;
    memset(&sm, 0, sizeof sm);
    sm.shmid = -1;
    int bo_ms = 200;
    while (!(*stop)) {
        if (shmem_attach(&sm) == 0)
            break;
        fprintf(stderr, "led: shmem not ready, retry in %d ms\n", bo_ms);
        sleep_ms_stop(bo_ms, stop);
        if (bo_ms < 5000) bo_ms *= 2;
    }
    if (*stop)
        return 0;

    struct led_state st_g  = { LED_OFF, 0, 0 };
    struct led_state st_r  = { LED_OFF, 0, 0 };
    struct led_state st_g2 = { LED_OFF, 0, 0 };

    /* Force initial off-write so we converge to known state. */
    gpio_write(55, 0);
    gpio_write(56, 0);
    gpio_write(57, 0);

    while (!(*stop)) {
        unsigned char user_state   = shmem_u8(&sm, 0x0a00);
        unsigned char red_led      = shmem_u8(&sm, 0x0a01);
        unsigned char green2_state = shmem_u8(&sm, 0x0a17);
        /* Only sm[0x0a71] is the producer-driven "firmware update"
         * flag; sm[0x0a72] is stock's self-managed debounce flag and
         * shouldn't be read by us. */
        unsigned char fw_update    = shmem_u8(&sm, 0x0a71);

        enum led_action a_g, a_r, a_g2;
        led_decide(user_state, red_led, green2_state, fw_update,
                   &a_g, &a_r, &a_g2);

        long now = monotonic_ms();
        /* GPIO mapping (verified against stock disassembly + bench
         * per-GPIO toggle): green→gpio56 (middle), red→gpio55
         * (bottom), green2→gpio57 (top). */
        led_apply(56, &st_g,  a_g,  now);
        led_apply(55, &st_r,  a_r,  now);
        led_apply(57, &st_g2, a_g2, now);

        sleep_ms_stop(POLL_MS, stop);
    }

    fprintf(stderr, "led: stopping\n");
    /* On exit, leave LEDs in current state — stock LED_control (if
     * restarting) will take over. */
    shmem_release(&sm);
    return 0;
}
