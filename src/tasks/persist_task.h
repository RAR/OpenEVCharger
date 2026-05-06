#ifndef OPENEVCHARGER_TASKS_PERSIST_TASK_H
#define OPENEVCHARGER_TASKS_PERSIST_TASK_H

#include "FreeRTOS.h"
#include "task.h"
#include "../persist/event_log.h"
#include "../persist/session_log.h"

/* Survey 2026-05-04: previous "374/384 high-water" reading was misread —
 * stack_watch dump prints free / configured (uxTaskGetStackHighWaterMark
 * returns FREE words), so 374 was free, not used → ~10 W actually used.
 * The old 384 W allocation already had ~95% margin; the bump to 512 was
 * unnecessary. Holding 512 W for now though — GET_FAULT_LOG path runs
 * comms_publish_event_seq from inside this task with a 63 B TLV frame
 * buffer on stack per record (up to 32 records); next bench fault-flood
 * survey will surface the real worst case via the [NEW PEAK] tag. */
#define PERSIST_TASK_STACK_WORDS  512U
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

/* Toggle the require-RFID-auth flag and persist. Once stored,
 * persist_task asks safety_task to publish an EVT_RFID_CONFIG so HA
 * sees the live state. */
int persist_post_require_rfid_auth(uint8_t enable);

/* Persist new CP calibration values. Same single-owner rationale. */
int persist_post_calibration(int16_t anchor_raw,
                             int16_t slope_num,
                             int16_t slope_den);

/* Persist BL0939 chassis scales (V/IA/IB/PA per raw). Single-owner via
 * persist_task; calibration_set_bl0939 talks to W25Q ping-pong. */
int persist_post_bl0939_cal(int16_t v_uv_per_raw,
                            int16_t ia_ua_per_raw,
                            int16_t ib_ua_per_raw,
                            int16_t pa_uw_per_raw);

/* Read up to `max_count` most-recent fault records from event_log and
 * publish them as EVT_FAULT_LOG_ENTRY frames (one per record), tagged
 * with the supplied response `seq`. Followed by a single EVT_FAULT_LOG_END
 * frame. Routed through persist_task so the SPI3 reads happen in the
 * single-owner task. */
int persist_post_get_fault_log(uint8_t max_count, uint8_t seq);

/* Sum mwh_delivered across all valid session_log records and publish
 * as a single EVT_LIFETIME_KWH frame (u32 mWh, response `seq`). */
int persist_post_get_lifetime_kwh(uint8_t seq);

/* RFID authorized-list management. add/remove are routed through
 * persist_task to keep SPI3 single-owner. clear empties the list.
 * get_list walks the in-RAM cache and emits the EVT_RFID_LIST_ENTRY /
 * EVT_RFID_LIST_END frames carrying response `seq`. */
int persist_post_rfid_authlist_add(uint32_t uid);
int persist_post_rfid_authlist_remove(uint32_t uid);
int persist_post_rfid_authlist_clear(void);
int persist_post_rfid_authlist_get_list(uint8_t seq);

/* OTA chunked upload — see commands.h. All three calls are non-blocking;
 * persist_task owns SPI3 + the OTA session state machine and emits the
 * matching EVT_OTA_*_ACK on its own thread. seq carries the FC41D's TLV
 * sequence number so the response pairs with the request.
 *
 * persist_post_ota_chunk: data may be up to TLV_PAYLOAD_MAX - 8 = 48 B
 * (the BEGIN/CHUNK headers already eat 8 B for session_id+offset). */
int persist_post_ota_begin(uint32_t image_size,
                           uint32_t image_crc32,
                           uint32_t session_id,
                           uint8_t  seq);
int persist_post_ota_chunk(uint32_t session_id,
                           uint32_t offset,
                           const uint8_t *data,
                           uint8_t  data_len,
                           uint8_t  seq);
int persist_post_ota_commit(uint32_t session_id, uint8_t seq);
int persist_post_ota_abort(uint32_t session_id, uint8_t seq);

#endif
