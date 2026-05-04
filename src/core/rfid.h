#ifndef OPENBHZD_CORE_RFID_H
#define OPENBHZD_CORE_RFID_H

#include <stdint.h>
#include <stddef.h>

/* Pure RFID frame re-framer + edge detector. Caller (safety_task)
 * drains the USART1 RX ring each tick and pushes the bytes through
 * rfid_feed(). Whenever a new card-present or card-removed edge is
 * seen, rfid_step() returns 1 with the latched UID + present flag in
 * the action struct; caller publishes EVT_RFID_SWIPE.
 *
 * Wire format (decoded from stock fw V1.0.066, bench-validated
 * 2026-05-04 — see docs/mcu-re/rfid-protocol.md):
 *
 *     AA D1 D0 02 LL 00 [UID×LL] STATE
 *
 * with LL=0 meaning "no card" and LL=4 meaning "4-byte UID at +6".
 * Stock fw treats UID as little-endian u32 read from byte 6. */

#define RFID_PREAMBLE_0   0xAAu
#define RFID_PREAMBLE_1   0xD1u
#define RFID_PREAMBLE_2   0xD0u
#define RFID_PREAMBLE_3   0x02u
#define RFID_FRAME_IDLE_LEN  7u    /* AA D1 D0 02 00 00 STATE */
#define RFID_FRAME_CARD_LEN  11u   /* AA D1 D0 02 04 00 U0..U3 STATE */
#define RFID_FRAME_MAX_LEN   16u   /* generous re-framing buffer */

typedef struct {
    /* Re-framing: bytes accumulate here until a complete frame is
     * either parsed or rejected. Reset on any preamble mismatch. */
    uint8_t  buf[RFID_FRAME_MAX_LEN];
    uint8_t  len;
    /* Latched state — what the caller publishes on each new edge. */
    uint32_t last_uid;
    uint8_t  card_present;
} rfid_ctx_t;

typedef struct {
    int      edge;        /* 1 if an edge fired this step, else 0 */
    uint32_t uid;         /* latched UID (0 on card-removed edge) */
    uint8_t  present;     /* 1 = card put down, 0 = lifted off */
} rfid_action_t;

/* Reset to "no card seen, empty buffer". */
static inline void rfid_init_ctx(rfid_ctx_t *ctx)
{
    ctx->buf[0] = 0;
    ctx->len = 0;
    ctx->last_uid = 0;
    ctx->card_present = 0;
}

/* Feed a stream of bytes from the UART ring. Internally re-frames on
 * the AA D1 D0 02 preamble, decodes complete frames, and updates the
 * latched state. Returns the number of edges produced (0 or 1 per
 * call typically; can be >1 if a backlog of frames was drained at
 * once). The caller can call rfid_step() to fetch the latest action
 * — only the most-recent edge is recoverable that way. For per-edge
 * fan-out, call this in a tight drain loop and check the return. */
unsigned rfid_feed(rfid_ctx_t *ctx, const uint8_t *bytes, size_t n,
                   rfid_action_t *out);

#endif
