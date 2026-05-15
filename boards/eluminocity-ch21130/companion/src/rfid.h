/* Custom RFID reader — replaces the stock /root/RFID daemon on /dev/ttyAMA4.
 *
 * Spec source: boards/eluminocity-ch21130/docs/08-rfid-auth-flow.md §1.
 * Wire framing per the Chinese-OEM generic serial RFID module the EVMU30
 * ships with:
 *
 *   request : [LEN][CMD][ARGS...][XOR][0x00]
 *   reply   : [LEN][CMD][PAYLOAD...][XOR][0x00]
 *
 *   XOR is over bytes 0..LEN-1 (LEN byte through end of payload, inclusive).
 *   LEN is the total request/reply byte count *excluding* the XOR + trailing
 *   zero — i.e. it equals the offset of the XOR byte from the start.
 *
 * Commands we issue:
 *   - 0x20 (Request_CardSN) — anticollision + UID readout. 5 Hz polled.
 *
 * Reply length-byte dispatch (per the stock RFID binary's RCV_DATA[0] cases):
 *   - 0x09  -> 4-byte UID at payload offset 0..3   (Mifare Classic short)
 *   - 0x0C  -> 7-byte UID at payload offset 0..6   (Mifare Classic 7-byte)
 *   - 0x0F  -> 10-byte UID at payload offset 0..9  (UltraLight C full)
 *   - other -> no-card (no callback)
 *
 * UID is converted to UPPERCASE ASCII hex (no separators), e.g. 4-byte
 * 0x04 0xa1 0xb2 0xc3 -> "04A1B2C3", and a debounce keeps a continuously-
 * held card from firing repeatedly: a new UID fires immediately, the same
 * UID re-presented within 2 s of the last sighting is suppressed.
 *
 * Unlike the stock daemon, we DO NOT enforce the "DETA" magic check on
 * UltraLight page 8 — by design (see docs/08 §1). The bridge ships the raw
 * UID and Home Assistant owns any allowlist policy.
 */
#ifndef RFID_H
#define RFID_H

#include <stddef.h>

/* Opaque to callers — defined in rfid.c. The bridge static-allocates one. */
struct rfid_reader;

/* Callback fired from rfid_reader_tick() when a debounced UID is observed.
 * `uid_hex` is NUL-terminated UPPERCASE hex (no separators). `user` is the
 * opaque pointer passed to rfid_reader_start(). */
typedef void (*rfid_scan_cb)(void *user, const char *uid_hex);

/* Start the reader. Opens `port` at 115200 8N1; if `kill_stock` is non-zero
 * the daemon at /root/RFID is killed first (it would otherwise own the UART).
 * Returns 0 on success, -1 on failure (port open failed). On success
 * `*out` points to a process-static reader; caller must NOT free. */
int  rfid_reader_start(struct rfid_reader **out,
                       const char *port,
                       int kill_stock,
                       rfid_scan_cb on_scan,
                       void *user);

/* Set polling rate in Hz (clamped to [1, 50]). Safe to call any time after
 * start; default is 5 Hz. */
void rfid_reader_set_poll_hz(struct rfid_reader *r, int hz);

/* Called from main loop; non-blocking. Drains serial RX, decides whether
 * to send the next 0x20 frame based on poll cadence, invokes `on_scan` on
 * debounced new UIDs. Returns the number of UIDs delivered this tick
 * (0 in the common no-card-present case). */
int  rfid_reader_tick(struct rfid_reader *r);

/* Shut down gracefully — close serial fd, clear state. Safe on a NULL
 * pointer or already-stopped reader. */
void rfid_reader_stop(struct rfid_reader *r);

/* --- testable internals (linked into the test binary too) ----------------
 * These let test/test_rfid.c exercise the parser + debounce without ever
 * opening a real serial port. */

/* Frame parser. Given a buffer that may contain a complete reply frame
 * starting at offset 0, returns:
 *   >0  : the frame was consumed; return value = bytes consumed
 *          (LEN + 2 — covers the XOR and trailing 0). On a VALID UID frame
 *          (cmd 0x20, reply len byte one of {0x09, 0x0C, 0x0F}),
 *          *uid_len_out is set to the UID byte count and uid_out holds
 *          the raw UID bytes. On any other valid frame, *uid_len_out=0.
 *    0  : need more bytes (partial frame).
 *   -1  : framing error — caller should discard one byte and re-sync.
 *
 * Validates the XOR checksum and the trailing 0x00. */
int rfid_parse_frame(const unsigned char *buf, size_t len,
                     unsigned char *uid_out, size_t *uid_len_out);

/* Convert raw UID bytes to uppercase ASCII hex into `out` (NUL-terminated).
 * `out_cap` must be at least 2*uid_len + 1. Returns 0 on success, -1 on
 * buffer too small. */
int rfid_uid_to_hex(const unsigned char *uid, size_t uid_len,
                    char *out, size_t out_cap);

/* Test-only: inject a parsed UID into the reader, fire on_scan if debounce
 * permits. now_ms is a monotonic-ms timestamp the caller controls; lets
 * tests verify the 2 s "card left field" gap rule deterministically. */
int rfid_reader_test_inject(struct rfid_reader *r,
                            const unsigned char *uid, size_t uid_len,
                            long now_ms);

/* Test-only: allocate a reader without opening serial. on_scan is wired
 * but the file descriptor stays -1, so rfid_reader_tick() will skip I/O.
 * Returns 0 on success. */
int rfid_reader_test_init(struct rfid_reader **out,
                          rfid_scan_cb on_scan, void *user);

#endif /* RFID_H */
