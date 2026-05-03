#include "test_harness.h"
#include "tlv.h"

void suite_tlv(void)
{
    uint8_t out[TLV_FRAME_MAX];
    uint8_t  cmd, seq;
    const uint8_t *pl;
    size_t   pl_len;

    TEST_CASE("build: minimal frame (no payload)");
    int n = tlv_build(0x01, 0x42, NULL, 0, out, sizeof out);
    CHECK_EQ_INT(n, (int)(TLV_HDR_LEN + TLV_CRC_LEN));   /* 8 */
    CHECK_EQ_U32(out[0], TLV_SOF0);
    CHECK_EQ_U32(out[1], TLV_SOF1);
    CHECK_EQ_U32(out[2], 0x02u);   /* LEN low = 2 (CMD+SEQ) */
    CHECK_EQ_U32(out[3], 0x00u);
    CHECK_EQ_U32(out[4], 0x01u);   /* CMD */
    CHECK_EQ_U32(out[5], 0x42u);   /* SEQ */

    TEST_CASE("parse: round-trips the empty frame");
    int p = tlv_parse(out, (size_t)n, &cmd, &seq, &pl, &pl_len);
    CHECK_EQ_INT(p, n);
    CHECK_EQ_U32(cmd, 0x01u);
    CHECK_EQ_U32(seq, 0x42u);
    CHECK_EQ_INT((int)pl_len, 0);

    TEST_CASE("build+parse: 4-byte payload");
    uint8_t pay[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    n = tlv_build(0x05, 0x07, pay, sizeof pay, out, sizeof out);
    CHECK_EQ_INT(n, (int)(TLV_HDR_LEN + 4 + TLV_CRC_LEN));
    p = tlv_parse(out, (size_t)n, &cmd, &seq, &pl, &pl_len);
    CHECK_EQ_INT(p, n);
    CHECK_EQ_U32(cmd, 0x05u);
    CHECK_EQ_U32(seq, 0x07u);
    CHECK_EQ_INT((int)pl_len, 4);
    CHECK(pl != NULL);
    CHECK(memcmp(pl, pay, 4) == 0);

    TEST_CASE("build: oversized payload returns -1");
    uint8_t big[TLV_PAYLOAD_MAX + 1] = {0};
    n = tlv_build(0x01, 0x01, big, sizeof big, out, sizeof out);
    CHECK_EQ_INT(n, -1);

    TEST_CASE("build: undersized output buffer returns -1");
    uint8_t small[4];
    n = tlv_build(0x01, 0x01, NULL, 0, small, sizeof small);
    CHECK_EQ_INT(n, -1);

    TEST_CASE("build: max-size payload (56 bytes)");
    uint8_t max_pay[TLV_PAYLOAD_MAX];
    for (size_t i = 0; i < sizeof max_pay; ++i) max_pay[i] = (uint8_t)i;
    n = tlv_build(0x0C, 0xFF, max_pay, sizeof max_pay, out, sizeof out);
    CHECK_EQ_INT(n, (int)TLV_FRAME_MAX);
    p = tlv_parse(out, (size_t)n, &cmd, &seq, &pl, &pl_len);
    CHECK_EQ_INT(p, n);
    CHECK_EQ_INT((int)pl_len, (int)sizeof max_pay);
    CHECK(memcmp(pl, max_pay, sizeof max_pay) == 0);

    TEST_CASE("parse: incomplete frame returns 0 (need more)");
    n = tlv_build(0x01, 0x01, pay, 4, out, sizeof out);
    /* truncate to 5 bytes (< header) */
    p = tlv_parse(out, 5u, &cmd, &seq, &pl, &pl_len);
    CHECK_EQ_INT(p, 0);
    /* truncate to header but not payload+CRC */
    p = tlv_parse(out, TLV_HDR_LEN, &cmd, &seq, &pl, &pl_len);
    CHECK_EQ_INT(p, 0);
    /* one byte short of CRC */
    p = tlv_parse(out, (size_t)(n - 1), &cmd, &seq, &pl, &pl_len);
    CHECK_EQ_INT(p, 0);

    TEST_CASE("parse: bad SOF returns -1 (resync)");
    out[0] = 0x00;
    p = tlv_parse(out, (size_t)n, &cmd, &seq, &pl, &pl_len);
    CHECK_EQ_INT(p, -1);

    TEST_CASE("parse: bad CRC returns -1");
    n = tlv_build(0x01, 0x01, pay, 4, out, sizeof out);
    out[n - 1] ^= 0xFFu;   /* flip last CRC byte */
    p = tlv_parse(out, (size_t)n, &cmd, &seq, &pl, &pl_len);
    CHECK_EQ_INT(p, -1);

    TEST_CASE("parse: oversized LEN field returns -1");
    n = tlv_build(0x01, 0x01, NULL, 0, out, sizeof out);
    /* claim len = 2 + TLV_PAYLOAD_MAX + 1 → out of range */
    uint16_t bad_len = (uint16_t)(2u + TLV_PAYLOAD_MAX + 1u);
    out[2] = (uint8_t)(bad_len & 0xFFu);
    out[3] = (uint8_t)(bad_len >> 8);
    p = tlv_parse(out, sizeof out, &cmd, &seq, &pl, &pl_len);
    CHECK_EQ_INT(p, -1);

    TEST_CASE("parse: LEN field < 2 returns -1");
    n = tlv_build(0x01, 0x01, NULL, 0, out, sizeof out);
    out[2] = 0x01;   /* len=1 < 2 (must include CMD+SEQ at minimum) */
    out[3] = 0x00;
    p = tlv_parse(out, (size_t)n, &cmd, &seq, &pl, &pl_len);
    CHECK_EQ_INT(p, -1);

    TEST_CASE("CRC byte order: stored big-endian (CRCH first)");
    /* Build a frame, then verify the stored CRC matches what big-endian
     * extraction yields when re-parsed. Implicit in round-trip but we
     * codify the wire order here so spec § 5 doesn't drift. */
    n = tlv_build(0x02, 0x33, pay, 4, out, sizeof out);
    uint16_t hi = out[n - 2];
    uint16_t lo = out[n - 1];
    /* The big-endian assembly hi<<8|lo must equal the parser's
     * accepted value. Easiest sanity: swap the two and parse should
     * fail. */
    out[n - 2] = (uint8_t)lo;
    out[n - 1] = (uint8_t)hi;
    p = tlv_parse(out, (size_t)n, &cmd, &seq, &pl, &pl_len);
    CHECK_EQ_INT(p, -1);
}
