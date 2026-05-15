/* delta-bridge — custom RFID reader (replaces /root/RFID).
 *
 * Design notes:
 *  - Single static instance (the bridge has exactly one reader). No heap.
 *  - Non-blocking: tick() reads what's available, parses what's buffered,
 *    sends the next request when the inter-poll interval has elapsed.
 *  - State machine: IDLE -> WAITING_RESP -> IDLE (on parsed frame or 300 ms
 *    timeout). The timeout reset is the no-card-present recovery path —
 *    when no tag is in field the reader returns either a "no-card" length
 *    byte (handled as "valid frame, no UID") or simply nothing.
 *  - Debounce rules:
 *      a) UID differs from last_uid          -> fire callback.
 *      b) Same UID but >= 2000 ms since last
 *         observation of ANY card            -> fire callback.
 *      c) Otherwise (same UID, no gap)       -> suppress.
 *    `last_seen_ms` is updated on every valid UID frame, so a continuously-
 *    held card keeps the timer warm and stays suppressed.
 *  - The stock daemon's UltraLight page-8 "DETA" check is deliberately
 *    skipped (docs/08 §1) — we want any ISO14443A tag to work.
 */

#define _POSIX_C_SOURCE 200112L
#define _GNU_SOURCE

#include "rfid.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define RX_BUF_CAP     128
#define UID_MAX        10            /* longest UID we accept (UltraLight C) */
#define UID_HEX_MAX    (UID_MAX * 2 + 1)
#define DEBOUNCE_MS    2000
#define RESP_TIMEOUT_MS 300

enum rfid_state {
    RFID_IDLE = 0,
    RFID_WAITING_RESP,
};

struct rfid_reader {
    int   fd;
    int   poll_hz;                   /* tunable; default 5 */
    int   active;                    /* zeroed by stop, set by start */

    enum rfid_state state;
    long  last_tx_ms;                /* monotonic ms when last 0x20 sent */
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
    if (len < 1)     return 0;
    unsigned int total = buf[0];     /* LEN byte */
    /* Frame structure on the wire: [LEN][CMD][PAYLOAD...][XOR][0x00].
     * LEN counts bytes 0..LEN-1 (i.e. through end of payload). Total frame
     * size on the wire is LEN+2 (XOR + trailing zero). */
    if (total < 2)                   return -1;     /* must have CMD at min */
    if (total + 2 > RX_BUF_CAP)      return -1;     /* never our frame size */
    if (len < total + 2)             return 0;      /* incomplete */

    /* XOR check across bytes 0..total-1. */
    unsigned char x = 0;
    for (unsigned int i = 0; i < total; i++)
        x ^= buf[i];
    if (x != buf[total])             return -1;
    if (buf[total + 1] != 0)         return -1;

    /* Per docs/08: UID replies use the LEN byte itself as the discriminant.
     * The CMD byte for a Request_CardSN reply is the same 0x20 we sent.
     * Valid UID frame lengths: 0x09 (4-byte UID), 0x0C (7-byte), 0x0F (10-byte).
     * Any other length is treated as a non-UID frame (no callback). */
    if (buf[1] == 0x20 && uid_out && uid_len_out) {
        size_t uid_n = 0;
        switch (total) {
        case 0x09: uid_n = 4;  break;
        case 0x0C: uid_n = 7;  break;
        case 0x0F: uid_n = 10; break;
        default:   uid_n = 0;  break;     /* valid frame, just no UID */
        }
        if (uid_n) {
            /* UID payload starts after LEN+CMD = 2 bytes in. */
            for (size_t i = 0; i < uid_n; i++)
                uid_out[i] = buf[2 + i];
            *uid_len_out = uid_n;
        }
    }
    return (int)(total + 2);
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
 * Build a [LEN][CMD][ARGS...][XOR][0] frame into `out`. Returns bytes
 * written. The on-the-wire 0x20 (Request_CardSN) command takes one zero
 * argument byte (per docs/08 §1, "cmd[0]=3, cmd[1]=0x20, cmd[2]=0"). */
static size_t build_cmd(unsigned char *out, size_t cap,
                        unsigned char cmd, const unsigned char *args,
                        size_t arglen)
{
    size_t total = 2 + arglen;   /* LEN + CMD + ARGS */
    if (cap < total + 2)  return 0;
    if (total > 0xFF)     return 0;
    out[0] = (unsigned char)total;
    out[1] = cmd;
    for (size_t i = 0; i < arglen; i++)
        out[2 + i] = args[i];
    unsigned char x = 0;
    for (size_t i = 0; i < total; i++)
        x ^= out[i];
    out[total]     = x;
    out[total + 1] = 0;
    return total + 2;
}

/* --- serial ------------------------------------------------------------- */

static int open_serial(const char *port)
{
    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios t;
    if (tcgetattr(fd, &t) != 0) {
        close(fd);
        return -1;
    }
    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~CRTSCTS;
    t.c_cc[VMIN]  = 0;   /* non-blocking reads */
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

/* Best-effort kill of the stock /root/RFID daemon. We don't care about
 * the exit code — if it wasn't running, killall returns nonzero and that's
 * fine. We do want the log line so an operator can see we tried. */
static void kill_stock_daemon(void)
{
    int rc = system("killall RFID 2>/dev/null");
    fprintf(stderr, "delta-bridge: rfid: killall RFID rc=%d (0=killed, "
                    "1=not running — either is fine)\n",
            WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
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
    /* Always update the last-seen timestamp so a continuously-held card
     * keeps refreshing the debounce window. */
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
            break;                    /* need more bytes */
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
        /* Consume `n` bytes from the buffer. */
        memmove(r->rx, r->rx + n, r->rx_len - (size_t)n);
        r->rx_len -= (size_t)n;
        /* Leaving WAITING_RESP after any complete frame — even non-UID
         * (e.g. "no card") frames close the request/response window. */
        r->state = RFID_IDLE;
    }
    return delivered;
}

/* --- public API --------------------------------------------------------- */

int rfid_reader_start(struct rfid_reader **out,
                      const char *port,
                      int kill_stock,
                      rfid_scan_cb on_scan,
                      void *user)
{
    if (!out) return -1;
    memset(&g_reader, 0, sizeof(g_reader));
    g_reader.fd      = -1;
    g_reader.poll_hz = 5;
    g_reader.cb      = on_scan;
    g_reader.user    = user;
    g_reader.state   = RFID_IDLE;

    if (kill_stock)
        kill_stock_daemon();

    g_reader.fd = open_serial(port ? port : "/dev/ttyAMA4");
    if (g_reader.fd < 0) {
        fprintf(stderr, "delta-bridge: rfid: open(%s) failed: %s\n",
                port ? port : "/dev/ttyAMA4", strerror(errno));
        return -1;
    }

    /* Notes on init:
     *
     * The reader's antenna IS NOT enabled by a UART command. The stock
     * /root/RFID binary contains a `Set_Antana(on)` function that would
     * emit `[03][11][03][XOR][00]`, but that function is dead code — never
     * called from main(). What does enable the antenna is the GPIO+PWM
     * init that stock's `main()` does once at startup (11 echo-to-sysfs
     * system() calls + PWM_Init on /dev/spr320_pwm1). Those run on the
     * unit's boot init, before the reader chip wakes up.
     *
     * That means killing /root/RFID and running our daemon will only work
     * if (a) the GPIO state has survived the stock-daemon death — likely,
     * since sysfs GPIO values are persistent — and (b) the reader chip
     * doesn't require the PWM signal as a live clock — open question,
     * the bench-RE agent's hypothesis was "PWM is buzzer only".
     *
     * If you hit the "card sits on reader, nothing happens" symptom and
     * stock RFID worked at boot, it's most likely option (b): the reader
     * needs the PWM keepalive. The fix would be to open /dev/spr320_pwm1
     * and call PWM_Init ourselves on startup. Be aware: the SPEAr3xx
     * kernel PWM driver has a NULL-deref bug on close+reopen, so once
     * we open it we shouldn't close until the process exits. */

    g_reader.active = 1;
    *out = &g_reader;
    fprintf(stderr, "delta-bridge: rfid: started on %s @ 115200 8N1, "
                    "poll=%d Hz\n",
            port ? port : "/dev/ttyAMA4", g_reader.poll_hz);
    return 0;
}

int rfid_reader_test_init(struct rfid_reader **out,
                          rfid_scan_cb on_scan, void *user)
{
    if (!out) return -1;
    memset(&g_reader, 0, sizeof(g_reader));
    g_reader.fd      = -1;
    g_reader.poll_hz = 5;
    g_reader.cb      = on_scan;
    g_reader.user    = user;
    g_reader.state   = RFID_IDLE;
    g_reader.active  = 1;
    *out = &g_reader;
    return 0;
}

void rfid_reader_set_poll_hz(struct rfid_reader *r, int hz)
{
    if (!r) return;
    if (hz < 1)  hz = 1;
    if (hz > 50) hz = 50;
    r->poll_hz = hz;
}

int rfid_reader_test_inject(struct rfid_reader *r,
                            const unsigned char *uid, size_t uid_len,
                            long now_ms)
{
    if (!r || !uid || uid_len == 0 || uid_len > UID_MAX) return -1;
    /* Tests assert callback firing through their own counter (the cb is
     * wired via rfid_reader_test_init). We just feed handle_uid() the
     * caller's clock so the 2 s gap rule is exercised deterministically. */
    handle_uid(r, uid, uid_len, now_ms);
    return 0;
}

int rfid_reader_tick(struct rfid_reader *r)
{
    if (!r || !r->active || r->fd < 0)
        return 0;

    long now = mono_ms();

    /* 1) Drain whatever the reader has already pushed at us. */
    for (;;) {
        if (r->rx_len >= sizeof(r->rx)) {
            /* Buffer full — drop one byte to make room. The parser will
             * re-sync on the next iteration. */
            memmove(r->rx, r->rx + 1, r->rx_len - 1);
            r->rx_len--;
        }
        ssize_t n = read(r->fd, r->rx + r->rx_len, sizeof(r->rx) - r->rx_len);
        if (n <= 0) break;
        r->rx_len += (size_t)n;
    }

    int delivered = drain_frames(r, now);

    /* 2) Recover from a stuck WAITING_RESP. The reader is silent when no
     *    tag is present so we never actually receive a "no card" frame;
     *    just time the request out and go IDLE so we can re-issue. */
    if (r->state == RFID_WAITING_RESP &&
        (now - r->last_rx_attempt_ms) >= RESP_TIMEOUT_MS) {
        r->state = RFID_IDLE;
    }

    /* 3) Issue the next poll if the inter-poll cadence has elapsed. */
    if (r->state == RFID_IDLE) {
        long period_ms = 1000 / (r->poll_hz > 0 ? r->poll_hz : 5);
        if (period_ms < 1) period_ms = 1;
        if (r->last_tx_ms == 0 || (now - r->last_tx_ms) >= period_ms) {
            unsigned char frame[8];
            unsigned char args[1] = { 0 };
            size_t n = build_cmd(frame, sizeof(frame), 0x20, args, 1);
            if (n > 0) {
                ssize_t w = write(r->fd, frame, n);
                if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    /* I/O hiccup — log once per minute at most. The reader
                     * sometimes resets briefly on plug-in noise; not fatal. */
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
    r->active = 0;
    fprintf(stderr, "delta-bridge: rfid: stopped\n");
}
