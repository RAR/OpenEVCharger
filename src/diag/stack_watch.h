/* Stack high-water survey for FreeRTOS tasks.
 *
 * Each task_create() registers its handle here at boot; io_task calls
 * stack_watch_dump() periodically. Output line:
 *   stack: <name>=<used>/<configured_words>  (over each registered task)
 *
 * "used" is derived: configured_words - uxTaskGetStackHighWaterMark()
 * (the FreeRTOS API returns FREE words). New worst-ever low-water-marks
 * also fire a [NEW PEAK ...] tag inline so deep fault-path stack reaches
 * surface in the log between dump cadences.
 *
 * Build-flag gated via OPENBHZD_STACK_WATCH. When the flag is 0 (default
 * for production) every entry point is a no-op so there is zero ROM/RAM
 * cost. The flag is on for bench builds so we can sample headroom and
 * trim configured stacks back to a sane budget. */

#ifndef OPENBHZD_DIAG_STACK_WATCH_H
#define OPENBHZD_DIAG_STACK_WATCH_H

#include "FreeRTOS.h"
#include "task.h"

#ifndef OPENBHZD_STACK_WATCH
#define OPENBHZD_STACK_WATCH 0
#endif

#if OPENBHZD_STACK_WATCH

void stack_watch_register(const char *name, TaskHandle_t handle,
                          uint16_t configured_words);
void stack_watch_dump(void);

#else

static inline void stack_watch_register(const char *name, TaskHandle_t handle,
                                        uint16_t configured_words)
{
    (void)name; (void)handle; (void)configured_words;
}
static inline void stack_watch_dump(void) { }

#endif

#endif
