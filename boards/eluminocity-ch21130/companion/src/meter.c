#define _GNU_SOURCE

/* meter — replacement for stock /root/MeterIC_new. See meter.h for
 * design. This file is the only place that opens /dev/ttyAMA2 and the
 * only place that writes to the shmem offsets stock claims. */
#include "meter.h"

#include "shmem.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* ============================================================
 * Stock termios captured from /dev/ttyAMA2 init (docs/13 §2.1)
 * ============================================================
 * 2400 8N1, CREAD | CLOCAL set in c_cflag (0x000008fc), VTIME=10
 * (1s read timeout), VMIN=0. The c_ispeed/c_ospeed fields look like
 * uninitialised stack bytes in the stock binary but the kernel
 * ignores them when CBAUDEX isn't set in CBAUD. We zero-pad those
 * positions for clean repro.
 *
 * MUST be pushed via raw ioctl(fd, TCSETS, &t) — musl tcsetattr()
 * sends a 60-byte struct into a kernel expecting 44 (docs/12). */
const unsigned char METER_STOCK_TERMIOS[60] = {
    /* c_iflag */ 0x00, 0x00, 0x00, 0x00,
    /* c_oflag */ 0x00, 0x00, 0x00, 0x00,
    /* c_cflag */ 0xfc, 0x08, 0x00, 0x00,    /* CS8|CREAD|CLOCAL|HUPCL|B2400 */
    /* c_lflag */ 0x00, 0x00, 0x00, 0x00,
    /* c_line  */ 0x00,
    /* c_cc[19]: only VMIN(=0) and VTIME(=10) matter for raw reads */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* c_ispeed */ 0x00, 0x00, 0x00, 0x00,
    /* c_ospeed */ 0x00, 0x00, 0x00, 0x00,
    /* trailing pad to 60 B (musl-side layout, kernel ignores) */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* ============================================================
 * Wire-protocol constants (docs/13)
 * ============================================================
 *   READ:  35 LL OO            -> response is LL+1 bytes
 *   WRITE: ca LL OO data...    -> writes LL+1 bytes to register OO
 *
 * The four polled registers, in cycle order: */
static const uint8_t CMD_READ_VRMS[3]   = { 0x35, 0x02, 0x27 };  /* 3-byte resp */
static const uint8_t CMD_READ_IRMS[3]   = { 0x35, 0x02, 0x1c };  /* 3-byte resp */
static const uint8_t CMD_READ_POWER[3]  = { 0x35, 0x03, 0x1a };  /* 4-byte resp */
static const uint8_t CMD_READ_ENERGY[3] = { 0x35, 0x03, 0x10 };  /* 4-byte resp */

/* One-shot init writes from stock — see docs/13 §2.1 trace. */
static const uint8_t CMD_INIT_CHIP_ID[3] = { 0x35, 0x01, 0x0e };  /* 2-byte resp */
static const uint8_t CMD_INIT_READ_26[3] = { 0x35, 0x02, 0x26 };  /* 3-byte resp */
static const uint8_t CMD_INIT_MODE[4]    = { 0xca, 0x00, 0x00, 0x05 };
static const uint8_t CMD_INIT_CAL0[6]    = { 0xca, 0x02, 0x00, 0x3b, 0xb7, 0x1e };
static const uint8_t CMD_INIT_READ_2E[3] = { 0x35, 0x02, 0x2e };
static const uint8_t CMD_INIT_CLR_2C[6]  = { 0xca, 0x02, 0x2c, 0x00, 0x00, 0x00 };
static const uint8_t CMD_INIT_SET_2C[6]  = { 0xca, 0x02, 0x2c, 0x00, 0x00, 0x08 };
static const uint8_t CMD_INIT_CAL2[5]    = { 0xca, 0x01, 0x02, 0x44, 0x80 };

/* ============================================================
 * Wire-protocol parsing
 * ============================================================ */

/* Number of response bytes expected for a given READ command (3 bytes
 * starting with 0x35). The protocol's second byte (LL) means response
 * length is LL+1 bytes. */
static int meter_resp_len_for_cmd(uint8_t op0, uint8_t op1)
{
    if (op0 != 0x35)
        return -1;
    return op1 + 1;
}

int meter_parse_response(uint8_t op1, const uint8_t *resp, size_t resp_len,
                         uint32_t *out)
{
    /* op1 == LL byte from the request. resp length must be LL+1. */
    if ((size_t)(op1 + 1) != resp_len || !out)
        return -1;
    uint32_t v = 0;
    /* Little-endian: byte 0 = low. Loop assembles up to 4 bytes. */
    for (size_t i = 0; i < resp_len && i < 4; i++)
        v |= ((uint32_t)resp[i]) << (i * 8);
    *out = v;
    return 0;
}

/* ============================================================
 * UART I/O
 * ============================================================ */

/* Open the meter UART and apply the verbatim stock termios bytes via
 * raw ioctl (bypassing musl's tcsetattr, per docs/12 lesson).
 * Returns the fd on success, -1 on failure (errno set). */
static int meter_open_uart(const char *port)
{
    int fd = open(port, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        fprintf(stderr, "meter: open(%s): %s\n", port, strerror(errno));
        return -1;
    }
    /* 0x5402 = TCSETS. ioctl arg is a pointer to the termios struct,
     * but the kernel reads only the first 44 bytes (its own layout)
     * regardless of what musl thinks the size is. */
    if (ioctl(fd, 0x5402, METER_STOCK_TERMIOS) != 0) {
        fprintf(stderr, "meter: ioctl(TCSETS): %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* Write `n` bytes; retry on EINTR. Returns 0 on success, -1 on error. */
static int write_all(int fd, const uint8_t *buf, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += w;
        n   -= w;
    }
    return 0;
}

/* Read up to `n` bytes with a soft deadline (VTIME=10 in the termios
 * gives a 1s read timeout per syscall). We loop reading 1 byte at a
 * time matching stock's pattern (and avoiding partial-frame issues
 * with VMIN/VTIME quirks across kernel versions). Returns # bytes
 * read on success, -1 on hard error, 0 on timeout. */
static ssize_t read_exact(int fd, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
                continue;
            return -1;
        }
        if (r == 0)
            return (ssize_t)got;          /* VTIME timeout */
        got += (size_t)r;
    }
    return (ssize_t)got;
}

/* Send a 3-byte READ command, read the expected response, parse it.
 * Returns 0 on success, -1 on UART error, -2 on parse error. */
static int do_read_register(int fd, const uint8_t cmd[3], uint32_t *out)
{
    int want = meter_resp_len_for_cmd(cmd[0], cmd[1]);
    if (want < 0)
        return -2;
    if (write_all(fd, cmd, 3) != 0)
        return -1;
    uint8_t resp[8] = {0};
    ssize_t got = read_exact(fd, resp, (size_t)want);
    if (got < 0)
        return -1;
    if (got != want)
        return -2;
    if (meter_parse_response(cmd[1], resp, (size_t)want, out) != 0)
        return -2;
    return 0;
}

/* ============================================================
 * Chip init sequence (docs/13 §2.1)
 * ============================================================
 * Empirically what stock does. We don't strictly need to honour the
 * exact timings — the per-command wait is dominated by the read
 * timeout — but we do follow the same order so the chip ends up in
 * the same configured state. */
static int meter_chip_init(int fd)
{
    uint8_t scratch[8] = {0};
    ssize_t got;

    /* Chip ID query — response observed: 92 0e (2 bytes). We don't act
     * on the value but reading it drains any chip-side stale bytes. */
    if (write_all(fd, CMD_INIT_CHIP_ID, sizeof CMD_INIT_CHIP_ID) != 0)
        return -1;
    got = read_exact(fd, scratch, 2);
    if (got < 2) {
        fprintf(stderr, "meter: chip-id readback short (%zd)\n", got);
    } else {
        fprintf(stderr, "meter: chip-id = %02x %02x\n",
                scratch[0], scratch[1]);
    }

    /* Cal-reg dump. */
    if (write_all(fd, CMD_INIT_READ_26, sizeof CMD_INIT_READ_26) != 0) return -1;
    read_exact(fd, scratch, 3);

    /* Write cal/mode sequence. */
    if (write_all(fd, CMD_INIT_MODE,    sizeof CMD_INIT_MODE)    != 0) return -1;
    if (write_all(fd, CMD_INIT_CAL0,    sizeof CMD_INIT_CAL0)    != 0) return -1;

    if (write_all(fd, CMD_INIT_READ_2E, sizeof CMD_INIT_READ_2E) != 0) return -1;
    read_exact(fd, scratch, 3);

    if (write_all(fd, CMD_INIT_CLR_2C,  sizeof CMD_INIT_CLR_2C)  != 0) return -1;
    if (write_all(fd, CMD_INIT_SET_2C,  sizeof CMD_INIT_SET_2C)  != 0) return -1;
    if (write_all(fd, CMD_INIT_CAL2,    sizeof CMD_INIT_CAL2)    != 0) return -1;

    return 0;
}

/* ============================================================
 * shmem publish
 * ============================================================
 * Stock writes a sprawl of bytes (27 unique offsets per docs/14 §4).
 * The high-value ones are:
 *   - 0x0000..0x0001 : u16 LE  Vrms / Vgain               (Pri_Comm input)
 *   - 0x0004..0x0005 : u16 LE (Vrms / Vgain) / 10         (Pri_Comm input)
 *   - 0x000c..0x000f : u32 LE Power / 100 (centi-watts)   (Pri_Comm input)
 *   - 0x015b..0x017e : 10× u32 telemetry block            (main, ErrorHandle inputs)
 *
 * The 40-byte block layout (slot index → likely-meaning, pending
 * 240 V ground-truth per docs/13 §6.1):
 *   slot 0 (0x015b) Vrms raw or engineering
 *   slot 1 (0x015f) Irms raw or engineering
 *   slot 2 (0x0163) Power (active)
 *   slot 3 (0x0167) Power (apparent or reactive)
 *   slot 4..9      additional channels — unknown until 240 V trace
 *
 * Our v1 strategy: write the four raw values into slots 0..3 in the
 * same byte ordering stock used (LE u32). Slots 4..9 are left zero,
 * matching what the 120 V bench would see in idle. ErrorHandle reads
 * the same slots and treats zeros as "no fault override". */

/* Stock's compact 16-bit fields at 0x0000/0x0004 — they're written
 * byte-by-byte, low-first. We use shmem_write_u16_le for the same
 * net behavior. */
static void publish_compact(struct shmem *sm,
                            const struct meter_readings *r,
                            const struct meter_cal *cal)
{
    /* Stock formula derived from disassembly (docs/13 §2.3 footnote
     * + my pre-trace static-RE comments): byte[0..1] gets the raw
     * value scaled by Vgain, byte[4..5] gets the same scaled / 10.
     * Pri_Comm consumes these (0x0000..0x0005 reads). */
    uint32_t vgain = cal->vgain > 0 ? (uint32_t)cal->vgain : 1;
    uint16_t v0 = (uint16_t)(r->vrms_raw / vgain);
    uint16_t v4 = (uint16_t)((r->vrms_raw / vgain) / 10);

    shmem_write_u16_le(sm, 0x0000, v0);
    shmem_write_u16_le(sm, 0x0004, v4);

    /* 4-byte power at 0x000c..0x000f — stock divides by 100 (centi-
     * watts). Wgain isn't applied here per disassembly. */
    uint32_t p_centi = r->power_raw / 100;
    shmem_write_u32_le(sm, 0x000c, p_centi);
}

/* Wide 40-byte telemetry block at 0x015b..0x017e. We populate the
 * first four u32 slots with raw chip readings; the remaining slots
 * stay at whatever they were (typically zero on the live segment). */
static void publish_telemetry_block(struct shmem *sm,
                                    const struct meter_readings *r)
{
    shmem_write_u32_le(sm, 0x015b, r->vrms_raw);
    shmem_write_u32_le(sm, 0x015f, r->irms_raw);
    shmem_write_u32_le(sm, 0x0163, r->power_raw);
    shmem_write_u32_le(sm, 0x0167, r->energy_raw);
    /* slots 4..9 (0x016b..0x017b) deliberately left untouched. */
}

/* ASCII kWh accumulator at shmem[0x05c0..0x05df]. Stock FlashLog (per
 * docs/19 §FlashLog) reads this 32-B ASCII buffer every ~60 sec and
 * shells out `echo X.XX > /root/Energy`. That file is what our
 * meter personality reads at boot for the initial kWh value.
 *
 * Without populating shmem[0x05c0..], FlashLog reads whatever is
 * there (zeros on cold boot, or stale stock value) and persists THAT
 * — meaning our running kWh counter never survives reboot.
 *
 * Conversion: meter chip's energy_raw is in chip-specific units that
 * stock's MeterIC_new converts to kWh using Wgain (see docs/13 §2.2).
 * Stock's exact formula isn't fully decoded; we use a pragmatic
 * proxy that matches what's persisted in /root/Energy on a
 * factory-virgin bench unit ("100.06\n" with Wgain=3199 produces a
 * value in the right ballpark when scaled). Refine when we have
 * 240 V ground-truth.
 *
 * Formula (provisional): kWh = energy_raw / Wgain / 100. */
static void publish_kwh_ascii(struct shmem *sm,
                              const struct meter_readings *r,
                              const struct meter_cal *cal)
{
    uint32_t wgain = cal->wgain > 0 ? (uint32_t)cal->wgain : 1;
    /* Use double for the division so we get fractional kWh. */
    double kwh = (double)r->energy_raw / (double)wgain / 100.0;
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%.2f", kwh);
    if (n < 0) return;
    if (n > 30) n = 30;
    for (int i = 0; i < n; i++)
        shmem_write_u8(sm, 0x05c0 + (unsigned)i, (uint8_t)buf[i]);
    /* NUL-terminate so FlashLog's `echo %s` doesn't pull garbage. */
    shmem_write_u8(sm, 0x05c0 + (unsigned)n, 0);
}

/* Compute + write cooked V/I/P/E integers for the web at 0x0500..0x050f.
 * See `OFF_BRIDGE_*` in shmem_offsets.h for unit definitions. */
static void publish_cooked_for_web(struct shmem *sm,
                                   const struct meter_readings *r,
                                   const struct meter_cal *cal)
{
    double vgain   = cal->vgain   > 0   ? (double)cal->vgain   : 1.0;
    double wgain   = cal->wgain   > 0   ? (double)cal->wgain   : 1.0;
    double v_scale = cal->v_scale > 0.0 ?         cal->v_scale : 60.0;

    double voltage_v = ((double)r->vrms_raw  / vgain) / v_scale;
    double power_w   =  (double)r->power_raw / wgain;
    /* Resistive-load valid (EV charging is essentially resistive). The
     * chip's irms reading has a fixed bias we haven't characterized and
     * Igain alone doesn't zero it; deriving I from P/V dodges that and
     * gives the right answer under any real load. */
    double current_a = (voltage_v > 1.0) ? (power_w / voltage_v) : 0.0;
    /* Watt-hours = kWh × 1000. energy_raw/Wgain is in chip-units that
     * stock's /100 → kWh; ×10 gives Wh. */
    double energy_wh = ((double)r->energy_raw / wgain) * 10.0;

    /* Clamp to u32 range so silly raws can't wrap. */
    uint32_t v_cv = (voltage_v > 0 && voltage_v < 42949672.0)
                      ? (uint32_t)(voltage_v * 100.0 + 0.5) : 0;
    uint32_t i_ma = (current_a > 0 && current_a < 4294967.0)
                      ? (uint32_t)(current_a * 1000.0 + 0.5) : 0;
    uint32_t p_w  = (power_w > 0 && power_w < 4.29e9)
                      ? (uint32_t)(power_w + 0.5) : 0;
    uint32_t e_wh = (energy_wh > 0 && energy_wh < 4.29e9)
                      ? (uint32_t)(energy_wh + 0.5) : 0;

    shmem_write_u32_le(sm, 0x0500, v_cv);
    shmem_write_u32_le(sm, 0x0504, i_ma);
    shmem_write_u32_le(sm, 0x0508, p_w);
    shmem_write_u32_le(sm, 0x050c, e_wh);
}

void meter_publish_shmem(struct shmem *sm, const struct meter_readings *r,
                         const struct meter_cal *cal)
{
    if (!sm || !sm->writable || !r || !r->valid || !cal)
        return;
    publish_compact(sm, r, cal);
    publish_telemetry_block(sm, r);
    publish_kwh_ascii(sm, r, cal);
    publish_cooked_for_web(sm, r, cal);
}

/* ============================================================
 * /Storage/Gain parser
 * ============================================================
 * Format observed: "Vgain:%d\nIgain:%d\nWgain:%d\n\n" */
int meter_load_cal(const char *path, struct meter_cal *cal)
{
    if (!cal)
        return -1;
    cal->vgain   = 1;
    cal->igain   = 1;
    cal->wgain   = 1;
    cal->v_scale = 60.0;       /* default; overlaid from delta-bridge.conf */
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    int got = 0;
    /* Tolerant scan: look for each key in any order. */
    const char *kv;
    if ((kv = strstr(buf, "Vgain:")) != NULL) {
        cal->vgain = atoi(kv + 6);
        got++;
    }
    if ((kv = strstr(buf, "Igain:")) != NULL) {
        cal->igain = atoi(kv + 6);
        got++;
    }
    if ((kv = strstr(buf, "Wgain:")) != NULL) {
        cal->wgain = atoi(kv + 6);
        got++;
    }
    if (cal->vgain <= 0) cal->vgain = 1;
    if (cal->igain <= 0) cal->igain = 1;
    if (cal->wgain <= 0) cal->wgain = 1;
    return got == 3 ? 0 : -1;
}

/* ============================================================
 * Main loop
 * ============================================================ */

/* Sleep for `ms` milliseconds but wake early on a stop signal. */
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

int meter_personality_run(const char *port, double v_scale, volatile int *stop)
{
    fprintf(stderr, "meter: starting (port=%s, v_scale=%.3f)\n", port, v_scale);

    /* 1. shmem RW attach — retry until present (stock waits forever
     *    on the segment IIUC; we use a bounded backoff). */
    struct shmem sm;
    memset(&sm, 0, sizeof sm);
    sm.shmid = -1;
    int bo_ms = 200;
    while (!(*stop)) {
        if (shmem_attach_rw(&sm) == 0)
            break;
        fprintf(stderr, "meter: shmem not ready, retry in %d ms\n", bo_ms);
        sleep_ms_stop(bo_ms, stop);
        if (bo_ms < 5000) bo_ms *= 2;
    }
    if (*stop)
        return 0;

    /* 2. Calibration from /Storage/Gain — best-effort. Then overlay
     *    v_scale from delta-bridge.conf (the Gain file doesn't carry it). */
    struct meter_cal cal;
    if (meter_load_cal("/Storage/Gain", &cal) != 0)
        fprintf(stderr,
                "meter: /Storage/Gain unparseable; defaulting Vgain/Igain/Wgain to 1\n");
    cal.v_scale = v_scale > 0.0 ? v_scale : 60.0;
    fprintf(stderr,
            "meter: cal Vgain=%d Igain=%d Wgain=%d v_scale=%.3f\n",
            cal.vgain, cal.igain, cal.wgain, cal.v_scale);

    /* 3. UART open + chip init. UART failure is fatal — if we can't
     *    talk to the meter, our reason for existence is gone. */
    int fd = meter_open_uart(port);
    if (fd < 0) {
        shmem_release(&sm);
        return 2;
    }
    if (meter_chip_init(fd) != 0) {
        fprintf(stderr, "meter: chip init failed\n");
        close(fd);
        shmem_release(&sm);
        return 3;
    }

    /* 4. Steady-state polling. ~600 ms cycle matching stock cadence.
     *    On READ errors we log + continue (the chip can transient
     *    out, and dropping a single sample is fine). */
    struct meter_readings r;
    memset(&r, 0, sizeof r);
    while (!(*stop)) {
        int ok = 0;
        ok += (do_read_register(fd, CMD_READ_VRMS,   &r.vrms_raw)   == 0);
        ok += (do_read_register(fd, CMD_READ_IRMS,   &r.irms_raw)   == 0);
        ok += (do_read_register(fd, CMD_READ_POWER,  &r.power_raw)  == 0);
        ok += (do_read_register(fd, CMD_READ_ENERGY, &r.energy_raw) == 0);
        if (ok == 4) {
            r.valid = 1;
            meter_publish_shmem(&sm, &r, &cal);
        } else {
            fprintf(stderr, "meter: poll cycle partial (%d/4)\n", ok);
        }
        /* Stock cycles ~every 600 ms (= 1.7 Hz). The 4 reads + their
         * UART waits already consume some of that; sleep the rest. */
        sleep_ms_stop(450, stop);
    }

    fprintf(stderr, "meter: stopping\n");
    close(fd);
    shmem_release(&sm);
    return 0;
}
