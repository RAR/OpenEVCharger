#ifndef OPENBHZD_TASKS_SAFETY_TASK_H
#define OPENBHZD_TASKS_SAFETY_TASK_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

/* Bench survey 2026-05-04: peak ~122 W used / 256 W → 134 W free, ~50%
 * headroom. Borderline tight on the safety-critical path under fault-
 * flood (raise + persist post + comms publish in one tick). Bumped to
 * 320 W for ~2× margin; cheap (256 B) versus the cost of a stack-overflow
 * trip in the supervisor. */
#define SAFETY_TASK_STACK_WORDS  320U
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

/* Arm one-shot learn-mode: the next card-present RFID swipe gets added
 * to the persisted authorized list (instead of going through the
 * lookup → start/stop dispatch). Disarms automatically once a UID is
 * captured, or after the timeout below. */
int safety_request_rfid_learn(void);

/* Ask safety_task to publish a fresh EVT_RFID_CONFIG (require_auth +
 * session_authorized). Used by comms after CMD_GET_RFID_CONFIG and
 * after the persist task lands a CMD_SET_REQUIRE_RFID_AUTH write. */
int safety_request_publish_rfid_config(void);

#endif
