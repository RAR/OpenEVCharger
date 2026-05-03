#ifndef OPENBHZD_TASKS_PERSIST_TASK_H
#define OPENBHZD_TASKS_PERSIST_TASK_H

#include "FreeRTOS.h"
#include "task.h"
#include "../persist/event_log.h"
#include "../persist/session_log.h"

#define PERSIST_TASK_STACK_WORDS  256U
#define PERSIST_TASK_PRIORITY     1U
#define PERSIST_QUEUE_DEPTH       8U

/* Create the queue + spawn persist_task. Call from main() before
 * vTaskStartScheduler(). */
void persist_task_create(void);

/* Post an event_record copy onto the persist queue. persist_task
 * consumes the queue and calls event_log_append (which fills boot_count
 * and crc16). Returns 0 on success, -1 if queue full. Non-blocking. */
int persist_post_event(const struct event_record *rec);

/* Same for session_records. */
int persist_post_session(const struct session_record *rec);

/* Post a "I'm alive past 60 s" request. persist_task calls
 * crash_state_reset_alive() (clears fast_restart_count to 0). */
int persist_post_crash_state_reset(void);

#endif
