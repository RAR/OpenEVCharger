/* test_rfid — exercises the rfid frame parser, UID-to-hex conversion, and
 * the debounce policy. NO serial I/O — rfid_reader_test_init() leaves the
 * fd at -1 so rfid_reader_tick() short-circuits; we drive handle_uid() via
 * rfid_reader_test_inject(). */
#include <string.h>
#include "test_harness.h"
#include "rfid.h"

/* --- shared callback recorder ------------------------------------------ */

static int  cb_count;
static char cb_last[32];

static void on_scan(void *user, const char *uid_hex)
{
    (void)user;
    cb_count++;
    snprintf(cb_last, sizeof(cb_last), "%s", uid_hex);
}

static void reset_cb(void) { cb_count = 0; cb_last[0] = '\0'; }

/* Build a wire-format frame: [LEN][CMD][PAYLOAD...][XOR][0]. Returns size. */
static size_t make_frame(unsigned char *out, unsigned char cmd,
                         const unsigned char *payload, size_t plen)
{
    size_t total = 2 + plen;
    out[0] = (unsigned char)total;
    out[1] = cmd;
    memcpy(out + 2, payload, plen);
    unsigned char x = 0;
    for (size_t i = 0; i < total; i++) x ^= out[i];
    out[total]     = x;
    out[total + 1] = 0;
    return total + 2;
}

/* --- frame parser ------------------------------------------------------- */

static void test_parse_valid_4byte_uid(void)
{
    /* 4-byte UID frame: LEN=0x09, CMD=0x20, 7 payload bytes. The payload
     * layout we exercise matches the stock daemon's RCV_DATA dispatch:
     * uid_out reads from buf[2..5], so the first 4 payload bytes are the
     * UID and the remaining 3 are anticollision trailer (ignored). */
    unsigned char payload[7] = { 0x04, 0xa1, 0xb2, 0xc3, 0x00, 0x00, 0x00 };
    unsigned char buf[16];
    size_t n = make_frame(buf, 0x20, payload, 7);
    CHECK_EQ(n, 11);
    CHECK_EQ(buf[0], 0x09);

    unsigned char uid[16];
    size_t uid_len = 0;
    int consumed = rfid_parse_frame(buf, n, uid, &uid_len);
    CHECK_EQ(consumed, 11);
    CHECK_EQ(uid_len, 4);
    CHECK_EQ(uid[0], 0x04); CHECK_EQ(uid[1], 0xa1);
    CHECK_EQ(uid[2], 0xb2); CHECK_EQ(uid[3], 0xc3);
}

static void test_parse_valid_7byte_uid(void)
{
    unsigned char payload[10] = { 0x04, 0x23, 0x67, 0x0a, 0xc3, 0x26, 0x80,
                                  0x00, 0x00, 0x00 };
    unsigned char buf[16];
    size_t n = make_frame(buf, 0x20, payload, 10);
    CHECK_EQ(buf[0], 0x0C);

    unsigned char uid[16];
    size_t uid_len = 0;
    int consumed = rfid_parse_frame(buf, n, uid, &uid_len);
    CHECK_EQ(consumed, (int)n);
    CHECK_EQ(uid_len, 7);
    CHECK_EQ(uid[0], 0x04);
    CHECK_EQ(uid[6], 0x80);
}

static void test_parse_bad_xor(void)
{
    unsigned char payload[7] = { 0x04, 0xa1, 0xb2, 0xc3, 0x00, 0x00, 0x00 };
    unsigned char buf[16];
    size_t n = make_frame(buf, 0x20, payload, 7);
    /* Corrupt the XOR byte. */
    buf[n - 2] ^= 0xFF;

    unsigned char uid[16];
    size_t uid_len = 999;
    int consumed = rfid_parse_frame(buf, n, uid, &uid_len);
    CHECK_EQ(consumed, -1);
    CHECK_EQ(uid_len, 0);
}

static void test_parse_bad_trailing_zero(void)
{
    unsigned char payload[7] = { 0x04, 0xa1, 0xb2, 0xc3, 0x00, 0x00, 0x00 };
    unsigned char buf[16];
    size_t n = make_frame(buf, 0x20, payload, 7);
    buf[n - 1] = 0xFF;           /* trailing zero broken */

    unsigned char uid[16];
    size_t uid_len = 0;
    int consumed = rfid_parse_frame(buf, n, uid, &uid_len);
    CHECK_EQ(consumed, -1);
}

static void test_parse_truncated(void)
{
    unsigned char payload[7] = { 0x04, 0xa1, 0xb2, 0xc3, 0x00, 0x00, 0x00 };
    unsigned char buf[16];
    size_t n = make_frame(buf, 0x20, payload, 7);
    /* Give the parser only the first half. */
    unsigned char uid[16];
    size_t uid_len = 999;
    int consumed = rfid_parse_frame(buf, n - 4, uid, &uid_len);
    CHECK_EQ(consumed, 0);   /* "need more bytes" */
    CHECK_EQ(uid_len, 0);
}

static void test_parse_payload_with_zeros(void)
{
    /* A 7-byte UID can legitimately contain 0x00 bytes anywhere. The parser
     * uses the LEN byte (not strlen) to size the frame, so embedded zeros
     * must round-trip cleanly. */
    unsigned char payload[10] = { 0x04, 0x00, 0x00, 0xaa, 0xbb, 0x00, 0xff,
                                  0x00, 0x00, 0x00 };
    unsigned char buf[16];
    size_t n = make_frame(buf, 0x20, payload, 10);

    unsigned char uid[16];
    size_t uid_len = 0;
    int consumed = rfid_parse_frame(buf, n, uid, &uid_len);
    CHECK_EQ(consumed, (int)n);
    CHECK_EQ(uid_len, 7);
    CHECK_EQ(uid[1], 0x00);
    CHECK_EQ(uid[2], 0x00);
    CHECK_EQ(uid[5], 0x00);
    CHECK_EQ(uid[6], 0xff);
}

static void test_parse_no_card_frame(void)
{
    /* A "valid frame, no UID" — len byte not in {0x09, 0x0C, 0x0F}.
     * The stock binary's dispatch labels this as the "no card present"
     * reply; we accept the frame but emit no callback / no UID. */
    unsigned char payload[1] = { 0x00 };
    unsigned char buf[8];
    size_t n = make_frame(buf, 0x20, payload, 1);   /* LEN = 3 */

    unsigned char uid[16];
    size_t uid_len = 999;
    int consumed = rfid_parse_frame(buf, n, uid, &uid_len);
    CHECK_EQ(consumed, (int)n);
    CHECK_EQ(uid_len, 0);
}

/* --- UID -> ASCII hex --------------------------------------------------- */

static void test_uid_to_hex(void)
{
    char out[32];
    unsigned char uid4[4] = { 0x04, 0xa1, 0xb2, 0xc3 };
    CHECK_EQ(rfid_uid_to_hex(uid4, 4, out, sizeof(out)), 0);
    CHECK_STR(out, "04A1B2C3");

    unsigned char uid7[7] = { 0x04, 0x23, 0x67, 0x0a, 0xc3, 0x26, 0x80 };
    CHECK_EQ(rfid_uid_to_hex(uid7, 7, out, sizeof(out)), 0);
    CHECK_STR(out, "0423670AC32680");

    /* Uppercase enforcement — input contains low and high hex nibbles. */
    unsigned char mixed[3] = { 0xab, 0xcd, 0xef };
    CHECK_EQ(rfid_uid_to_hex(mixed, 3, out, sizeof(out)), 0);
    CHECK_STR(out, "ABCDEF");

    /* Too-small buffer rejected. */
    char small[3];
    CHECK_EQ(rfid_uid_to_hex(uid4, 4, small, sizeof(small)), -1);
}

/* --- Debounce policy ---------------------------------------------------- */

static void test_debounce(void)
{
    struct rfid_reader *r = NULL;
    CHECK_EQ(rfid_reader_test_init(&r, on_scan, NULL), 0);
    CHECK(r != NULL);

    unsigned char uidA[4] = { 0x04, 0xa1, 0xb2, 0xc3 };
    unsigned char uidB[4] = { 0x04, 0xde, 0xad, 0xbe };

    reset_cb();

    /* First-ever scan -> fires. */
    CHECK_EQ(rfid_reader_test_inject(r, uidA, 4, 1000), 0);
    CHECK_EQ(cb_count, 1);
    CHECK_STR(cb_last, "04A1B2C3");

    /* Same UID re-presented 500 ms later -> suppressed. */
    CHECK_EQ(rfid_reader_test_inject(r, uidA, 4, 1500), 0);
    CHECK_EQ(cb_count, 1);

    /* Same UID, still in the held window (< 2 s gap) -> suppressed. */
    CHECK_EQ(rfid_reader_test_inject(r, uidA, 4, 2500), 0);
    CHECK_EQ(cb_count, 1);

    /* Different UID -> fires immediately. */
    CHECK_EQ(rfid_reader_test_inject(r, uidB, 4, 2600), 0);
    CHECK_EQ(cb_count, 2);
    CHECK_STR(cb_last, "04DEADBE");

    /* Same UID right after switching -> suppressed. */
    CHECK_EQ(rfid_reader_test_inject(r, uidB, 4, 2700), 0);
    CHECK_EQ(cb_count, 2);

    /* Now back to A — it's different from current `last_uid` so it fires
     * regardless of timing. */
    CHECK_EQ(rfid_reader_test_inject(r, uidA, 4, 2800), 0);
    CHECK_EQ(cb_count, 3);
    CHECK_STR(cb_last, "04A1B2C3");

    /* Hold A continuously past the 2 s mark — should stay suppressed
     * because each tick refreshes last_seen_ms. */
    CHECK_EQ(rfid_reader_test_inject(r, uidA, 4, 3300), 0);
    CHECK_EQ(rfid_reader_test_inject(r, uidA, 4, 4000), 0);
    CHECK_EQ(rfid_reader_test_inject(r, uidA, 4, 4800), 0);
    CHECK_EQ(rfid_reader_test_inject(r, uidA, 4, 5500), 0);  /* 2.7s after last fire, but each call refreshes */
    CHECK_EQ(cb_count, 3);

    /* Stop seeing the card for >2 s, then present A again -> fires. */
    CHECK_EQ(rfid_reader_test_inject(r, uidA, 4, 8000), 0);  /* 2500 ms gap */
    CHECK_EQ(cb_count, 4);
}

int main(void)
{
    test_parse_valid_4byte_uid();
    test_parse_valid_7byte_uid();
    test_parse_bad_xor();
    test_parse_bad_trailing_zero();
    test_parse_truncated();
    test_parse_payload_with_zeros();
    test_parse_no_card_frame();
    test_uid_to_hex();
    test_debounce();
    TEST_MAIN_END();
}
