#ifndef OPENBHZD_TASKS_IO_TASK_H
#define OPENBHZD_TASKS_IO_TASK_H

#include "FreeRTOS.h"
#include "task.h"

#define IO_TASK_STACK_WORDS  512U
#define IO_TASK_PRIORITY     3U

void io_task_create(void);

#endif
