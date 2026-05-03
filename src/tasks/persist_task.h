#ifndef OPENBHZD_TASKS_PERSIST_TASK_H
#define OPENBHZD_TASKS_PERSIST_TASK_H

#include "FreeRTOS.h"
#include "task.h"
#include "../persist/event_log.h"
#include "../persist/session_log.h"

#define PERSIST_TASK_STACK_WORDS  384U
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

/* Update FC41D-advertised amps cap and ping-pong-write to W25Q.
 * Routed through persist_task to keep SPI3 single-owner per spec § 6. */
int persist_post_boot_config_amps(uint8_t amps);

/* Persist new CP calibration values. Same single-owner rationale. */
int persist_post_calibration(int16_t anchor_raw,
                             int16_t slope_num,
                             int16_t slope_den);

/* Read up to `max_count` most-recent fault records from event_log and
 * publish them as EVT_FAULT_LOG_ENTRY frames (one per record), tagged
 * with the supplied response `seq`. Followed by a single EVT_FAULT_LOG_END
 * frame. Routed through persist_task so the SPI3 reads happen in the
 * single-owner task. */
int persist_post_get_fault_log(uint8_t max_count, uint8_t seq);

/* Sum mwh_delivered across all valid session_log records and publish
 * as a single EVT_LIFETIME_KWH frame (u32 mWh, response `seq`). */
int persist_post_get_lifetime_kwh(uint8_t seq);

#endif
