#include "tlv.h"
#include "../persist/crc16.h"

int tlv_build(uint8_t cmd, uint8_t seq,
              const void *payload, size_t payload_len,
              uint8_t *out_buf, size_t out_buf_len)
{
    if (payload_len > TLV_PAYLOAD_MAX) return -1;
    size_t total = TLV_HDR_LEN + payload_len + TLV_CRC_LEN;
    if (out_buf_len < total) return -1;

    /* SOF */
    out_buf[0] = TLV_SOF0;
    out_buf[1] = TLV_SOF1;

    /* LEN = bytes from CMD through end of payload (excluding CRC). */
    uint16_t len_field = (uint16_t)(2u + payload_len);  /* CMD + SEQ + payload */
    out_buf[2] = (uint8_t)(len_field & 0xFFu);
    out_buf[3] = (uint8_t)((len_field >> 8) & 0xFFu);

    out_buf[4] = cmd;
    out_buf[5] = seq;

    if (payload && payload_len) {
        const uint8_t *src = (const uint8_t *)payload;
        for (size_t i = 0; i < payload_len; ++i) {
            out_buf[TLV_HDR_LEN + i] = src[i];
        }
    }

    /* CRC16 over LEN..PAYLOAD inclusive (offset 2 .. TLV_HDR_LEN+payload_len). */
    uint16_t crc = crc16_ccitt(&out_buf[2],
                               (size_t)(2u + 2u + payload_len));
    /* CRC stored big-endian per spec § 5 ASCII diagram (CRCH then CRCL). */
    out_buf[TLV_HDR_LEN + payload_len]     = (uint8_t)((crc >> 8) & 0xFFu);
    out_buf[TLV_HDR_LEN + payload_len + 1] = (uint8_t)(crc & 0xFFu);

    return (int)total;
}

int tlv_parse(const uint8_t *buf, size_t len,
              uint8_t *cmd, uint8_t *seq,
              const uint8_t **payload, size_t *payload_len)
{
    if (len < TLV_HDR_LEN + TLV_CRC_LEN) return 0;          /* need more */
    if (buf[0] != TLV_SOF0 || buf[1] != TLV_SOF1) return -1; /* resync */

    uint16_t len_field = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    if (len_field < 2u || len_field > (2u + TLV_PAYLOAD_MAX)) return -1;

    size_t pl_len = (size_t)(len_field - 2u);
    size_t total  = TLV_HDR_LEN + pl_len + TLV_CRC_LEN;
    if (len < total) return 0;                               /* need more */

    uint16_t got = ((uint16_t)buf[TLV_HDR_LEN + pl_len] << 8) |
                   (uint16_t)buf[TLV_HDR_LEN + pl_len + 1];
    uint16_t want = crc16_ccitt(&buf[2], (size_t)(2u + 2u + pl_len));
    if (got != want) return -1;

    *cmd = buf[4];
    *seq = buf[5];
    *payload = &buf[TLV_HDR_LEN];
    *payload_len = pl_len;
    return (int)total;
}
