#ifndef OPENBHZD_TASKS_SAFETY_TASK_H
#define OPENBHZD_TASKS_SAFETY_TASK_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

#define SAFETY_TASK_STACK_WORDS  256U
#define SAFETY_TASK_PRIORITY     4U
#define SAFETY_TASK_PERIOD_MS    20U
#define SAFETY_INBOX_DEPTH       4U

void safety_task_create(void);

/* Cross-task control inbox. Producer is comms_task; consumer is
 * safety_task drained at the top of its tick. All non-blocking; drop
 * silently on full queue (UI re-issues). */

/* Clear a latched fault. fault_id = 0 means "all clearable" (every
 * latched fault except GFCI). Has no effect on self-clearing faults. */
int safety_request_clear_fault(uint32_t fault_id);

/* Request EVSE_USER_PAUSED entry. Ignored from FAULT/BOOT/SELF_TEST. */
int safety_request_pause(uint8_t reason);

/* Request EVSE_USER_PAUSED exit. Returns to READY; classifier decides
 * the next transition. */
int safety_request_resume(void);

#endif
