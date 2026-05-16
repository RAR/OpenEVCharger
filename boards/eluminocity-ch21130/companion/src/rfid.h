/* Custom RFID reader — replaces the stock /root/RFID daemon on /dev/ttyAMA4.
 *
 * Spec source: boards/eluminocity-ch21130/docs/10-rfid-protocol-decoded.md §6.
 * Frame format (verified on the wire 2026-05-16 via LD_PRELOAD shim):
 *
 *   request : [LEN][CMD][ARGS...][XOR]
 *   reply   : [LEN][CMD][PAYLOAD...][XOR]
 *
 *   LEN counts bytes from offset 0 through end of payload (i.e. one less
 *   than the total wire size — XOR sits at index LEN). NO trailing 0x00,
 *   contrary to docs/08's earlier guess. The wire-confirmed examples are:
 *
 *     TX poll              : 03 20 00 23
 *     RX no card (steady)  : 02 df dd
 *     RX no card (post-tx) : 02 be bc
 *     RX 4-byte UID        : 09 20 UU UU UU UU mm mm mm XX  (10 bytes)
 *     RX 7-byte UID        : 0c 20 UU UU UU UU UU UU UU mm mm mm XX  (13 bytes)
 *     RX 10-byte UID       : 0f 20 UU UU UU UU UU UU UU UU UU UU mm mm mm XX
 *
 *   Discriminator for UID length is the LEN byte itself (per the stock
 *   binary's RCV_DATA[0] dispatch in docs/08): 0x09 -> 4, 0x0C -> 7,
 *   0x0F -> 10. Any other LEN is treated as "valid frame, no UID".
 *
 *   Unlike the stock daemon, we do NOT enforce the "DETA" magic prefix
 *   (see docs/10 §5) — by design. The bridge publishes the raw UID and
 *   Home Assistant owns any allowlist policy.
 *
 * Init contract (matches stock /root/RFID exactly, per docs/10 §1):
 *
 *   1. open("/dev/ttyAMA4", O_RDWR | O_NOCTTY)
 *   2. tcsetattr 115200 8N1 raw
 *   3. echo NN > /sys/class/gpio/export ; echo out > .../direction ;
 *      echo V > .../value  — for (NN,V) in (48,1) (57,1) (56,0) (55,0)
 *   4. open("/dev/spr320_pwm1", O_RDWR)
 *   5. write 8 bytes: 9a 5b 06 00 9a 5b 06 00  (period=duty=0x00065B9A)
 *   6. NEVER close the PWM fd — closing triggers a SPEAr3xx kernel-driver
 *      NULL-deref on next open. Hold for process lifetime.
 *
 * Init is skipped when the reader is started via rfid_reader_test_init().
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

/* Start the reader — opens the UART, runs GPIO+PWM init, and arms the
 * poll loop. v0.6 is a true /root/RFID replacement: it owns the UART and
 * the reader peripheral. Do NOT run alongside stock /root/RFID; deploy
 * by replacing /root/RFID with a wrapper that exec's delta-bridge.
 *
 * Args:
 *   port      — UART device path; defaults to "/dev/ttyAMA4" if NULL.
 *   on_scan   — fired on each debounced UID. May be NULL.
 *   user      — opaque pointer passed back to on_scan.
 *
 * Returns 0 on success, -1 on UART open / GPIO / PWM init failure.
 * On success *out points to a process-static reader; caller must NOT free. */
int  rfid_reader_start(struct rfid_reader **out,
                       const char *port,
                       rfid_scan_cb on_scan,
                       void *user);

/* Called from main loop; non-blocking. Drains serial RX, parses any whole
 * frame found, fires on_scan via the debounce rule, and re-issues the
 * poll request when the previous response has been consumed. Returns the
 * number of UIDs delivered this tick (0 in the common no-card case). */
int  rfid_reader_tick(struct rfid_reader *r);

/* Shut down — close serial fd, clear state. Safe on NULL or stopped
 * reader. Does NOT close the PWM fd (closing it would crash the kernel
 * driver per the bug we documented in docs/09/10); the OS reaps it on
 * process exit, which is harmless because the driver was being unloaded
 * anyway. */
void rfid_reader_stop(struct rfid_reader *r);

/* --- testable internals ------------------------------------------------ */

/* Frame parser. Given a buffer that may contain a complete reply frame
 * starting at offset 0, returns:
 *   >0  : frame consumed; return = bytes consumed (= LEN + 1, since the
 *         on-wire frame is LEN bytes of header+payload plus 1 XOR byte
 *         and NO trailing 0). On a valid UID frame (CMD=0x20, LEN in
 *         {0x09, 0x0C, 0x0F}), *uid_len_out is set and uid_out holds the
 *         raw UID bytes. On any other valid frame, *uid_len_out=0.
 *    0  : need more bytes (partial frame).
 *   -1  : framing error — caller should discard one byte and re-sync. */
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

/* Test-only: allocate a reader without opening serial / GPIO / PWM.
 * on_scan is wired but tick() short-circuits because the UART fd stays
 * -1. Returns 0 on success. */
int rfid_reader_test_init(struct rfid_reader **out,
                          rfid_scan_cb on_scan, void *user);

#endif /* RFID_H */
