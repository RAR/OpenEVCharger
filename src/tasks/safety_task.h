#ifndef OPENBHZD_TASKS_SAFETY_TASK_H
#define OPENBHZD_TASKS_SAFETY_TASK_H

#include "FreeRTOS.h"
#include "task.h"

#define SAFETY_TASK_STACK_WORDS  256U
#define SAFETY_TASK_PRIORITY     4U
#define SAFETY_TASK_PERIOD_MS    20U

void safety_task_create(void);

#endif
