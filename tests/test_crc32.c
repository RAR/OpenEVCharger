#include "test_harness.h"
#include "crc.h"

void suite_crc32(void)
{
    TEST_CASE("crc32 empty");
    CHECK_EQ_U32(crc32("", 0), 0x00000000u);  /* init 0xFFFFFFFF, final XOR same */

    TEST_CASE("crc32 standard check vector \"123456789\"");
    /* Well-known IEEE 802.3 / zlib check value. */
    CHECK_EQ_U32(crc32("123456789", 9), 0xCBF43926u);

    TEST_CASE("crc32 reproducibility");
    uint8_t buf[64];
    memset(buf, 0xA5, sizeof buf);
    uint32_t base = crc32(buf, sizeof buf);
    CHECK_EQ_U32(crc32(buf, sizeof buf), base);

    TEST_CASE("crc32 sensitive to single-bit flip");
    buf[31] ^= 0x80u;
    CHECK(crc32(buf, sizeof buf) != base);

    TEST_CASE("crc32 sensitive to length");
    uint32_t a = crc32("abc", 3);
    uint32_t b = crc32("abc", 2);
    CHECK(a != b);
}
