/* meter — replacement personality for stock /root/MeterIC_new.
 *
 * Talks to the meter chip over /dev/ttyAMA2 (2400 8N1) using the
 * `35 LL OO`/`ca LL OO …` register-read/-write protocol decoded in
 * docs/13 §2. Polls Vrms/Irms/P/E at ~1.7 Hz, publishes every result
 * into the same SysV-shmem offsets stock writes so downstream
 * consumers (Pri_Comm, ErrorHandle, web UI, delta-bridge state reader)
 * see no behavior change.
 *
 * Deployed as a personality of `delta-bridge --personality=meter`.
 * The wrapper at /root/MeterIC_new exec's this so /etc/funs starts us
 * in the stock slot.
 *
 * Per docs/12, the kernel UART driver expects the legacy glibc-2.10
 * 44-byte termios; musl's 60-byte struct silently mispacks via
 * tcsetattr(). We bypass tcsetattr and push exact stock bytes via
 * raw ioctl(TCSETS, &t).
 */
#ifndef METER_H
#define METER_H

#include <stdint.h>

#include "shmem.h"

/* Most-recent decoded readings, in chip-raw units. Scaling to V/A/W
 * happens at publish time using cal loaded from /Storage/Gain. */
struct meter_readings {
    /* From `35 02 27` (3-byte response, LE u24) */
    uint32_t vrms_raw;
    /* From `35 02 1c` (3-byte response, LE u24) */
    uint32_t irms_raw;
    /* From `35 03 1a` (4-byte response, LE u32) */
    uint32_t power_raw;
    /* From `35 03 10` (4-byte response, LE u32) */
    uint32_t energy_raw;

    /* True once chip init has completed and at least one full cycle
     * has produced values. */
    int      valid;
};

/* Per-unit calibration loaded from /Storage/Gain. Stock format is:
 *   Vgain:342\nIgain:557\nWgain:3199\n
 * Values are factory-set; bridge reads at boot, never re-writes. */
struct meter_cal {
    int vgain;
    int igain;
    int wgain;
};

/* Run the meter personality forever (until SIGTERM/SIGINT sets *stop).
 * Opens /dev/ttyAMA2, runs the chip-init sequence (docs/13 §2.1),
 * then loops: poll-cycle every ~600 ms; publish each cycle's
 * readings to the shmem offsets stock writes.
 *
 * `port`: UART device path (default /dev/ttyAMA2)
 * `stop`: shared volatile flag; loop exits when set non-zero
 *
 * Returns 0 on clean shutdown, non-zero on unrecoverable error
 * (e.g. shmem attach failed). */
int meter_personality_run(const char *port, volatile int *stop);

/* --- Lower-level helpers, exposed for host tests ----------------- */

/* Parse a raw response buffer for the given query opcode. Returns 0
 * on success and stores the decoded raw value in *out; returns -1 if
 * the response length doesn't match what the opcode requested. */
int meter_parse_response(uint8_t cmd_op, const uint8_t *resp, size_t resp_len,
                         uint32_t *out);

/* Pack the four most-recent raw readings into the SysV-shmem layout
 * stock uses (see docs/13 §4.2 / docs/14 §3 for the offset map):
 *   - 0x0000..0x0001 = u16 LE Vrms scaled / Vgain          [Pri_Comm input]
 *   - 0x0004..0x0005 = u16 LE Vrms scaled / Vgain / 10    [Pri_Comm input]
 *   - 0x000c..0x000f = u32 LE Power scaled / 100          [Pri_Comm input]
 *   - 0x015b..0x017e = 40-byte telemetry block (10× u32)  [main, ErrorHandle inputs]
 *
 * `cal` provides Vgain/Igain/Wgain used to scale raw → engineering
 * units before packing.
 *
 * The shmem segment must be writable (use shmem_attach_rw). */
void meter_publish_shmem(struct shmem *sm, const struct meter_readings *r,
                         const struct meter_cal *cal);

/* Parse /Storage/Gain content into `cal`. Missing file or unparseable
 * content fills cal with defaults (vgain=1, igain=1, wgain=1) and
 * returns -1; otherwise returns 0. */
int meter_load_cal(const char *path, struct meter_cal *cal);

/* The exact 60-byte termios struct stock /root/MeterIC_new pushes
 * via ioctl(TCSETS) — captured from a live trace (docs/13 §2.1).
 * Pushed via raw ioctl(fd, TCSETS, ...) because musl tcsetattr()
 * would mis-pack a 60-byte struct against the kernel's 44-byte
 * layout (docs/12 §2). */
extern const unsigned char METER_STOCK_TERMIOS[60];

#endif /* METER_H */
