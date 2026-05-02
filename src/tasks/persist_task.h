#ifndef OPENBHZD_TASKS_PERSIST_TASK_H
#define OPENBHZD_TASKS_PERSIST_TASK_H

#include "FreeRTOS.h"
#include "task.h"

#define PERSIST_TASK_STACK_WORDS  192U
#define PERSIST_TASK_PRIORITY     1U

void persist_task_create(void);

#endif
