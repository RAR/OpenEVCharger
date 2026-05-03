#include "test_harness.h"
#include "crc16.h"

void suite_crc16(void)
{
    TEST_CASE("crc16 empty");
    CHECK_EQ_U32(crc16_ccitt("", 0), 0xFFFFu);  /* init value, no data */

    TEST_CASE("crc16 standard check vector \"123456789\"");
    /* The well-known CRC16-CCITT-FALSE check value. Anchors the algorithm. */
    CHECK_EQ_U32(crc16_ccitt("123456789", 9), 0x29B1u);

    TEST_CASE("crc16 single byte 0x00");
    /* Hand-traced: init 0xFFFF, byte 0x00, run 8 shift-XOR iterations. */
    CHECK_EQ_U32(crc16_ccitt("\x00", 1), 0xE1F0u);

    TEST_CASE("crc16 reproducibility");
    uint8_t f[32];
    memset(f, 0xFF, sizeof f);
    uint16_t base = crc16_ccitt(f, sizeof f);
    CHECK_EQ_U32(crc16_ccitt(f, sizeof f), base);

    TEST_CASE("crc16 sensitive to single-bit flip");
    f[15] ^= 0x01u;
    CHECK(crc16_ccitt(f, sizeof f) != base);

    TEST_CASE("crc16 sensitive to length");
    /* same prefix, different lengths -> different CRCs (with high prob) */
    uint16_t a = crc16_ccitt("abc", 3);
    uint16_t b = crc16_ccitt("abc", 2);
    CHECK(a != b);
}
