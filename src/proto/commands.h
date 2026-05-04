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

#endif /* OPENBHZD_PROTO_COMMANDS_H */
