#ifndef OPENBHZD_TASKS_COMMS_TASK_H
#define OPENBHZD_TASKS_COMMS_TASK_H

#include <stddef.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

#define COMMS_TASK_STACK_WORDS  512U   /* Survey 2026-05-04: stack_watch_dump
                                        * format had been misread (lo = FREE
                                        * words, not used). Real high-water-
                                        * mark on bench: ~10 W used out of
                                        * 1024. accum(64 B) + send_frame buf
                                        * (64 B) + handler locals + nested
                                        * call frames + FreeRTOS context
                                        * save fits comfortably under 256 W.
                                        * 512 W keeps a 50× margin while
                                        * recovering 2 KB RAM. Bumped from
                                        * 384 originally on the same misread
                                        * that flagged "tight" headroom. */
#define COMMS_TASK_PRIORITY     2U

void comms_task_create(void);

/* Publish an unsolicited event over the FC41D TLV link. cmd should be
 * one of the EVT_* constants in proto/commands.h. payload may be NULL.
 * Returns 0 on success, <0 if frame can't be built (oversized payload).
 * Safe to call from any task; serialised by an internal mutex.
 *
 * Currently emits the frame synchronously over UART4 (~5 ms at 64 B).
 * If that becomes too long-running for safety_task we'll move to a TX
 * queue + worker. */
int comms_publish_event(uint8_t cmd, const void *payload, size_t payload_len);

/* Same, but tagged with a non-zero seq so it pairs with an outstanding
 * request (used for response chains like GET_FAULT_LOG). */
int comms_publish_event_seq(uint8_t cmd, uint8_t seq,
                            const void *payload, size_t payload_len);

#endif
