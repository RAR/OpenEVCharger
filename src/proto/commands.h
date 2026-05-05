#ifndef OPENBHZD_PROTO_COMMANDS_H
#define OPENBHZD_PROTO_COMMANDS_H

#include <stdint.h>

/* Command IDs per spec § 5. Requests have bit 7 = 0; responses /
 * unsolicited events have bit 7 = 1. */

/* FC41D → MCU requests */
#define CMD_PING                  0x01u
#define CMD_GET_STATE             0x02u
#define CMD_SET_ADVERTISED_AMPS   0x03u
#define CMD_REQUEST_STOP          0x04u
#define CMD_REQUEST_START_RESUME  0x05u
#define CMD_CLEAR_FAULT           0x06u
#define CMD_GET_FAULT_LOG         0x07u
#define CMD_GET_LIFETIME_KWH      0x08u
#define CMD_WRITE_CALIBRATION     0x09u
#define CMD_SET_LED_OVERRIDE      0x0Au
#define CMD_BUZZER_BEEP           0x0Bu
#define CMD_GET_BUILD_INFO        0x0Cu
#define CMD_GET_DEVICE_ID         0x0Du
#define CMD_WRITE_BL0939_CAL      0x0Eu

/* RFID authorized-tag list management. CMD_RFID_LEARN_NEXT arms a
 * one-shot: the next swiped card UID gets added to the persisted list
 * (instead of being checked for membership). CMD_RFID_REMOVE_UID
 * deletes a specific UID; CMD_RFID_CLEAR_LIST empties the list;
 * CMD_RFID_GET_LIST emits a chain of EVT_RFID_LIST_ENTRY frames + a
 * single EVT_RFID_LIST_END terminator (response seq matches the
 * request). */
#define CMD_RFID_LEARN_NEXT       0x0Fu
#define CMD_RFID_REMOVE_UID       0x10u   /* payload: u32 uid LE */
#define CMD_RFID_CLEAR_LIST       0x11u
#define CMD_RFID_GET_LIST         0x12u

/* Session authorization mode. Payload (1 B): u8 enable. When enable=1,
 * charging is gated on a matched authorized-tag swipe per session
 * (J1772-A unplug clears the per-session grant). When enable=0,
 * plug-in alone starts charging — the original behaviour. The current
 * value (plus the live session_authorized bit) is published via
 * EVT_RFID_CONFIG, on change and as the response to CMD_GET_RFID_CONFIG. */
#define CMD_SET_REQUIRE_RFID_AUTH 0x13u
#define CMD_GET_RFID_CONFIG       0x14u

/* OTA — FC41D-mediated firmware push. The FC41D fetches a new image
 * (HA upload / HTTPS / etc.) and chunks it to the MCU over TLV. The
 * MCU stages to W25Q (upper-half region, see ota_stage.h), verifies a
 * caller-supplied CRC32 on COMMIT, and arms a "pending OTA" flag in
 * boot_config so main()'s pre-FreeRTOS first stage copies the staged
 * image over internal flash on the next reboot.
 *
 * Session model: BEGIN allocates a session_id (FC41D-chosen u32 random
 * token); every subsequent CHUNK / COMMIT / ABORT echoes it. A second
 * BEGIN with a different token aborts any in-flight session. */
#define CMD_OTA_BEGIN             0x15u   /* payload (12 B): u32 size, u32 crc32, u32 session_id */
#define CMD_OTA_CHUNK             0x16u   /* payload (8..56 B): u32 session_id, u32 offset, u8 data[..48] */
#define CMD_OTA_COMMIT            0x17u   /* payload (4 B): u32 session_id */
#define CMD_OTA_ABORT             0x18u   /* payload (4 B): u32 session_id */

/* Wall-clock sync. The MCU keeps a software clock seeded by the FC41D
 * from HA's `time:` component. unix_seconds = 0 in CMD_SET_TIME means
 * "clear / mark not-set". */
#define CMD_SET_TIME              0x19u   /* payload (4 B): u32 unix_seconds LE */
#define CMD_GET_TIME              0x1Au   /* no payload */

/* MCU → FC41D events / responses (bit 7 set) */
#define EVT_PING_ACK              0x81u   /* response to PING */
#define EVT_STATE_REPORT          0x82u   /* response to GET_STATE / spontaneous */
#define EVT_BUILD_INFO            0x8Cu   /* response to GET_BUILD_INFO */

#define EVT_STATE_CHANGED         0x80u   /* unsolicited: J1772 state */
#define EVT_FAULT_RAISED          0x83u   /* unsolicited: u32 fault_id + snapshot */
#define EVT_FAULT_CLEARED         0x84u   /* unsolicited */
#define EVT_SESSION_BEGAN         0x85u
#define EVT_SESSION_ENDED         0x86u
#define EVT_BOOT_COMPLETE         0x87u

/* Responses to GET_FAULT_LOG / GET_LIFETIME_KWH. seq matches the
 * request. EVT_FAULT_LOG_ENTRY is repeated once per record; the
 * terminator EVT_FAULT_LOG_END always follows (count payload). */
#define EVT_FAULT_LOG_ENTRY       0x88u
#define EVT_FAULT_LOG_END         0x89u
#define EVT_LIFETIME_KWH          0x8Au
#define EVT_DEVICE_ID             0x8Du   /* response to GET_DEVICE_ID: 12 B GD32 UID96 */

/* Unsolicited RFID swipe edge.
 *   payload (5 B, packed LE):
 *     u32 uid     — 4-byte card UID, 0 on card-removed edge
 *     u8  present — 1 = card put down, 0 = card lifted off */
#define EVT_RFID_SWIPE            0x8Eu

/* RFID authorization decision. Emitted on every card-present swipe
 * after the authorized-list lookup runs. Lets HA show what action
 * (if any) the swipe drove and which UID the rejection was for.
 *   payload (5 B, packed LE):
 *     u32 uid
 *     u8  result — RFID_AUTH_RESULT_* below */
#define EVT_RFID_AUTH_RESULT      0x8Fu

/* Authorized-list dump (response to CMD_RFID_GET_LIST, response seq).
 *   EVT_RFID_LIST_ENTRY payload (6 B): u8 idx, u8 count, u32 uid
 *   EVT_RFID_LIST_END   payload (1 B): u8 count */
#define EVT_RFID_LIST_ENTRY       0x90u
#define EVT_RFID_LIST_END         0x91u

/* Session-auth config + live state. Payload (2 B):
 *   u8 require_rfid_auth   — persisted boot_config flag
 *   u8 session_authorized  — runtime "this session has been authorized" */
#define EVT_RFID_CONFIG           0x92u

/* OTA event responses (response seq matches the originating request).
 *   EVT_OTA_BEGIN_ACK   payload (8 B): u32 session_id, u8 status, u8 chunk_size_max, u16 reserved
 *   EVT_OTA_CHUNK_ACK   payload (13 B): u32 session_id, u32 next_offset, u32 running_crc32, u8 status
 *   EVT_OTA_COMMITTED   payload (5 B):  u32 session_id, u8 status
 *   EVT_OTA_ABORTED     payload (4 B):  u32 session_id */
#define EVT_OTA_BEGIN_ACK         0x93u
#define EVT_OTA_CHUNK_ACK         0x94u
#define EVT_OTA_COMMITTED         0x95u
#define EVT_OTA_ABORTED           0x96u

/* Response to CMD_GET_TIME — and emitted unsolicited every time the
 * MCU clock is set via CMD_SET_TIME so HA sees the round-trip ack.
 *   payload (5 B): u32 unix_seconds LE, u8 is_set */
#define EVT_TIME                  0x97u

/* Result codes carried in EVT_RFID_AUTH_RESULT.result. */
#define RFID_AUTH_RESULT_LEARNED       0u   /* UID added to list */
#define RFID_AUTH_RESULT_START         1u   /* matched, drove pause→resume */
#define RFID_AUTH_RESULT_STOP          2u   /* matched, drove charging→pause */
#define RFID_AUTH_RESULT_MATCHED_NOOP  3u   /* matched, but no state change applicable */
#define RFID_AUTH_RESULT_REJECTED      4u   /* not on list */
#define RFID_AUTH_RESULT_LIST_FULL     5u   /* learn-mode but list is full */

/* OTA status codes — carried in EVT_OTA_BEGIN_ACK / CHUNK_ACK / COMMITTED. */
#define OTA_STATUS_OK                  0u
#define OTA_STATUS_SESSION_INVALID     1u   /* session_id mismatch */
#define OTA_STATUS_OFFSET_MISMATCH     2u   /* chunk arrived out of order */
#define OTA_STATUS_WRITE_ERROR         3u   /* W25Q stage write failed */
#define OTA_STATUS_OVERSIZE            4u   /* chunk extends past expected size */
#define OTA_STATUS_TOO_LARGE           5u   /* BEGIN: image_size exceeds region */
#define OTA_STATUS_ERASE_FAIL          6u   /* BEGIN: stage erase failed */
#define OTA_STATUS_INVALID_PAYLOAD     7u   /* BEGIN/COMMIT/ABORT: malformed */
#define OTA_STATUS_NO_SESSION          8u   /* CHUNK/COMMIT: no BEGIN sent */
#define OTA_STATUS_CRC_MISMATCH        9u   /* COMMIT: staged CRC != expected */
#define OTA_STATUS_PERSIST_FAIL       10u   /* COMMIT: pending-flag write failed */

#endif /* OPENBHZD_PROTO_COMMANDS_H */
