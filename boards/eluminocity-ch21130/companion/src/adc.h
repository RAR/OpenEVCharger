/* adc — replacement personality for stock /root/Adc.
 *
 * Reads CP-sense bytes from /dev/adc0 at ~128 Hz and classifies them
 * into a J1772 pilot state (A/B/C/D/transient/F), publishing the
 * one-byte state to shmem[0x0a08] (the PilotState consumer is read
 * by 7+ daemons including LED_control, Pri_Comm, main, CSR).
 *
 * Wire-protocol reverse-engineering: docs/13 §3 (LD_PRELOAD trace
 * of stock) and stock's PilotState() in /root/Adc disassembly:
 *   open("/dev/adc0", O_RDWR)
 *   ioctl(fd, 0x40104102, &cfg1)  ;; cfg1 = {1, 0, 2500, 0} u32 LE
 *   ioctl(fd, 0x40104102, &cfg2)  ;; cfg2 = {0, 0, 1, 0}    u32 LE
 *   loop: read(fd, &byte, 1) at ~128 Hz, classify, publish
 *
 * Calibration assumption (from docs/13 idle histogram):
 *   adc≈97  → CP ≈ -12 V  (low rail)
 *   adc≈236 → CP ≈ +12 V  (high rail)
 *   midpoint ≈ 166, slope ≈ 5.79 ADC counts per volt
 *
 * From these the standard J1772 thresholds become ADC ranges; see
 * adc.c for the table. The full stock PilotState() classifier is
 * larger (sliding-window majority + per-class buffer counters + Alarm
 * flag); our v1 implements an equivalent shape with a smaller window
 * and conservative hysteresis. Bench-validation parity with stock at
 * idle (both should publish state 4 / "transient" on the bench's 120 V
 * UVP fault) is the acceptance test; richer differential testing
 * requires 240 V mains + real plug states.
 */
#ifndef ADC_H
#define ADC_H

#include <stdint.h>

#include "charger_state.h"     /* reuse enum pilot_state defined there */

/* Short aliases for the pilot-state enum so this file's tables stay
 * readable. Mapping matches the byte values stock writes to
 * shmem[0x0a08] (consumed by 7+ daemons; per docs/14 §3 +
 * decode_sharemem.py mapping). */
#define PS_A          PILOT_A
#define PS_B          PILOT_B
#define PS_C          PILOT_C
#define PS_D          PILOT_D
#define PS_TRANSIENT  PILOT_TRANSIENT
#define PS_F          PILOT_F

/* Run the adc personality forever. Opens /dev/adc0, sets up the two
 * channel-config ioctls, polls the byte stream, runs the classifier,
 * publishes one byte (the current pilot state) to shmem[0x0a08].
 *
 * `port`: ADC device path (default /dev/adc0)
 * `stop`: shared volatile flag; loop exits when set non-zero
 *
 * Returns 0 on clean shutdown, non-zero on unrecoverable error. */
int adc_personality_run(const char *port, volatile int *stop);

/* --- Lower-level helpers, exposed for host tests --------------- */

/* Classify a single ADC byte into a coarse voltage class. Returns
 * one of PS_A/PS_B/PS_C/PS_D/PS_F, or PS_TRANSIENT if the value
 * straddles class boundaries (in the "guard band" between classes).
 *
 * This is single-sample classification; the per-window state
 * decision (in adc_window_state) applies majority + hysteresis. */
enum pilot_state adc_classify_sample(uint8_t adc_byte);

/* Combine the last `n` per-sample classifications into a single
 * pilot state. Implements the majority-vote-with-hysteresis used
 * by stock:
 *   - If a single class wins ≥75% of the window, state = that class
 *   - Else if the window is bimodal (rail+rail), state = TRANSIENT
 *   - Else state = previous state (hold during ambiguous windows)
 *
 * `samples` is the array of per-sample states; `prev` is the last
 * state we published (so we can hold during ambiguity). */
enum pilot_state adc_window_state(const enum pilot_state *samples,
                                  int n, enum pilot_state prev);

/* The two 16-byte ioctl arg structs stock sends to /dev/adc0 at
 * init (verbatim from /root/Adc rodata at 0xabec, see docs/17 §2.1). */
extern const unsigned char ADC_INIT_CFG_1[16];
extern const unsigned char ADC_INIT_CFG_2[16];

#endif /* ADC_H */
