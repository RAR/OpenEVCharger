/* led — replacement personality for stock /root/LED_control.
 *
 * Drives 3 LED outputs via sysfs. Mapping comes from disassembling
 * stock /Storage/stk/LED_control (debug symbols present); the
 * disassembly's function names are accurate and match physical colors:
 *
 *   GPIO 55 = BOTTOM red LED ("fault")    (stock `Red_IOCtrl`)
 *   GPIO 56 = MIDDLE green LED ("charge") (stock `Green_IOCtrl`)
 *   GPIO 57 = TOP green2 LED ("power"/Wi-Fi)  (stock `Green2_IOCtrl`)
 *   GPIO 82 = INPUT  (Powerdtect — read-only)
 *
 * All three are ACTIVE-HIGH: writing '1' to /sys/class/gpio/gpioNN/value
 * turns the LED ON. See `led_sysfs_byte_for()` in led.c.
 *
 * Stock state-machine (re-decoded 2026-05-16 from main() at 0x8b74):
 *
 *   if shmem[0x0a71] != 0:                      "firmware update" path
 *     - drives a 5-sec debounce + sleep pattern using
 *       shmem[0x0a72] as a SELF-MANAGED internal flag
 *     - net visual = TOP + MIDDLE solid, BOTTOM off
 *   elif shmem[0x0a07] == 5:                    fault path
 *     - stock only resets its internal "is currently on/off" trackers;
 *       does not actively change any GPIO. Effectively a no-op until
 *       next iteration re-issues the normal shmem state.
 *   else:
 *     shmem[0x0a00] USER_STATE     → Green_IOCtrl  → MIDDLE green
 *       0 = off, 1 = solid, 2 = flash (0.5 Hz)
 *     shmem[0x0a01] RED_LED        → Red_IOCtrl    → BOTTOM red
 *       0 = off, 1 = solid, 2 = flash
 *     shmem[0x0a17] GREEN2_STATE   → Green2_IOCtrl → TOP green2
 *       0 = off, 1 = solid, 2 = flash, 3 = Green2_Wifi() pattern
 *
 * Per-LED state-change tracking: stock only re-writes the GPIO when the
 * desired action *changes*, so steady-state operation is system()-free.
 * Mirrored in our `led_apply()` with a per-LED `last_action` cache.
 *
 * Output is via `system("echo {0,1} > /sys/class/gpio/gpioXX/value")` —
 * same mechanism as stock. Slower than a /dev/mem write but matches the
 * existing GPIO-export setup that stock does at boot (we still re-export
 * defensively in case we're started before stock).
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
 * Output names match the stock disassembly's `*_IOCtrl` symbols and the
 * physical color mapping in this header's top comment:
 *   `green_out`  → MIDDLE green LED (gpio56)
 *   `red_out`    → BOTTOM red   LED (gpio55)
 *   `green2_out` → TOP green2   LED (gpio57)
 * Returns 1 if the firmware-update override path is active (suppresses
 * normal state mapping), 0 otherwise.
 *
 * `fw_update` is the simplified gate for stock's `shmem[0x0a71] != 0`
 * firmware path — we don't replicate stock's 5-sec debounce / self-
 * managed `shmem[0x0a72]` flag here. */
int led_decide(unsigned char user_state,
               unsigned char red_led,
               unsigned char green2_state,
               unsigned char fw_update,
               enum led_action *green_out,
               enum led_action *red_out,
               enum led_action *green2_out);

/* Translate a logical LED state ("on"/"off") to the sysfs byte we write
 * to /sys/class/gpio/gpioNN/value. LEDs on this board are wired
 * ACTIVE-HIGH (bench-confirmed 2026-05-16 by controlled per-GPIO
 * toggle): logical_on=1 → byte 1, logical_on=0 → byte 0. Pure identity
 * for nonzero/zero; exposed for host tests so a future cleanup can't
 * silently flip the polarity. */
int led_sysfs_byte_for(int logical_on);

#endif /* LED_H */
