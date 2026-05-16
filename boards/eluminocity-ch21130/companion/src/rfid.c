/* delta-bridge — custom RFID reader (replaces /root/RFID).
 *
 * v0.6: real /root/RFID replacement based on the wire-decoded protocol from
 * docs/10. Performs the full GPIO+PWM init that the reader chip needs, then
 * polls Request_CardSN (0x20) and parses the actual on-the-wire frame format
 * (which is [LEN][CMD][PAYLOAD][XOR] — NO trailing 0, contrary to docs/08).
 *
 * Design:
 *  - Single static instance (one reader per process).
 *  - Non-blocking tick(): drain available RX, parse, send next poll when
 *    previous response has been consumed.
 *  - State machine: IDLE -> WAITING -> IDLE on parsed frame or timeout.
 *    Timeout exists only to recover from a hypothetical lost response;
 *    in normal operation the reader always replies.
 *  - Debounce: a fresh UID fires immediately; the same UID re-presented
 *    within 2 s of any sighting is suppressed; >= 2 s gap re-fires.
 *  - PWM fd held open for process lifetime — see the close+reopen kernel
 *    bug discussion in docs/09 §1 and docs/10 §3.
 */

#define _POSIX_C_SOURCE 200112L
#define _GNU_SOURCE

#include "rfid.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define RX_BUF_CAP      128
#define UID_MAX         10            /* longest UID we accept (UltraLight C) */
#define UID_HEX_MAX     (UID_MAX * 2 + 1)
#define DEBOUNCE_MS     2000
#define RESP_TIMEOUT_MS 300

/* PWM init bytes — period=duty=0x00065B9A, little-endian. Matches the exact
 * 8 bytes stock /root/RFID writes after the 12 GPIO setup system() calls.
 * Don't tweak — the reader chip needs this clock to detect cards. */
static const unsigned char PWM_INIT[8] = {
    0x9a, 0x5b, 0x06, 0x00,    /* period_ticks  = 0x00065B9A LE */
    0x9a, 0x5b, 0x06, 0x00,    /* duty_ticks    = 0x00065B9A LE (100 %) */
};

enum rfid_state {
    RFID_IDLE = 0,
    RFID_WAITING_RESP,
};

struct rfid_reader {
    int   fd;                        /* UART fd; -1 in test mode */
    int   pwm_fd;                    /* PWM fd; -1 in test mode or pre-init */
    int   active;

    enum rfid_state state;
    long  last_tx_ms;                /* monotonic ms of last poll request */
    long  last_rx_attempt_ms;        /* timeout reference for WAITING_RESP */

    unsigned char rx[RX_BUF_CAP];
    size_t        rx_len;

    /* Debounce */
    char  last_uid[UID_HEX_MAX];     /* "" = never scanned */
    long  last_seen_ms;              /* monotonic ms of last sighting */

    /* Callback */
    rfid_scan_cb cb;
    void        *user;
};

static struct rfid_reader g_reader;

/* --- time --------------------------------------------------------------- */

static long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* --- parser ------------------------------------------------------------- */

int rfid_parse_frame(const unsigned char *buf, size_t len,
                     unsigned char *uid_out, size_t *uid_len_out)
{
    if (uid_len_out) *uid_len_out = 0;
    if (len < 1)                     return 0;

    unsigned int total = buf[0];     /* LEN byte */
    /* Frame on the wire: [LEN][CMD][PAYLOAD...][XOR]. LEN counts bytes
     * 0..LEN-1 (LEN, CMD, payload). XOR sits at offset LEN. Total wire
     * size = LEN + 1. NO trailing zero (corrects docs/08's guess).
     *
     * Minimum sensible frame is LEN=2 (e.g. "no card" = 02 df dd), where
     * total=2, payload=0, XOR at byte 2. */
    if (total < 2)                   return -1;     /* nonsense LEN */
    if (total + 1 > RX_BUF_CAP)      return -1;     /* never our frame */
    if (len < total + 1)             return 0;      /* need XOR byte too */

    /* XOR check across bytes 0..total-1. */
    unsigned char x = 0;
    for (unsigned int i = 0; i < total; i++)
        x ^= buf[i];
    if (x != buf[total])             return -1;

    /* UID extraction. Per docs/08 / docs/10 §4: for a Request_CardSN
     * reply (CMD=0x20) the LEN byte itself encodes the UID length —
     * 0x09 -> 4 bytes, 0x0C -> 7 bytes, 0x0F -> 10 bytes. UID payload
     * begins immediately after LEN+CMD (offset 2). Any other LEN with
     * CMD=0x20 (e.g. the "no card" replies 02 df / 02 be) is a valid
     * frame that just doesn't carry a UID. */
    if (buf[1] == 0x20 && uid_out && uid_len_out) {
        size_t uid_n = 0;
        switch (total) {
        case 0x09: uid_n = 4;  break;
        case 0x0C: uid_n = 7;  break;
        case 0x0F: uid_n = 10; break;
        default:   uid_n = 0;  break;
        }
        if (uid_n) {
            for (size_t i = 0; i < uid_n; i++)
                uid_out[i] = buf[2 + i];
            *uid_len_out = uid_n;
        }
    }
    return (int)(total + 1);
}

int rfid_uid_to_hex(const unsigned char *uid, size_t uid_len,
                    char *out, size_t out_cap)
{
    static const char H[] = "0123456789ABCDEF";
    if (out_cap < uid_len * 2 + 1) return -1;
    for (size_t i = 0; i < uid_len; i++) {
        out[2 * i]     = H[(uid[i] >> 4) & 0xF];
        out[2 * i + 1] = H[uid[i] & 0xF];
    }
    out[uid_len * 2] = '\0';
    return 0;
}

/* --- request frame builder --------------------------------------------- *
 * Build a [LEN][CMD][ARGS...][XOR] frame into `out`. Returns bytes written.
 * On-wire: NO trailing 0; LEN counts bytes through end of args. */
static size_t build_cmd(unsigned char *out, size_t cap,
                        unsigned char cmd, const unsigned char *args,
                        size_t arglen)
{
    size_t total = 2 + arglen;       /* LEN + CMD + ARGS */
    if (cap < total + 1)  return 0;
    if (total > 0xFF)     return 0;
    out[0] = (unsigned char)total;
    out[1] = cmd;
    for (size_t i = 0; i < arglen; i++)
        out[2 + i] = args[i];
    unsigned char x = 0;
    for (size_t i = 0; i < total; i++)
        x ^= out[i];
    out[total] = x;
    return total + 1;
}

/* --- GPIO + PWM init --------------------------------------------------- *
 * Replicates stock /root/RFID's startup exactly. The shell out via system()
 * matches the static-RE finding (4 GPIOs × 3 shell calls = 12 system()
 * invocations, matching the 12 paired CLOSE fd events the LD_PRELOAD shim
 * captured). The PWM is a direct open+write, not a system() shell call. */

/* Returns 0 always — failures here are logged but non-fatal. If the GPIO
 * is already exported (which is the case if stock /root/RFID was killed
 * earlier in this boot) `echo > export` returns EBUSY, exit nonzero, and
 * we proceed: the GPIO is configured the way we want either way. */
static int reader_gpio_init(void)
{
    struct { int gpio; int value; } pins[] = {
        { 48, 1 }, { 57, 1 }, { 56, 0 }, { 55, 0 },
    };
    char cmd[128];
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        snprintf(cmd, sizeof(cmd),
                 "echo %d > /sys/class/gpio/export 2>/dev/null",
                 pins[i].gpio);
        (void)system(cmd);
        snprintf(cmd, sizeof(cmd),
                 "echo out > /sys/class/gpio/gpio%d/direction 2>/dev/null",
                 pins[i].gpio);
        (void)system(cmd);
        snprintf(cmd, sizeof(cmd),
                 "echo %d > /sys/class/gpio/gpio%d/value 2>/dev/null",
                 pins[i].value, pins[i].gpio);
        (void)system(cmd);
    }
    return 0;
}

/* Open the PWM device and write the 8 magic init bytes. Returns the fd on
 * success (caller must NEVER close it), -1 on failure. */
static int reader_pwm_init(void)
{
    int fd = open("/dev/spr320_pwm1", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "delta-bridge: rfid: open(/dev/spr320_pwm1): %s\n",
                strerror(errno));
        return -1;
    }
    ssize_t w = write(fd, PWM_INIT, sizeof(PWM_INIT));
    if (w != (ssize_t)sizeof(PWM_INIT)) {
        fprintf(stderr,
                "delta-bridge: rfid: PWM init write returned %zd, expected %zu\n",
                w, sizeof(PWM_INIT));
        /* Don't close fd — closing breaks the driver per the bug. Leak
         * the fd and let the process keep running; reader may still work
         * if the previous owner had it primed. */
    }
    return fd;
}

/* --- serial ------------------------------------------------------------- */

/* Stock /root/RFID's exact termios bytes — captured 2026-05-16 via the
 * uart_trace.so shim's TCSETS arg-deref (see docs/12). The kernel UART
 * driver ignores the CBAUD bits and clocks at 115200 regardless; the
 * platform binding is fixed at 115200 by the SPEAr3xx UART driver.
 * Mirroring stock's bytes verbatim is the only way our daemon's UART
 * behaves identically to stock's — every other deviation (IGNPAR,
 * VTIME=0, different VREPRINT) might subtly change RX delivery.
 *
 * Layout (glibc 2.10 ARM, NCCS=19, struct size 44 with c_ispeed/ospeed
 * at offsets 36/40):
 *   c_iflag=0, c_oflag=0, c_cflag=0x000008BE, c_lflag=0, c_line=0,
 *   c_cc[VTIME(5)]=0x0a, c_cc[VMIN(6)]=0, c_cc[VREPRINT(12)]=0x03,
 *   c_cc[17]=0x80, c_cc[18]=0x27, c_ispeed=0x40, c_ospeed=0.
 * (The c_ispeed value 0x40 / 64 baud is meaningless per the driver
 * override; we keep it for verbatim parity with stock.)
 *
 * sizeof(struct termios) under musl is 60 bytes (NCCS=32), not 44 like
 * glibc 2.10 — so we can't simply assign to a `struct termios`. We pack
 * the bytes into a fixed-size buffer and pass to raw ioctl(TCSETS). */
static const unsigned char STOCK_TERMIOS[60] = {
    /* c_iflag */ 0x00, 0x00, 0x00, 0x00,
    /* c_oflag */ 0x00, 0x00, 0x00, 0x00,
    /* c_cflag */ 0xbe, 0x08, 0x00, 0x00,
    /* c_lflag */ 0x00, 0x00, 0x00, 0x00,
    /* c_line  */ 0x00,
    /* c_cc[19]:                                                     */
    /*  0  1  2  3  4 5(VTIME) 6(VMIN)  7  8  9 10 11 12(VREPRINT) 13..18 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x80, 0x27,
    /* c_ispeed */ 0x40, 0x00, 0x00, 0x00,
    /* c_ospeed */ 0x00, 0x00, 0x00, 0x00,
    /* trailing pad (kernel reads only 44 bytes; pad for safety) */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static int open_serial(const char *port)
{
    /* Mirror stock /root/RFID's open exactly: flags=O_RDWR only. Then
     * push stock's verbatim termios via raw TCSETS ioctl — this bypasses
     * the libc tcsetattr() helper which on musl would pack a 60-byte
     * struct against the kernel's expected 44-byte layout. */
    int fd = open(port, O_RDWR);
    if (fd < 0) return -1;
    if (ioctl(fd, TCSETS, STOCK_TERMIOS) != 0) {
        fprintf(stderr, "delta-bridge: rfid: TCSETS: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    /* Flip the fd to non-blocking AFTER the termios is applied so the
     * tick loop's read() returns immediately when the RX buffer is
     * empty. Stock uses VTIME=10 (1 s blocking) but our tick lives in
     * the bigger delta-bridge main loop and can't afford to block. */
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

/* --- debounce + dispatch ------------------------------------------------ */

static void handle_uid(struct rfid_reader *r,
                       const unsigned char *uid, size_t uid_len,
                       long now_ms)
{
    char hex[UID_HEX_MAX];
    if (rfid_uid_to_hex(uid, uid_len, hex, sizeof(hex)) != 0)
        return;

    int fire = 0;
    if (r->last_uid[0] == '\0') {
        fire = 1;                      /* first scan ever */
    } else if (strcmp(hex, r->last_uid) != 0) {
        fire = 1;                      /* different card */
    } else if (now_ms - r->last_seen_ms >= DEBOUNCE_MS) {
        fire = 1;                      /* same card, but card-left-field gap */
    }
    r->last_seen_ms = now_ms;
    if (fire) {
        snprintf(r->last_uid, sizeof(r->last_uid), "%s", hex);
        if (r->cb)
            r->cb(r->user, hex);
    }
}

/* Consume as many complete frames as the RX buffer holds. */
static int drain_frames(struct rfid_reader *r, long now_ms)
{
    int delivered = 0;
    while (r->rx_len > 0) {
        unsigned char uid[UID_MAX];
        size_t uid_len = 0;
        int n = rfid_parse_frame(r->rx, r->rx_len, uid, &uid_len);
        if (n == 0)
            break;                    /* partial frame */
        if (n < 0) {
            /* Bad frame — drop one byte and re-sync. */
            memmove(r->rx, r->rx + 1, r->rx_len - 1);
            r->rx_len--;
            continue;
        }
        if (uid_len > 0) {
            handle_uid(r, uid, uid_len, now_ms);
            delivered++;
        }
        memmove(r->rx, r->rx + n, r->rx_len - (size_t)n);
        r->rx_len -= (size_t)n;
        /* Any complete frame closes the request/response window. */
        r->state = RFID_IDLE;
    }
    return delivered;
}

/* --- public API --------------------------------------------------------- */

int rfid_reader_start(struct rfid_reader **out,
                      const char *port,
                      rfid_scan_cb on_scan,
                      void *user)
{
    if (!out) return -1;
    memset(&g_reader, 0, sizeof(g_reader));
    g_reader.fd      = -1;
    g_reader.pwm_fd  = -1;
    g_reader.cb      = on_scan;
    g_reader.user    = user;
    g_reader.state   = RFID_IDLE;

    const char *p = port ? port : "/dev/ttyAMA4";
    g_reader.fd = open_serial(p);
    if (g_reader.fd < 0) {
        fprintf(stderr, "delta-bridge: rfid: open(%s): %s\n",
                p, strerror(errno));
        return -1;
    }

    /* GPIO + PWM init — runs once, identical to stock's startup. */
    reader_gpio_init();
    g_reader.pwm_fd = reader_pwm_init();
    if (g_reader.pwm_fd < 0) {
        fprintf(stderr,
                "delta-bridge: rfid: PWM init failed — card detection may "
                "not work. Continuing anyway.\n");
    }

    g_reader.active = 1;
    *out = &g_reader;
    fprintf(stderr,
            "delta-bridge: rfid: started on %s @ 115200 8N1, "
            "GPIO + PWM initialized\n", p);
    return 0;
}

int rfid_reader_test_init(struct rfid_reader **out,
                          rfid_scan_cb on_scan, void *user)
{
    if (!out) return -1;
    memset(&g_reader, 0, sizeof(g_reader));
    g_reader.fd      = -1;
    g_reader.pwm_fd  = -1;
    g_reader.cb      = on_scan;
    g_reader.user    = user;
    g_reader.state   = RFID_IDLE;
    g_reader.active  = 1;
    *out = &g_reader;
    return 0;
}

int rfid_reader_test_inject(struct rfid_reader *r,
                            const unsigned char *uid, size_t uid_len,
                            long now_ms)
{
    if (!r || !uid || uid_len == 0 || uid_len > UID_MAX) return -1;
    handle_uid(r, uid, uid_len, now_ms);
    return 0;
}

int rfid_reader_tick(struct rfid_reader *r)
{
    if (!r || !r->active || r->fd < 0)
        return 0;

    long now = mono_ms();

    /* 1. Drain whatever the reader has already pushed at us. */
    for (;;) {
        if (r->rx_len >= sizeof(r->rx)) {
            /* Buffer full — drop one byte to make room. Parser re-syncs
             * on the next iteration. */
            memmove(r->rx, r->rx + 1, r->rx_len - 1);
            r->rx_len--;
        }
        ssize_t n = read(r->fd, r->rx + r->rx_len, sizeof(r->rx) - r->rx_len);
        if (n <= 0) break;
        r->rx_len += (size_t)n;
    }

    int delivered = drain_frames(r, now);

    /* 2. Recover from a stuck WAITING_RESP — the reader should always
     *    reply, but if anything stalls, drop back to IDLE so we can poll
     *    again. */
    if (r->state == RFID_WAITING_RESP &&
        (now - r->last_rx_attempt_ms) >= RESP_TIMEOUT_MS) {
        r->state = RFID_IDLE;
    }

    /* 3. Issue the next poll. No throttle: the reader naturally paces
     *    us at ~110 ms per cycle (see docs/10 §3). */
    if (r->state == RFID_IDLE) {
        unsigned char frame[8];
        unsigned char args[1] = { 0 };
        size_t n = build_cmd(frame, sizeof(frame), 0x20, args, 1);
        if (n > 0) {
            ssize_t w = write(r->fd, frame, n);
            if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                static long last_warn = 0;
                if (now - last_warn > 60000) {
                    fprintf(stderr,
                            "delta-bridge: rfid: write() failed: %s\n",
                            strerror(errno));
                    last_warn = now;
                }
            }
            r->last_tx_ms = now;
            r->last_rx_attempt_ms = now;
            r->state = RFID_WAITING_RESP;
        }
    }
    return delivered;
}

void rfid_reader_stop(struct rfid_reader *r)
{
    if (!r || !r->active) return;
    if (r->fd >= 0) {
        close(r->fd);
        r->fd = -1;
    }
    /* Do NOT close pwm_fd. See docs/09 §1 — closing /dev/spr320_pwm1
     * triggers a kernel NULL-deref on the next open. The OS reaps it on
     * process exit, which is the only safe time to close it. */
    r->active = 0;
    fprintf(stderr, "delta-bridge: rfid: stopped\n");
}
