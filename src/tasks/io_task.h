#ifndef OPENEVCHARGER_TASKS_IO_TASK_H
#define OPENEVCHARGER_TASKS_IO_TASK_H

#include "FreeRTOS.h"
#include "task.h"

/* Survey 2026-05-04: peak ~114 W used / 512 W → 398 W free, ~78% margin.
 * Holding 512 W until LED render + buzzer + state-feedback path is
 * stressed under a real charging session (M7/M10); shrink-with-margin
 * candidate is 384 W. */
#define IO_TASK_STACK_WORDS  512U
#define IO_TASK_PRIORITY     3U

void io_task_create(void);

#endif
