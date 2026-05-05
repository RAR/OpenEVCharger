#ifndef OPENEVCHARGER_CORE_SYSTEM_TIME_H
#define OPENEVCHARGER_CORE_SYSTEM_TIME_H

#include <stdint.h>

/* Software wall-clock for the MCU. Resolution = 1 second.
 *
 * The MCU has no battery-backed RTC plumbed in this build (VBAT
 * isn't reliably wired on the OEM PCB and the LSE crystal hasn't
 * been characterised), so we keep a software clock that's set over
 * TLV from the FC41D side every time HA's `time` component
 * resyncs. On power loss the value is lost; the next FC41D sync
 * brings it back.
 *
 * Internals: at set-time we record (unix_seconds, tick_ms). At
 * read-time we add (current_tick_ms − base_tick_ms) / 1000 to the
 * stored unix_seconds. Tick subtraction is unsigned so 32-bit
 * wraparound is safe up to ~49.7 days between set-and-read calls
 * (way longer than the FC41D's 30-minute sync cadence).
 *
 * `unix_seconds = 0` means "never set"; system_time_now then
 * returns 0 and is_set returns 0. */
void     system_time_set(uint32_t unix_seconds, uint32_t tick_ms_now);
uint32_t system_time_now(uint32_t tick_ms_now);
int      system_time_is_set(void);

#endif
