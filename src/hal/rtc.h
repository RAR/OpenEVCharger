#ifndef OPENBHZD_HAL_RTC_H
#define OPENBHZD_HAL_RTC_H

#include <stdint.h>

/* On-chip GD32F205 RTC peripheral, LSI-clocked (no LSE crystal on this
 * PCB; LSI ≈ 40 kHz, ±5%). The backup domain survives any non-power-cycle
 * reset (NRST, SYSRESETREQ, watchdog, brown-out-recover), so the wall
 * clock survives OTA + watchdog + crash-loop reboots. It is lost on a
 * full mains-off cold boot — VBAT pin isn't wired to a battery on this
 * board.
 *
 * Drift is ±5% over the 30-minute HA resync cadence, i.e. ≤90 s worst
 * case before HA's next push corrects it. For bridging the few-second
 * gap between an OTA reset and the next FC41D time-push, drift is
 * sub-second and irrelevant.
 *
 * The RTC counter holds unix epoch seconds. A 32-bit magic stored
 * across BKP_DATA0+BKP_DATA1 marks the counter as "set this VDD cycle";
 * after a cold boot the magic is wiped and rtc_load_unix() returns 0,
 * which is the same code path as system_time before any HA push. */

/* Initialise APB1 PMU+BKPI clocks and unlock backup-domain writes.
 * If the magic is already valid (a previous boot in this VDD cycle
 * configured the BD), this is a no-op aside from APB enables.
 * Otherwise it does the LSI-on + RTC-clock-src + prescaler dance,
 * leaving the counter at 0 and the magic invalid. */
void rtc_init(void);

/* If the BKP magic is valid, returns 1 and writes the current RTC
 * counter (unix seconds) to *out. Otherwise returns 0; *out untouched. */
int  rtc_load_unix(uint32_t *out);

/* Stores unix_seconds to the RTC counter and stamps the magic so the
 * next boot's rtc_load_unix() picks it up. */
void rtc_store_unix(uint32_t unix_seconds);

#endif
