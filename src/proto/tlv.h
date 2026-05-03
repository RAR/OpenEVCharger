#ifndef OPENBHZD_PROTO_TLV_H
#define OPENBHZD_PROTO_TLV_H

/* FC41D ↔ MCU TLV protocol per spec § 5.
 *
 * Frame layout:
 *
 *   off  size  field
 *   0    2     SOF       = 0xA5 0x5A
 *   2    2     LEN       = u16 LE = bytes from CMD through end of payload
 *   4    1     CMD       = u8 (0x00..0x7F = request, 0x80..0xFF = event/resp)
 *   5    1     SEQ       = u8 (0 = event/unsolicited, otherwise pairs req/resp)
 *   6..  N     payload   = LEN-2 bytes
 *   N+6  2     CRC16     = u16 BE, CRC16-CCITT-FALSE over LEN..PAYLOAD
 *
 * Max total frame: 64 bytes (2 SOF + 2 LEN + 1 CMD + 1 SEQ + 56 payload + 2 CRC).
 *
 * Spec § 5 says max 54 payload; we round to 56 to keep the math even
 * (header 6 + payload 56 + CRC 2 = 64). Implementation accepts up to 56;
 * extra payload bytes a rejection from the parser. */

#include <stddef.h>
#include <stdint.h>

#define TLV_SOF0          0xA5u
#define TLV_SOF1          0x5Au
#define TLV_HDR_LEN       6u    /* SOF[2] + LEN[2] + CMD + SEQ */
#define TLV_CRC_LEN       2u
#define TLV_PAYLOAD_MAX   56u
#define TLV_FRAME_MAX     (TLV_HDR_LEN + TLV_PAYLOAD_MAX + TLV_CRC_LEN)

/* Build a frame into out_buf. Returns total frame length on success
 * (≥ TLV_HDR_LEN+TLV_CRC_LEN), or -1 if payload_len > TLV_PAYLOAD_MAX
 * or out_buf is too small. out_buf must be at least TLV_FRAME_MAX bytes
 * (caller's responsibility). */
int tlv_build(uint8_t cmd, uint8_t seq,
              const void *payload, size_t payload_len,
              uint8_t *out_buf, size_t out_buf_len);

/* Parse a candidate frame buffer. Returns:
 *   >0 = valid frame; total bytes consumed (length of the frame)
 *    0 = need more bytes (incomplete frame)
 *   <0 = framing/CRC error; caller should resync (advance 1 byte)
 *
 * On success, *cmd, *seq, *payload (pointer into buf), *payload_len
 * are populated. */
int tlv_parse(const uint8_t *buf, size_t len,
              uint8_t *cmd, uint8_t *seq,
              const uint8_t **payload, size_t *payload_len);

#endif /* OPENBHZD_PROTO_TLV_H */
