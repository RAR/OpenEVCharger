#ifndef OPENBHZD_TASKS_COMMS_TASK_H
#define OPENBHZD_TASKS_COMMS_TASK_H

#include "FreeRTOS.h"
#include "task.h"

#define COMMS_TASK_STACK_WORDS  192U
#define COMMS_TASK_PRIORITY     2U

void comms_task_create(void);

#endif
