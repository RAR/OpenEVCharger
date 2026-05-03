#include "comms_task.h"
#include "safety_task.h"
#include "persist_task.h"
#include "../hal/uart5.h"
#include "../hal/uart.h"
#include "../proto/tlv.h"
#include "../proto/commands.h"
#include "../proto/build_info.h"
#include "../core/system_state.h"
#include "../core/fault.h"
#include "../ui/led_patterns.h"
#include "../ui/buzzer.h"
#include "../diag/stack_watch.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>

/* Hardware advertise cap: spec § 3 & safety_task. Duplicated as a
 * compile-time clamp so SET_ADVERTISED_AMPS validation runs without
 * pulling in the safety_task tick. */
#define COMMS_HW_AMPS_MAX  48u

#define ACCUM_BUF_BYTES   TLV_FRAME_MAX
#define RX_POLL_MS        5U     /* tick rate while idle; bursts drain faster */

static SemaphoreHandle_t    s_tx_mutex;

/* TX serialisation. Multiple producers (comms_task itself, plus
 * comms_publish_event from safety_task) so we mutex around uart5_send.
 * The mutex and stream buffer are created in comms_task_create()
 * pre-scheduler so other tasks can call comms_publish_event() the
 * moment they start running. uart5_init() (and therefore actual byte
 * traffic) only fires once comms_task itself runs, but events posted
 * before that just block briefly on the mutex with nothing competing. */
static int send_frame(uint8_t cmd, uint8_t seq, const void *payload, size_t plen)
{
    if (s_tx_mutex == NULL) return -3;
    uint8_t buf[TLV_FRAME_MAX];
    int n = tlv_build(cmd, seq, payload, plen, buf, sizeof(buf));
    if (n < 0) return -1;

    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return -2;
    (void)uart5_send(buf, (size_t)n);
    xSemaphoreGive(s_tx_mutex);
    return 0;
}

int comms_publish_event(uint8_t cmd, const void *payload, size_t payload_len)
{
    /* Unsolicited events use seq=0 per spec § 5. */
    return send_frame(cmd, 0u, payload, payload_len);
}

int comms_publish_event_seq(uint8_t cmd, uint8_t seq,
                            const void *payload, size_t payload_len)
{
    return send_frame(cmd, seq, payload, payload_len);
}

/* --- Command handlers --------------------------------------------------- */

static void handle_ping(uint8_t seq)
{
    (void)send_frame(EVT_PING_ACK, seq, NULL, 0);
}

static void handle_get_state(uint8_t seq)
{
    struct openbhzd_state s = system_state_snapshot();
    (void)send_frame(EVT_STATE_REPORT, seq, &s, sizeof(s));
}

static void handle_get_build_info(uint8_t seq)
{
    /* Payload format: ASCII "version|sha", null-terminated. */
    char info[48];
    int n = 0;
    const char *v = OPENBHZD_VERSION;
    const char *g = OPENBHZD_GIT_SHA;
    while (*v && n < 23) info[n++] = *v++;
    info[n++] = '|';
    while (*g && n < 46) info[n++] = *g++;
    info[n++] = '\0';
    (void)send_frame(EVT_BUILD_INFO, seq, info, (size_t)n);
}

static void handle_get_device_id(uint8_t seq)
{
    /* GD32F20x UID96 lives at 0x1FFFF7E8 (12 bytes). Per RM
     * "factory-programmed Unique device identifier (96 bits)" —
     * guaranteed unique per die. Used by the FC41D side to derive
     * a stable per-board MAC since the OEM ships a constant MAC
     * across all units. */
    const uint8_t *uid = (const uint8_t *)0x1FFFF7E8u;
    uint8_t buf[12];
    for (int i = 0; i < 12; ++i) buf[i] = uid[i];
    (void)send_frame(EVT_DEVICE_ID, seq, buf, sizeof(buf));
}

static void handle_set_led_override(const uint8_t *p, size_t plen)
{
    /* Payload: u8 mode + u8 r + u8 g + u8 b. mode=0 disables override. */
    if (plen < 4) return;
    led_override_set(p[0], p[1], p[2], p[3]);
}

static void handle_buzzer_beep(const uint8_t *p, size_t plen)
{
    /* Payload: u16 LE ms (capped 1..500 by buzzer module). */
    if (plen < 2) return;
    uint16_t ms = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    buzzer_set_oneshot(ms);
}

static void handle_set_advertised_amps(const uint8_t *p, size_t plen)
{
    /* Payload: u8 amps. Spec § 5: "Clamped to DIP1 + hw cap." DIP1 is
     * sampled by safety_task each tick; we just clamp to the global
     * hardware cap here (48 A) and let safety_task apply the DIP1
     * floor/ceiling via effective_advertised_amps(). */
    if (plen < 1) return;
    uint8_t amps = p[0];
    if (amps > COMMS_HW_AMPS_MAX) amps = COMMS_HW_AMPS_MAX;
    int rc = persist_post_boot_config_amps(amps);
    if (rc != 0) {
        printk("comms: SET_ADVERTISED_AMPS persist post FAIL rc=%d\n", rc);
    } else {
        printk("comms: SET_ADVERTISED_AMPS=%u (queued)\n", (unsigned)amps);
    }
}

static void handle_clear_fault(const uint8_t *p, size_t plen)
{
    /* Payload: u32 LE fault_id; 0 = "all clearable". */
    if (plen < 4) return;
    uint32_t fid = (uint32_t)p[0]        |
                  ((uint32_t)p[1] <<  8) |
                  ((uint32_t)p[2] << 16) |
                  ((uint32_t)p[3] << 24);
    int rc = safety_request_clear_fault(fid);
    if (rc != 0) {
        printk("comms: CLEAR_FAULT inbox post FAIL rc=%d\n", rc);
    }
}

static void handle_get_fault_log(const uint8_t *p, size_t plen, uint8_t seq)
{
    /* Payload: u8 max_count. Persist task does the SPI3 reads + emits
     * EVT_FAULT_LOG_ENTRY chain + EVT_FAULT_LOG_END. */
    uint8_t max_count = (plen >= 1) ? p[0] : 16u;
    int rc = persist_post_get_fault_log(max_count, seq);
    if (rc != 0) {
        printk("comms: GET_FAULT_LOG persist post FAIL rc=%d\n", rc);
    }
}

static void handle_get_lifetime_kwh(uint8_t seq)
{
    int rc = persist_post_get_lifetime_kwh(seq);
    if (rc != 0) {
        printk("comms: GET_LIFETIME_KWH persist post FAIL rc=%d\n", rc);
    }
}

static void handle_write_calibration(const uint8_t *p, size_t plen)
{
    /* Payload (packed LE):
     *   i16 cp_anchor_raw
     *   i16 cp_slope_num
     *   i16 cp_slope_den
     *   (rest reserved for future CT/leakage/NTC trims)
     * Validation: slope_den must be non-zero; reject obviously broken
     * values to keep the bench from hanging on a divide-by-zero or
     * absurd anchor. */
    if (plen < 6) return;
    int16_t anchor = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    int16_t snum   = (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
    int16_t sden   = (int16_t)((uint16_t)p[4] | ((uint16_t)p[5] << 8));
    if (sden == 0) {
        printk("comms: WRITE_CALIBRATION rejected (slope_den=0)\n");
        return;
    }
    if (anchor < 0 || anchor > 4095) {
        printk("comms: WRITE_CALIBRATION rejected (anchor=%d out of 12-bit ADC range)\n",
               (int)anchor);
        return;
    }
    int rc = persist_post_calibration(anchor, snum, sden);
    if (rc != 0) {
        printk("comms: WRITE_CALIBRATION persist post FAIL rc=%d\n", rc);
    } else {
        printk("comms: WRITE_CALIBRATION (anchor=%d num=%d den=%d) queued\n",
               (int)anchor, (int)snum, (int)sden);
    }
}

static void handle_request_stop(const uint8_t *p, size_t plen)
{
    uint8_t reason = (plen >= 1) ? p[0] : 0u;
    (void)safety_request_pause(reason);
}

static void handle_request_start_resume(void)
{
    (void)safety_request_resume();
}

static void dispatch(uint8_t cmd, uint8_t seq,
                     const uint8_t *payload, size_t plen)
{
    switch (cmd) {
    case CMD_PING:                  handle_ping(seq); break;
    case CMD_GET_STATE:             handle_get_state(seq); break;
    case CMD_GET_BUILD_INFO:        handle_get_build_info(seq); break;
    case CMD_GET_DEVICE_ID:         handle_get_device_id(seq); break;
    case CMD_SET_LED_OVERRIDE:      handle_set_led_override(payload, plen); break;
    case CMD_BUZZER_BEEP:           handle_buzzer_beep(payload, plen); break;
    case CMD_SET_ADVERTISED_AMPS:   handle_set_advertised_amps(payload, plen); break;
    case CMD_CLEAR_FAULT:           handle_clear_fault(payload, plen); break;
    case CMD_GET_FAULT_LOG:         handle_get_fault_log(payload, plen, seq); break;
    case CMD_GET_LIFETIME_KWH:      handle_get_lifetime_kwh(seq); break;
    case CMD_WRITE_CALIBRATION:     handle_write_calibration(payload, plen); break;
    case CMD_REQUEST_STOP:          handle_request_stop(payload, plen); break;
    case CMD_REQUEST_START_RESUME:  handle_request_start_resume(); break;
    default:
        printk("comms: unhandled cmd 0x%02x seq=%u plen=%u\n",
               cmd, (unsigned)seq, (unsigned)plen);
        break;
    }
}

/* --- RX accumulator ----------------------------------------------------- */

static void comms_task_run(void *arg)
{
    (void)arg;

    /* RX path comes alive here so byte traffic only flows after the
     * scheduler is running and we're inside the comms_task. uart5
     * peripheral itself was idle before this. */
    uart5_init();

    uint8_t accum[ACCUM_BUF_BYTES];
    size_t  accum_len = 0;

    for (;;) {
        /* Drain whatever the ISR ring has accumulated since the last
         * tick. Idle tick is RX_POLL_MS (= 5 ms); during a burst the
         * inner loop processes back-to-back without yielding. At
         * 115200 a 64-byte TLV frame takes ~5.5 ms so the worst-case
         * dispatch latency is ~10 ms — well under safety's 20 ms. */
        size_t got = uart5_rx_pop(accum + accum_len,
                                  sizeof(accum) - accum_len);
        if (got == 0) {
            vTaskDelay(pdMS_TO_TICKS(RX_POLL_MS));
            continue;
        }
        accum_len += got;

        /* Try to parse from the head; on success consume the frame,
         * on framing error advance one byte and retry, on incomplete
         * keep accumulating. */
        for (;;) {
            uint8_t cmd = 0, seq = 0;
            const uint8_t *payload = NULL;
            size_t payload_len = 0;
            int rc = tlv_parse(accum, accum_len,
                               &cmd, &seq, &payload, &payload_len);
            if (rc > 0) {
                dispatch(cmd, seq, payload, payload_len);
                size_t consumed = (size_t)rc;
                if (accum_len > consumed) {
                    memmove(accum, accum + consumed, accum_len - consumed);
                }
                accum_len -= consumed;
                continue;   /* try the next frame */
            }
            if (rc < 0) {
                /* Framing/CRC error — drop one byte and resync. */
                if (accum_len > 1) {
                    memmove(accum, accum + 1, accum_len - 1);
                }
                accum_len -= 1;
                continue;
            }
            /* rc == 0: need more bytes */
            break;
        }

        /* Defensive: if accum is full and nothing parses, reset.
         * Should never happen given the FRAME_MAX accumulator size. */
        if (accum_len == sizeof(accum)) {
            printk("comms: accumulator full without parse — reset\n");
            accum_len = 0;
        }
    }
}

void comms_task_create(void)
{
    /* TX mutex pre-scheduler so any task can call comms_publish_event()
     * the moment the scheduler starts. RX side uses a lockless ring
     * in uart5.c (no kernel object). */
    s_tx_mutex  = xSemaphoreCreateMutex();
    configASSERT(s_tx_mutex);

    TaskHandle_t h = NULL;
    xTaskCreate(comms_task_run,
                "comms",
                COMMS_TASK_STACK_WORDS,
                NULL,
                COMMS_TASK_PRIORITY,
                &h);
    stack_watch_register("comms", h, COMMS_TASK_STACK_WORDS);
}
