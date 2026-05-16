/* led — replacement personality for stock /root/LED_control.
 *
 * Reads four shmem state bytes and drives 3 LED outputs via sysfs:
 *   GPIO 55 = Green        (user-state LED)
 *   GPIO 56 = Red          (fault LED)
 *   GPIO 57 = Green2       (Wi-Fi / secondary)
 *   GPIO 82 = INPUT        (Powerdtect — read-only)
 *
 * State-machine inputs (from docs/14 §3 + docs/19 §LED_control §"state machine"):
 *   shmem[0x0a00] USER_STATE      0=off, 1=solid, 2=flash → Green
 *   shmem[0x0a01] RED_LED         0=off, 1=solid, 2=flash → Red
 *   shmem[0x0a07] PRI_STATE       == 5 → fault override pattern
 *   shmem[0x0a08] PILOT_STATE     informational; not used in basic LED policy
 *   shmem[0x0a71], [0x0a72]       override flags (firmware-update mode)
 *
 * Flash pattern: toggle every 1 second (0.5 Hz, 50% duty) — matches stock.
 *
 * Output is via `system("echo {0,1} > /sys/class/gpio/gpioXX/value")` —
 * same mechanism as stock LED_control. Slower than a /dev/mem write but
 * simpler and matches the existing GPIO-export setup done by stock at
 * boot (we don't re-do export — assume it's already set up).
 *
 * Deployed as a personality of `delta-bridge --personality=led`.
 */
#ifndef LED_H
#define LED_H

/* Run the led personality forever (until stop). Polls shmem every
 * ~250 ms, runs the LED state machine, drives GPIOs.
 *
 * The personality assumes stock-style sysfs GPIO setup is already done
 * (export + direction = out for 55/56/57). On a fresh boot where stock
 * LED_control hasn't run, we re-export defensively in init.
 *
 * Returns 0 on clean shutdown, non-zero on unrecoverable error. */
int led_personality_run(volatile int *stop);

/* --- Lower-level helpers, exposed for host tests --------------- */

enum led_action {
    LED_OFF   = 0,
    LED_SOLID = 1,
    LED_FLASH = 2,
};

/* Decide what each LED should do given the current shmem state bytes.
 * `green_out`, `red_out`, `green2_out` are filled in. Returns 1 if
 * override mode is active (suppresses normal state mapping); 0
 * otherwise. */
int led_decide(unsigned char user_state,
               unsigned char red_led,
               unsigned char pri_state,
               unsigned char ovr_0a71,
               unsigned char ovr_0a72,
               enum led_action *green_out,
               enum led_action *red_out,
               enum led_action *green2_out);

#endif /* LED_H */
