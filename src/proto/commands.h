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

/* Result codes carried in EVT_RFID_AUTH_RESULT.result. */
#define RFID_AUTH_RESULT_LEARNED       0u   /* UID added to list */
#define RFID_AUTH_RESULT_START         1u   /* matched, drove pause→resume */
#define RFID_AUTH_RESULT_STOP          2u   /* matched, drove charging→pause */
#define RFID_AUTH_RESULT_MATCHED_NOOP  3u   /* matched, but no state change applicable */
#define RFID_AUTH_RESULT_REJECTED      4u   /* not on list */
#define RFID_AUTH_RESULT_LIST_FULL     5u   /* learn-mode but list is full */

#endif /* OPENBHZD_PROTO_COMMANDS_H */
