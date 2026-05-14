/* boards/nexcyber/hal/led_ring.h — LED ring driver.
 *
 * Two-channel LED indicator on the Nexcyber bench unit (red driver
 * is hardware-damaged on this unit; see pin_map.h for the topology
 * and bench notes).
 *
 *   PC10 = BLUE  — used here for "standby / ready / waiting"
 *   PC12 = GREEN — used here for "charging"
 *
 * Active-HIGH via NPN/MOSFET buffer to the 12 V LED rail.
 */
#pragma once

#include <stdbool.h>

void led_ring_init(void);

void led_blue_set(bool on);
void led_green_set(bool on);
