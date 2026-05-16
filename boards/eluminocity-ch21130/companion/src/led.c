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

int led_decide(unsigned char user_state,
               unsigned char red_led,
               unsigned char pri_state,
               unsigned char ovr_0a71,
               unsigned char ovr_0a72,
               enum led_action *green_out,
               enum led_action *red_out,
               enum led_action *green2_out)
{
    /* Override paths from stock (docs/19 §LED_control "main loop"):
     *   shmem[0x0a72] || shmem[0x0a71]   → firmware-update path
     *   shmem[0x0a07] == 5               → fault pattern */
    if (ovr_0a71 != 0 || ovr_0a72 != 0) {
        /* "firmware-update": both green LEDs solid, red off. */
        *green_out  = LED_SOLID;
        *red_out    = LED_OFF;
        *green2_out = LED_SOLID;
        return 1;
    }
    if (pri_state == 5) {
        /* "fault": red flashing, green off. */
        *green_out  = LED_OFF;
        *red_out    = LED_FLASH;
        *green2_out = LED_OFF;
        return 1;
    }
    /* Normal mapping. Stock encoding (docs/14 + decode_sharemem.py):
     *   USER_STATE: 0=idle (green off), 1=auth/ready (green solid),
     *               2=charging (green flash)
     *   RED_LED:    0=off, 1=solid (alarm), 2=flash (alarm w/ attention) */
    *green_out  = (user_state == 2) ? LED_FLASH :
                  (user_state == 1) ? LED_SOLID : LED_OFF;
    *red_out    = (red_led == 2) ? LED_FLASH :
                  (red_led == 1) ? LED_SOLID : LED_OFF;
    /* Green2 — we don't replicate stock's Wi-Fi indicator yet (docs/19
     * §"What we DON'T know" §Green2_Wifi). Hold steady-off; can wire
     * to a delta-bridge state (MQTT-connected, etc.) later. */
    *green2_out = LED_OFF;
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

static void gpio_write(int gpio_num, int value)
{
    char buf[128];
    snprintf(buf, sizeof buf,
             "echo %d > /sys/class/gpio/gpio%d/value 2>/dev/null",
             value ? 1 : 0, gpio_num);
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
        unsigned char user_state = shmem_u8(&sm, 0x0a00);
        unsigned char red_led    = shmem_u8(&sm, 0x0a01);
        unsigned char pri_state  = shmem_u8(&sm, 0x0a07);
        unsigned char ovr_71     = shmem_u8(&sm, 0x0a71);
        unsigned char ovr_72     = shmem_u8(&sm, 0x0a72);

        enum led_action a_g, a_r, a_g2;
        led_decide(user_state, red_led, pri_state, ovr_71, ovr_72,
                   &a_g, &a_r, &a_g2);

        long now = monotonic_ms();
        led_apply(55, &st_g,  a_g,  now);
        led_apply(56, &st_r,  a_r,  now);
        led_apply(57, &st_g2, a_g2, now);

        sleep_ms_stop(POLL_MS, stop);
    }

    fprintf(stderr, "led: stopping\n");
    /* On exit, leave LEDs in current state — stock LED_control (if
     * restarting) will take over. */
    shmem_release(&sm);
    return 0;
}
