#include "comms_task.h"
#include "../hal/uart5.h"
#include "../hal/uart.h"
#include "../proto/tlv.h"
#include "../proto/commands.h"
#include "../proto/build_info.h"
#include "../core/system_state.h"
#include "FreeRTOS.h"
#include "stream_buffer.h"
#include "semphr.h"
#include <string.h>

#define RX_STREAM_BYTES   256U   /* well above one max TLV frame (64 B) */
#define ACCUM_BUF_BYTES   TLV_FRAME_MAX

static StreamBufferHandle_t s_rx_stream;
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

static void dispatch(uint8_t cmd, uint8_t seq,
                     const uint8_t *payload, size_t plen)
{
    (void)payload;
    (void)plen;
    switch (cmd) {
    case CMD_PING:           handle_ping(seq); break;
    case CMD_GET_STATE:      handle_get_state(seq); break;
    case CMD_GET_BUILD_INFO: handle_get_build_info(seq); break;
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
    uart5_init(s_rx_stream);

    uint8_t accum[ACCUM_BUF_BYTES];
    size_t  accum_len = 0;

    for (;;) {
        size_t got = xStreamBufferReceive(
            s_rx_stream,
            accum + accum_len,
            sizeof(accum) - accum_len,
            portMAX_DELAY);
        if (got == 0) continue;
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
    /* Create the synchronisation primitives pre-scheduler so any task
     * (e.g. safety_task) can call comms_publish_event() the moment the
     * scheduler starts. xStreamBufferCreate / xSemaphoreCreateMutex
     * use heap_4 which is safe before vTaskStartScheduler. */
    s_rx_stream = xStreamBufferCreate(RX_STREAM_BYTES, 1);
    s_tx_mutex  = xSemaphoreCreateMutex();
    configASSERT(s_rx_stream && s_tx_mutex);

    xTaskCreate(comms_task_run,
                "comms",
                COMMS_TASK_STACK_WORDS,
                NULL,
                COMMS_TASK_PRIORITY,
                NULL);
}
