/* test_rfid — frame parser + UID-to-hex + debounce, against the v0.6 wire
 * format ([LEN][CMD][PAYLOAD][XOR], NO trailing zero — corrects v0.5).
 * No serial I/O: rfid_reader_test_init() leaves the fd at -1 so tick()
 * short-circuits; we drive handle_uid() via rfid_reader_test_inject(). */
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

/* Build a wire-format frame: [LEN][CMD][PAYLOAD...][XOR]. Returns size.
 * Matches the on-wire format observed via the LD_PRELOAD shim 2026-05-16
 * (docs/10). No trailing zero. */
static size_t make_frame(unsigned char *out, unsigned char cmd,
                         const unsigned char *payload, size_t plen)
{
    size_t total = 2 + plen;
    out[0] = (unsigned char)total;
    out[1] = cmd;
    memcpy(out + 2, payload, plen);
    unsigned char x = 0;
    for (size_t i = 0; i < total; i++) x ^= out[i];
    out[total] = x;
    return total + 1;
}

/* --- frame parser ------------------------------------------------------- */

static void test_parse_valid_4byte_uid(void)
{
    /* LEN=0x09 means 4-byte UID. Payload = 4 UID bytes + 3 trailer bytes. */
    unsigned char payload[7] = { 0x04, 0xa1, 0xb2, 0xc3, 0x00, 0x00, 0x00 };
    unsigned char buf[16];
    size_t n = make_frame(buf, 0x20, payload, 7);
    CHECK_EQ(n, 10);                     /* LEN(1) + CMD(1) + payload(7) + XOR(1) */
    CHECK_EQ(buf[0], 0x09);

    unsigned char uid[16];
    size_t uid_len = 0;
    int consumed = rfid_parse_frame(buf, n, uid, &uid_len);
    CHECK_EQ(consumed, 10);
    CHECK_EQ(uid_len, 4);
    CHECK_EQ(uid[0], 0x04); CHECK_EQ(uid[1], 0xa1);
    CHECK_EQ(uid[2], 0xb2); CHECK_EQ(uid[3], 0xc3);
}

static void test_parse_valid_7byte_uid(void)
{
    /* LEN=0x0C means 7-byte UID. Payload = 7 UID bytes + 3 trailer bytes.
     * This is the wire-confirmed shape from our 2026-05-16 capture
     * (card UID `04 ae bf 7e 3e 61 81`, trailer `44 00 00`). */
    unsigned char payload[10] = { 0x04, 0xae, 0xbf, 0x7e, 0x3e, 0x61, 0x81,
                                  0x44, 0x00, 0x00 };
    unsigned char buf[16];
    size_t n = make_frame(buf, 0x20, payload, 10);
    CHECK_EQ(n, 13);                     /* LEN(1) + CMD(1) + payload(10) + XOR(1) */
    CHECK_EQ(buf[0], 0x0C);

    unsigned char uid[16];
    size_t uid_len = 0;
    int consumed = rfid_parse_frame(buf, n, uid, &uid_len);
    CHECK_EQ(consumed, 13);
    CHECK_EQ(uid_len, 7);
    CHECK_EQ(uid[0], 0x04);
    CHECK_EQ(uid[6], 0x81);
}

static void test_parse_wire_capture_2026_05_16(void)
{
    /* Exact bytes from companion/test/data/uart-trace-stock-2026-05-16.log
     * line "[118.539722] READ << 0c 20 04 ae bf 7e 3e 61 81 44 00 00 dd".
     * Verifies our parser accepts the real reader's output verbatim. */
    unsigned char wire[13] = {
        0x0c, 0x20, 0x04, 0xae, 0xbf, 0x7e, 0x3e, 0x61, 0x81, 0x44,
        0x00, 0x00, 0xdd
    };
    unsigned char uid[16];
    size_t uid_len = 0;
    int consumed = rfid_parse_frame(wire, sizeof(wire), uid, &uid_len);
    CHECK_EQ(consumed, 13);
    CHECK_EQ(uid_len, 7);
    CHECK_EQ(uid[0], 0x04); CHECK_EQ(uid[1], 0xae);
    CHECK_EQ(uid[2], 0xbf); CHECK_EQ(uid[3], 0x7e);
    CHECK_EQ(uid[4], 0x3e); CHECK_EQ(uid[5], 0x61);
    CHECK_EQ(uid[6], 0x81);
}

static void test_parse_no_card_steady(void)
{
    /* "02 df dd" — steady-state no-card response. LEN=2, payload=df, XOR=dd.
     * Parser must accept (valid frame) but emit no UID. */
    unsigned char wire[3] = { 0x02, 0xdf, 0xdd };
    unsigned char uid[16];
    size_t uid_len = 999;
    int consumed = rfid_parse_frame(wire, sizeof(wire), uid, &uid_len);
    CHECK_EQ(consumed, 3);
    CHECK_EQ(uid_len, 0);
}

static void test_parse_no_card_post_session(void)
{
    /* "02 be bc" — observed once at the end of our capture; either a
     * "card just removed" or alternate no-card status. Same shape. */
    unsigned char wire[3] = { 0x02, 0xbe, 0xbc };
    unsigned char uid[16];
    size_t uid_len = 999;
    int consumed = rfid_parse_frame(wire, sizeof(wire), uid, &uid_len);
    CHECK_EQ(consumed, 3);
    CHECK_EQ(uid_len, 0);
}

static void test_parse_bad_xor(void)
{
    unsigned char payload[7] = { 0x04, 0xa1, 0xb2, 0xc3, 0x00, 0x00, 0x00 };
    unsigned char buf[16];
    size_t n = make_frame(buf, 0x20, payload, 7);
    buf[n - 1] ^= 0xFF;                  /* corrupt XOR */

    unsigned char uid[16];
    size_t uid_len = 999;
    int consumed = rfid_parse_frame(buf, n, uid, &uid_len);
    CHECK_EQ(consumed, -1);
    CHECK_EQ(uid_len, 0);
}

static void test_parse_truncated(void)
{
    unsigned char payload[7] = { 0x04, 0xa1, 0xb2, 0xc3, 0x00, 0x00, 0x00 };
    unsigned char buf[16];
    size_t n = make_frame(buf, 0x20, payload, 7);

    unsigned char uid[16];
    size_t uid_len = 999;
    int consumed = rfid_parse_frame(buf, n - 4, uid, &uid_len);
    CHECK_EQ(consumed, 0);               /* "need more bytes" */
    CHECK_EQ(uid_len, 0);
}

static void test_parse_payload_with_zeros(void)
{
    /* A 7-byte UID can legitimately contain 0x00 bytes anywhere. The parser
     * uses LEN (not strlen) for sizing — embedded zeros round-trip cleanly. */
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

static void test_parse_partial_then_complete(void)
{
    /* Caller may receive a frame in pieces. Parser must return 0 ("need
     * more") on the partial slice, then succeed when given the full frame. */
    unsigned char wire[13] = {
        0x0c, 0x20, 0x04, 0xae, 0xbf, 0x7e, 0x3e, 0x61, 0x81, 0x44,
        0x00, 0x00, 0xdd
    };
    unsigned char uid[16];
    size_t uid_len = 999;
    int r;

    /* Just LEN+CMD — no payload yet. */
    r = rfid_parse_frame(wire, 2, uid, &uid_len);
    CHECK_EQ(r, 0); CHECK_EQ(uid_len, 0);

    /* LEN+CMD+partial payload — still incomplete (need 13, have 12). */
    r = rfid_parse_frame(wire, 12, uid, &uid_len);
    CHECK_EQ(r, 0); CHECK_EQ(uid_len, 0);

    /* All 13 bytes — succeeds. */
    r = rfid_parse_frame(wire, 13, uid, &uid_len);
    CHECK_EQ(r, 13); CHECK_EQ(uid_len, 7);
}

/* --- UID -> ASCII hex --------------------------------------------------- */

static void test_uid_to_hex(void)
{
    char out[32];
    unsigned char uid4[4] = { 0x04, 0xa1, 0xb2, 0xc3 };
    CHECK_EQ(rfid_uid_to_hex(uid4, 4, out, sizeof(out)), 0);
    CHECK_STR(out, "04A1B2C3");

    unsigned char uid7[7] = { 0x04, 0xae, 0xbf, 0x7e, 0x3e, 0x61, 0x81 };
    CHECK_EQ(rfid_uid_to_hex(uid7, 7, out, sizeof(out)), 0);
    CHECK_STR(out, "04AEBF7E3E6181");

    unsigned char mixed[3] = { 0xab, 0xcd, 0xef };
    CHECK_EQ(rfid_uid_to_hex(mixed, 3, out, sizeof(out)), 0);
    CHECK_STR(out, "ABCDEF");

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

    CHECK_EQ(rfid_reader_test_inject(r, uidA, 4, 1000), 0);
    CHECK_EQ(cb_count, 1);
    CHECK_STR(cb_last, "04A1B2C3");

    /* Same UID re-presented 500 ms later -> suppressed. */
    CHECK_EQ(rfid_reader_test_inject(r, uidA, 4, 1500), 0);
    CHECK_EQ(cb_count, 1);

    /* Same UID, still held (< 2 s gap) -> suppressed. */
    CHECK_EQ(rfid_reader_test_inject(r, uidA, 4, 2500), 0);
    CHECK_EQ(cb_count, 1);

    /* Different UID -> fires immediately. */
    CHECK_EQ(rfid_reader_test_inject(r, uidB, 4, 2600), 0);
    CHECK_EQ(cb_count, 2);
    CHECK_STR(cb_last, "04DEADBE");

    /* Same UID right after switching -> suppressed. */
    CHECK_EQ(rfid_reader_test_inject(r, uidB, 4, 2700), 0);
    CHECK_EQ(cb_count, 2);

    /* Back to A — different from current last_uid, fires. */
    CHECK_EQ(rfid_reader_test_inject(r, uidA, 4, 2800), 0);
    CHECK_EQ(cb_count, 3);
    CHECK_STR(cb_last, "04A1B2C3");

    /* Hold A continuously past the 2 s mark — each call refreshes last_seen. */
    CHECK_EQ(rfid_reader_test_inject(r, uidA, 4, 3300), 0);
    CHECK_EQ(rfid_reader_test_inject(r, uidA, 4, 4000), 0);
    CHECK_EQ(rfid_reader_test_inject(r, uidA, 4, 4800), 0);
    CHECK_EQ(rfid_reader_test_inject(r, uidA, 4, 5500), 0);
    CHECK_EQ(cb_count, 3);

    /* Card removed for >2 s, then re-presented -> fires. */
    CHECK_EQ(rfid_reader_test_inject(r, uidA, 4, 8000), 0);
    CHECK_EQ(cb_count, 4);
}

int main(void)
{
    test_parse_valid_4byte_uid();
    test_parse_valid_7byte_uid();
    test_parse_wire_capture_2026_05_16();
    test_parse_no_card_steady();
    test_parse_no_card_post_session();
    test_parse_bad_xor();
    test_parse_truncated();
    test_parse_payload_with_zeros();
    test_parse_partial_then_complete();
    test_uid_to_hex();
    test_debounce();
    TEST_MAIN_END();
}
