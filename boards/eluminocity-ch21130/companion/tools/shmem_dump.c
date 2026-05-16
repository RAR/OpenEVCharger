/* shmem_dump — read-only diagnostic for the Delta EVMU30 SysV shmem segment.
 *
 * Pure bench tool, not shipped in DcoFImage. Three modes:
 *
 *   shmem_dump                       Hex dump named offsets + interesting windows.
 *   shmem_dump <seconds>             Watch 0..0x2000 at 10 Hz; type a label +
 *                                    Enter on stdin to inject inline MARK lines
 *                                    that pin physical events to the wall clock.
 *   shmem_dump --save <file>         Snapshot the full 256 KiB segment to file.
 *   shmem_dump --diff <f1> <f2> ...  Compare 2+ snapshot files side-by-side; for
 *                                    every byte whose value differs across the
 *                                    given snapshots, print one row.  Use this
 *                                    after capturing A/B/C state snapshots —
 *                                    the bytes that take *distinct* values per
 *                                    state ARE the state-tracking bytes,
 *                                    wherever they live in the 256 KiB.
 *
 * Typical bench workflow:
 *   ./shmem_dump --save snap-a.bin       # state A (test plug out)
 *   # insert plug, switch to state B
 *   ./shmem_dump --save snap-b.bin
 *   # switch to state C (may not click contactor under 120 V — that's OK)
 *   ./shmem_dump --save snap-c.bin
 *   ./shmem_dump --diff snap-a.bin snap-b.bin snap-c.bin > diff.txt
 *
 * Cross-compile from companion/:
 *   docker run --rm -v "$PWD:/work" -w /work muslcc/x86_64:armv5l-linux-musleabi \
 *     sh -c 'cc -static -O2 -Wall -Wextra -Isrc tools/shmem_dump.c -o tools/shmem_dump'
 */
#define _POSIX_C_SOURCE 200809L

#include "shmem_offsets.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int sig) { (void)sig; g_stop = 1; }

/* ----- shared helpers ------------------------------------------------------ */

static const unsigned char *attach_ro(int *out_id)
{
    int id = shmget(SHMEM_KEY, SHMEM_SIZE, 0);
    if (id < 0) { perror("shmget(0x153E, 0x40000, 0)"); return NULL; }
    void *vp = shmat(id, NULL, SHM_RDONLY);
    if (vp == (void *)-1) { perror("shmat"); return NULL; }
    if (out_id) *out_id = id;
    return (const unsigned char *)vp;
}

static void hex_line(const unsigned char *p, unsigned off, unsigned n)
{
    printf("  %04x:", off);
    for (unsigned i = 0; i < n; i++)
        printf(" %02x", p[off + i]);
    putchar('\n');
}

static void hex_window(const unsigned char *p, unsigned start, unsigned end)
{
    for (unsigned o = start; o < end; o += 16)
        hex_line(p, o, 16);
}

static double monotonic_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ----- one-shot dump (default mode) ---------------------------------------- */

struct named { unsigned off; unsigned width; const char *name; };
static const struct named MAP[] = {
    /* Metering (LE multi-byte) */
    { OFF_VRMS_MEAS,    2, "VRMS_MEAS (u16 LE,  raw/10 = V)" },
    { OFF_IRMS_MEAS,    2, "IRMS_MEAS (u16 LE,  raw/10 = A)" },
    { OFF_POWER_MEAS,   4, "POWER_MEAS (u32 LE, raw/1000)"   },

    /* State cluster */
    { OFF_USER_STATE,   1, "USER_STATE (0=idle,1=auth,2=chg)" },
    { OFF_RED_LED,      1, "RED_LED (0=off,1=solid,2=flash)"  },
    { OFF_PRI_STATE,    1, "PRI_STATE (Pri_Comm digested)"    },
    { OFF_PILOT_STATE,  1, "PILOT_STATE (0..5 = A..F)"        },
    { OFF_STM32_FAULT,  1, "STM32_FAULT (bit 0x10 = UART t/o)"},
    { OFF_PILOT_DUTY,   1, "PILOT_DUTY %"                     },
    { OFF_RATED_AMPS,   1, "RATED_AMPS"                       },

    /* Real alarm bitmap (NOT 0x0138 — that's process-health). */
    { OFF_ALARM_BITMAP, 4, "ALARM_BITMAP (u32 LE)"            },
};

static int cmd_oneshot(void)
{
    int id;
    const unsigned char *p = attach_ro(&id);
    if (!p) return 1;

    puts("== named offsets ==");
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        printf("  @0x%04x  %-20s ", MAP[i].off, MAP[i].name);
        for (unsigned k = 0; k < MAP[i].width; k++)
            printf("%02x ", p[MAP[i].off + k]);
        putchar('\n');
    }
    puts("");
    puts("== hex window 0x0a00 .. 0x0a80 (active state cluster) ==");
    hex_window(p, 0x0a00, 0x0a80);
    puts("");
    puts("== hex window 0x0130 .. 0x0160 (netcfg, not alarms) ==");
    hex_window(p, 0x0130, 0x0160);
    puts("");
    puts("== hex window 0x0bf0 .. 0x0c00 (decode_sharemem 'limit cur' hint) ==");
    hex_window(p, 0x0bf0, 0x0c00);

    shmdt(p);
    return 0;
}

/* ----- watch (existing) ---------------------------------------------------- */

static int cmd_watch(int watch_s)
{
    if (watch_s <= 0) return cmd_oneshot();

    int id;
    const unsigned char *p = attach_ro(&id);
    if (!p) return 1;

    struct sigaction sa = {0};
    sa.sa_handler = on_sig;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Print named offsets and a baseline window so the watch log is self-contained. */
    cmd_oneshot();   /* re-attaches; harmless */

    enum { WATCH_LEN = 0x2000 };
    unsigned char *prev = malloc(WATCH_LEN);
    if (!prev) { perror("malloc"); shmdt(p); return 1; }
    memcpy(prev, p, WATCH_LEN);

    printf("\n== watching 0x0000..0x%04x for %d s @ 10 Hz ==\n", WATCH_LEN, watch_s);
    puts("   Type a label + Enter at any time for an inline MARK.\n");

    int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (fl != -1) fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);

    double t0 = monotonic_s(), next_poll = t0;
    char lbuf[128]; size_t lpos = 0;

    while (!g_stop) {
        double now = monotonic_s();
        if (now - t0 >= watch_s) break;

        double to_next = next_poll - now;
        int timeout_ms = (to_next <= 0) ? 0 : (int)(to_next * 1000);
        if (timeout_ms > 100) timeout_ms = 100;

        struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
        if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN)) {
            char c; ssize_t n;
            while ((n = read(STDIN_FILENO, &c, 1)) == 1) {
                if (c == '\n' || c == '\r') {
                    lbuf[lpos < sizeof(lbuf) ? lpos : sizeof(lbuf) - 1] = '\0';
                    if (lpos > 0)
                        printf("t=%7.3fs  MARK: %s\n", monotonic_s() - t0, lbuf);
                    lpos = 0;
                } else if (lpos + 1 < sizeof(lbuf)) {
                    lbuf[lpos++] = c;
                }
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && fl != -1)
                fcntl(STDIN_FILENO, F_SETFL, fl);
        }

        now = monotonic_s();
        if (now >= next_poll) {
            for (unsigned o = 0; o < WATCH_LEN; o++) {
                if (p[o] != prev[o]) {
                    printf("t=%7.3fs  @%04x: %02x -> %02x\n", now - t0, o, prev[o], p[o]);
                    prev[o] = p[o];
                }
            }
            next_poll += 0.1;
            if (next_poll < now) next_poll = now + 0.1;
        }
    }
    free(prev);
    shmdt(p);
    return 0;
}

/* ----- save ---------------------------------------------------------------- */

static int cmd_save(const char *path)
{
    int id;
    const unsigned char *p = attach_ro(&id);
    if (!p) return 1;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror(path); shmdt(p); return 1; }

    size_t left = SHMEM_SIZE;
    const unsigned char *q = p;
    while (left > 0) {
        ssize_t n = write(fd, q, left);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            perror("write"); close(fd); shmdt(p); return 1;
        }
        q += n; left -= (size_t)n;
    }
    close(fd); shmdt(p);
    fprintf(stderr, "shmem_dump: wrote %d bytes to %s\n", SHMEM_SIZE, path);
    return 0;
}

/* ----- diff ---------------------------------------------------------------- */

static int cmd_diff(int n_files, char **paths)
{
    if (n_files < 2) {
        fprintf(stderr, "shmem_dump --diff: need at least 2 snapshot files\n");
        return 2;
    }

    unsigned char **bufs = calloc((size_t)n_files, sizeof *bufs);
    if (!bufs) { perror("calloc"); return 1; }

    for (int i = 0; i < n_files; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) { perror(paths[i]); return 1; }
        bufs[i] = malloc(SHMEM_SIZE);
        if (!bufs[i]) { perror("malloc"); fclose(f); return 1; }
        size_t got = fread(bufs[i], 1, SHMEM_SIZE, f);
        fclose(f);
        if (got != SHMEM_SIZE) {
            fprintf(stderr, "%s: expected %d bytes, got %zu\n",
                    paths[i], SHMEM_SIZE, got);
            return 1;
        }
    }

    /* Header: column index -> file path. Pasted output is short enough that the
     * eye-mapping is fine. */
    printf("# columns:\n");
    for (int i = 0; i < n_files; i++)
        printf("#   [%d] %s\n", i, paths[i]);
    printf("\n");
    printf("offset  ");
    for (int i = 0; i < n_files; i++)
        printf(" [%d]", i);
    printf("   ascii\n");
    printf("------- ");
    for (int i = 0; i < n_files; i++)
        printf(" ---");
    printf("  ------\n");

    int n_diff = 0;
    for (unsigned off = 0; off < SHMEM_SIZE; off++) {
        unsigned char v0 = bufs[0][off];
        int differs = 0;
        for (int i = 1; i < n_files; i++)
            if (bufs[i][off] != v0) { differs = 1; break; }
        if (!differs) continue;

        printf("0x%05x ", off);
        for (int i = 0; i < n_files; i++)
            printf(" %02x", bufs[i][off]);
        /* ASCII glyph for the first column's value, '.' if non-printable. */
        printf("   %c\n", (v0 >= 32 && v0 < 127) ? v0 : '.');
        n_diff++;
    }
    fprintf(stderr, "\nshmem_dump: %d differing byte%s across %d snapshots\n",
            n_diff, n_diff == 1 ? "" : "s", n_files);

    for (int i = 0; i < n_files; i++) free(bufs[i]);
    free(bufs);
    return 0;
}

/* ----- entry --------------------------------------------------------------- */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s                          one-shot named dump + hex windows\n"
        "       %s <seconds>                watch 0..0x2000 for <seconds>\n"
        "       %s --save <path>            snapshot full 256 KiB to file\n"
        "       %s --diff <f1> <f2> [...]   diff 2+ snapshot files side-by-side\n"
        "       %s --help                   this help\n",
        argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return cmd_oneshot();

    const char *a = argv[1];
    if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
        usage(argv[0]);
        return 0;
    }
    if (!strcmp(a, "--save") || !strcmp(a, "-s")) {
        if (argc < 3) { usage(argv[0]); return 2; }
        return cmd_save(argv[2]);
    }
    if (!strcmp(a, "--diff") || !strcmp(a, "-d")) {
        return cmd_diff(argc - 2, argv + 2);
    }
    /* fallback: numeric => watch seconds */
    int sec = atoi(a);
    if (sec > 0)
        return cmd_watch(sec);

    fprintf(stderr, "shmem_dump: unknown argument '%s'\n", a);
    usage(argv[0]);
    return 2;
}
