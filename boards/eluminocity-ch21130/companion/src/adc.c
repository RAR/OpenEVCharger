#define _GNU_SOURCE

/* adc — see adc.h for design.
 *
 * Key design choice: classify on a sliding window of 16 samples
 * (~125 ms at 128 Hz). Stock uses 20-byte per-class buffers visible
 * in its .bss (Buf_9V/6V/12V/N_12V) suggesting a similar window
 * size. The benchmark for correctness is "bench idle (CP at -12 V
 * UVP-fault, occasional spikes to +12V) should classify as
 * PS_TRANSIENT" — which is exactly what stock publishes.
 *
 * The two ioctls are bit-for-bit copies of what stock sends; we
 * captured them statically from the rodata section of /root/Adc.
 * The kernel /dev/adc0 driver isn't documented anywhere we can
 * find, so the safe path is just to mimic stock's setup verbatim.
 */
#include "adc.h"

#include "shmem.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* ============================================================
 * Init: two ioctl(0x40104102) calls with 16-byte struct args
 * ============================================================
 * Bytes captured verbatim from /root/Adc .rodata at vaddr 0xabec
 * (first 28 bytes of which 16 land in the first ioctl arg) and
 * the constructed second struct from the stock code:
 *   first  = {1, 0, 2500, 0}    u32 LE   "configure channel 1, rate 2500"?
 *   second = {0, 0, 1, 0}        u32 LE   "enable"?
 *
 * Kernel driver source not available; the exact semantics of these
 * fields are guesses. We just send what stock sends.  */
const unsigned char ADC_INIT_CFG_1[16] = {
    0x01, 0x00, 0x00, 0x00,        /* u32 LE: 1   (channel id?) */
    0x00, 0x00, 0x00, 0x00,        /* u32 LE: 0 */
    0xc4, 0x09, 0x00, 0x00,        /* u32 LE: 2500 (sample rate?) */
    0x00, 0x00, 0x00, 0x00,        /* u32 LE: 0 */
};
const unsigned char ADC_INIT_CFG_2[16] = {
    0x00, 0x00, 0x00, 0x00,        /* u32 LE: 0 */
    0x00, 0x00, 0x00, 0x00,        /* u32 LE: 0 */
    0x01, 0x00, 0x00, 0x00,        /* u32 LE: 1 (enable?) */
    0x00, 0x00, 0x00, 0x00,        /* u32 LE: 0 */
};

/* ============================================================
 * Single-sample classifier
 * ============================================================
 * From the bench idle histogram (docs/13 §3.2): CP at -11.9V → ADC
 * cluster 94..99 (centered ~97), with rare spikes 232..240 (centered
 * ~236) when CP briefly pulls to +12V rail. Linear fit:
 *   slope = (236 - 97) / (12 - (-12)) ≈ 5.79 ADC counts per volt
 *   adc_at_0V ≈ 167
 * J1772 voltage classes (peak voltage, SAE J1772 Table 2):
 *   State A:   +12V        nominal
 *   State B:   +9V
 *   State C:   +6V
 *   State D:   +3V
 *   State F:  -12V         (fault)
 * Translating to ADC counts (with ±1.5V class half-band):
 *   PS_A: adc ≥ 232  (CP ≥ +11.2 V)
 *   PS_B: adc 207..230 (+7..+11)
 *   PS_C: adc 188..207 (+4..+7)
 *   PS_D: adc 170..188 (+1..+4)
 *   PS_F: adc ≤  104 (CP ≤ -11 V)
 *   else: PS_TRANSIENT
 *
 * Margins are generous because the bench's exact ADC-to-volt slope
 * is calibration-dependent and we want robust classification. */
enum pilot_state adc_classify_sample(uint8_t v)
{
    if (v >= 232) return PS_A;
    if (v >= 207) return PS_B;     /* gap 230..232 = transient guard */
    if (v >= 188) return PS_C;
    if (v >= 170) return PS_D;
    if (v <= 104) return PS_F;
    return PS_TRANSIENT;
}

/* ============================================================
 * Window-level state decision
 * ============================================================
 * Counts per-class votes across the window, picks the dominant class
 * iff ≥ 75% of samples agree. If the window is bimodal (mostly rail
 * + rail, like the bench-idle CP-at-low-with-spikes pattern), report
 * PS_TRANSIENT. Otherwise hold the previous state — this dampens
 * transient classification flips during state transitions or noise. */
enum pilot_state adc_window_state(const enum pilot_state *samples,
                                  int n, enum pilot_state prev)
{
    int counts[6] = {0};
    if (n <= 0)
        return prev;
    for (int i = 0; i < n; i++) {
        int s = samples[i];
        if (s >= 0 && s < 6)
            counts[s]++;
    }
    /* 1) Bimodal rail-rail BEFORE dominant — bench idle is 15× PS_F +
     *    1× PS_A; treating that as "stuck low" would be wrong because
     *    stock reports TRANSIENT in the same condition (docs/13 §3.2).
     *    Rule: if BOTH rails (A + F) have any presence and there are
     *    NO mid-class samples, the CP is straddling rails → TRANSIENT.
     *    This dominates the dominant-class rule. */
    int mid_sum = counts[PS_B] + counts[PS_C] + counts[PS_D];
    if (mid_sum == 0 && counts[PS_A] > 0 && counts[PS_F] > 0)
        return PS_TRANSIENT;
    /* 2) Dominant class ≥75% wins (single steady state) */
    int threshold_dom = (n * 3 + 3) / 4;        /* ceil(n * 0.75) */
    for (int s = 0; s < 6; s++) {
        if (s == PS_TRANSIENT)
            continue;
        if (counts[s] >= threshold_dom)
            return (enum pilot_state)s;
    }
    /* 3) Ambiguous — hold previous (hysteresis) */
    return prev;
}

/* ============================================================
 * UART/ADC I/O
 * ============================================================ */

static int adc_open_and_configure(const char *port)
{
    int fd = open(port, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "adc: open(%s): %s\n", port, strerror(errno));
        return -1;
    }
    /* ioctl 0x40104102 = _IOR('A', 2, 16) per docs/13 decode. */
    if (ioctl(fd, 0x40104102, ADC_INIT_CFG_1) != 0) {
        fprintf(stderr, "adc: ioctl(cfg1): %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    if (ioctl(fd, 0x40104102, ADC_INIT_CFG_2) != 0) {
        fprintf(stderr, "adc: ioctl(cfg2): %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* Sleep `ms` ms but wake early on stop. */
static void sleep_ms_stop(int ms, volatile int *stop)
{
    struct timespec ts;
    while (ms > 0 && !(*stop)) {
        int step = ms > 100 ? 100 : ms;
        ts.tv_sec  = step / 1000;
        ts.tv_nsec = (step % 1000) * 1000000L;
        nanosleep(&ts, NULL);
        ms -= step;
    }
}

/* ============================================================
 * Main loop
 * ============================================================ */

#define WINDOW_N 16        /* ~125 ms at 128 Hz */

int adc_personality_run(const char *port, volatile int *stop)
{
    fprintf(stderr, "adc: starting (port=%s)\n", port);

    struct shmem sm;
    memset(&sm, 0, sizeof sm);
    sm.shmid = -1;
    int bo_ms = 200;
    while (!(*stop)) {
        if (shmem_attach_rw(&sm) == 0)
            break;
        fprintf(stderr, "adc: shmem not ready, retry in %d ms\n", bo_ms);
        sleep_ms_stop(bo_ms, stop);
        if (bo_ms < 5000) bo_ms *= 2;
    }
    if (*stop)
        return 0;

    int fd = adc_open_and_configure(port);
    if (fd < 0) {
        shmem_release(&sm);
        return 2;
    }

    enum pilot_state window[WINDOW_N];
    for (int i = 0; i < WINDOW_N; i++) window[i] = PS_TRANSIENT;
    int wi = 0;
    enum pilot_state cur_state = PS_TRANSIENT;
    enum pilot_state last_published = (enum pilot_state)0xff;
    int published_count = 0;

    while (!(*stop)) {
        uint8_t b;
        ssize_t r = read(fd, &b, 1);
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                /* Brief retry; stock saw ~1.9% EAGAIN. Don't burn CPU. */
                struct timespec t = { 0, 1 * 1000000L }; /* 1 ms */
                nanosleep(&t, NULL);
                continue;
            }
            fprintf(stderr, "adc: read(): %s — exiting loop\n",
                    strerror(errno));
            break;
        }
        if (r == 0) {
            /* No data available; back off briefly. */
            struct timespec t = { 0, 5 * 1000000L };
            nanosleep(&t, NULL);
            continue;
        }
        /* Got one sample. Classify, slot into ring. */
        window[wi] = adc_classify_sample(b);
        wi = (wi + 1) % WINDOW_N;

        /* Recompute window state every WINDOW_N samples (each ~125 ms). */
        if (wi == 0) {
            cur_state = adc_window_state(window, WINDOW_N, cur_state);
            if (cur_state != last_published) {
                shmem_write_u8(&sm, 0x0a08, (uint8_t)cur_state);
                last_published = cur_state;
            }
            /* Force a periodic publish so consumers see a fresh value
             * even if the state is stable. Every ~5 s. */
            else if (++published_count >= 40) {
                shmem_write_u8(&sm, 0x0a08, (uint8_t)cur_state);
                published_count = 0;
            }
        }
    }

    fprintf(stderr, "adc: stopping\n");
    close(fd);
    shmem_release(&sm);
    return 0;
}
