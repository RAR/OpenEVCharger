#include "rfid.h"
#include <string.h>

/* Try to consume one complete frame from the head of ctx->buf. Returns
 * the number of bytes consumed, or 0 if the buffer is incomplete /
 * not yet a valid frame. Sets out->edge=1 if this frame produced a
 * new card-present or card-removed edge. */
static unsigned try_parse_one(rfid_ctx_t *ctx, rfid_action_t *out)
{
    /* Re-sync: drop bytes until the head matches the preamble byte 0. */
    while (ctx->len > 0 && ctx->buf[0] != RFID_PREAMBLE_0) {
        memmove(ctx->buf, ctx->buf + 1, ctx->len - 1);
        ctx->len--;
    }
    if (ctx->len < 5) return 0;

    /* Validate preamble bytes 1..3. If any don't match, drop byte 0
     * and ask the caller to re-enter. */
    if (ctx->buf[1] != RFID_PREAMBLE_1 ||
        ctx->buf[2] != RFID_PREAMBLE_2 ||
        ctx->buf[3] != RFID_PREAMBLE_3) {
        memmove(ctx->buf, ctx->buf + 1, ctx->len - 1);
        ctx->len--;
        return 0;
    }

    uint8_t  ll = ctx->buf[4];
    unsigned frame_len;
    if (ll == 0u)      frame_len = RFID_FRAME_IDLE_LEN;
    else if (ll == 4u) frame_len = RFID_FRAME_CARD_LEN;
    else {
        /* Unknown LL — drop the preamble byte and re-sync. Stops a
         * malformed length from stalling the parser indefinitely. */
        memmove(ctx->buf, ctx->buf + 1, ctx->len - 1);
        ctx->len--;
        return 0;
    }

    if (ctx->len < frame_len) return 0;   /* incomplete */

    /* Frame complete — decode UID + edge-detect. */
    uint32_t uid = 0;
    if (ll == 4u) {
        /* Stock fw uses ldr.w r1, [r0, #6] — little-endian read. */
        uid = ((uint32_t)ctx->buf[6])
            | ((uint32_t)ctx->buf[7] << 8)
            | ((uint32_t)ctx->buf[8] << 16)
            | ((uint32_t)ctx->buf[9] << 24);
    }
    uint8_t present_now = (ll == 4u) ? 1u : 0u;

    if (uid != ctx->last_uid || present_now != ctx->card_present) {
        ctx->last_uid     = uid;
        ctx->card_present = present_now;
        out->edge    = 1;
        out->uid     = uid;
        out->present = present_now;
    }

    /* Consume the frame from the head of the buffer. */
    if (ctx->len > frame_len) {
        memmove(ctx->buf, ctx->buf + frame_len, ctx->len - frame_len);
    }
    ctx->len -= frame_len;
    return frame_len;
}

unsigned rfid_feed(rfid_ctx_t *ctx, const uint8_t *bytes, size_t n,
                   rfid_action_t *out)
{
    out->edge = 0;
    out->uid = ctx->last_uid;
    out->present = ctx->card_present;

    /* Append, dropping overflow at the tail. The buffer is sized at
     * 16 B — enough for an idle (7) + card (11) overlap if a frame
     * straddles a tick. Real-world steady-state has at most one
     * pending frame between ticks, so overflow is rare. */
    for (size_t i = 0; i < n; ++i) {
        if (ctx->len < RFID_FRAME_MAX_LEN) {
            ctx->buf[ctx->len++] = bytes[i];
        } else {
            /* Drop oldest byte; shift the rest up. Lets us keep
             * parsing if the line is noisy. */
            memmove(ctx->buf, ctx->buf + 1, RFID_FRAME_MAX_LEN - 1);
            ctx->buf[RFID_FRAME_MAX_LEN - 1] = bytes[i];
        }
    }

    unsigned edges = 0;
    while (try_parse_one(ctx, out) > 0) {
        if (out->edge) {
            edges++;
            /* Keep edge=1 latched; caller sees it on return. If
             * multiple edges in one feed, the LAST one wins — UI
             * cadence (~3 Hz) means this is rare. */
        }
    }
    return edges;
}
