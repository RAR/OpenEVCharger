#include "persist_task.h"
#include "comms_task.h"
#include "queue.h"
#include "../hal/uart.h"
#include "../persist/crash_state.h"
#include "../persist/boot_config.h"
#include "../persist/calibration.h"
#include "../proto/commands.h"
#include "../diag/stack_watch.h"
#include <string.h>

typedef enum {
    PERSIST_REQ_EVENT,
    PERSIST_REQ_SESSION,
    PERSIST_REQ_CRASH_STATE_RESET,
    PERSIST_REQ_BOOT_CONFIG_AMPS,
    PERSIST_REQ_CALIBRATION,
    PERSIST_REQ_BL0939_CAL,
    PERSIST_REQ_GET_FAULT_LOG,
    PERSIST_REQ_GET_LIFETIME_KWH,
} persist_req_type_t;

struct __attribute__((packed)) cal_args {
    int16_t anchor_raw;
    int16_t slope_num;
    int16_t slope_den;
};

struct __attribute__((packed)) bl0939_cal_args {
    int16_t v_uv_per_raw;
    int16_t ia_ua_per_raw;
    int16_t ib_ua_per_raw;
    int16_t pa_mw_per_raw;
};

struct __attribute__((packed)) get_fault_log_args {
    uint8_t max_count;
    uint8_t seq;
};

struct persist_req {
    persist_req_type_t type;
    union {
        struct event_record   event;
        struct session_record session;
        uint8_t               amps;
        struct cal_args       cal;
        struct bl0939_cal_args bl0939_cal;
        struct get_fault_log_args fault_log;
        uint8_t               seq;        /* for GET_LIFETIME_KWH */
    } payload;
};

static QueueHandle_t   s_queue;
static volatile uint8_t s_overflow_warned = 0;

static int post(const struct persist_req *req)
{
    if (s_queue == NULL || req == NULL) return -1;
    if (xQueueSend(s_queue, req, 0) != pdTRUE) {
        if (!s_overflow_warned) {
            printk("persist: queue full -- request dropped\n");
            s_overflow_warned = 1;
        }
        return -1;
    }
    return 0;
}

int persist_post_event(const struct event_record *rec)
{
    if (rec == NULL) return -1;
    struct persist_req req;
    req.type = PERSIST_REQ_EVENT;
    memcpy(&req.payload.event, rec, sizeof *rec);
    return post(&req);
}

int persist_post_session(const struct session_record *rec)
{
    if (rec == NULL) return -1;
    struct persist_req req;
    req.type = PERSIST_REQ_SESSION;
    memcpy(&req.payload.session, rec, sizeof *rec);
    return post(&req);
}

int persist_post_crash_state_reset(void)
{
    struct persist_req req;
    req.type = PERSIST_REQ_CRASH_STATE_RESET;
    return post(&req);
}

int persist_post_boot_config_amps(uint8_t amps)
{
    struct persist_req req;
    req.type = PERSIST_REQ_BOOT_CONFIG_AMPS;
    req.payload.amps = amps;
    return post(&req);
}

int persist_post_calibration(int16_t anchor_raw,
                             int16_t slope_num,
                             int16_t slope_den)
{
    struct persist_req req;
    req.type = PERSIST_REQ_CALIBRATION;
    req.payload.cal.anchor_raw = anchor_raw;
    req.payload.cal.slope_num  = slope_num;
    req.payload.cal.slope_den  = slope_den;
    return post(&req);
}

int persist_post_bl0939_cal(int16_t v_uv_per_raw,
                            int16_t ia_ua_per_raw,
                            int16_t ib_ua_per_raw,
                            int16_t pa_mw_per_raw)
{
    struct persist_req req;
    req.type = PERSIST_REQ_BL0939_CAL;
    req.payload.bl0939_cal.v_uv_per_raw  = v_uv_per_raw;
    req.payload.bl0939_cal.ia_ua_per_raw = ia_ua_per_raw;
    req.payload.bl0939_cal.ib_ua_per_raw = ib_ua_per_raw;
    req.payload.bl0939_cal.pa_mw_per_raw = pa_mw_per_raw;
    return post(&req);
}

int persist_post_get_fault_log(uint8_t max_count, uint8_t seq)
{
    struct persist_req req;
    req.type = PERSIST_REQ_GET_FAULT_LOG;
    req.payload.fault_log.max_count = max_count;
    req.payload.fault_log.seq       = seq;
    return post(&req);
}

int persist_post_get_lifetime_kwh(uint8_t seq)
{
    struct persist_req req;
    req.type = PERSIST_REQ_GET_LIFETIME_KWH;
    req.payload.seq = seq;
    return post(&req);
}

/* Walk the event_log ring backwards from head, emit up to max_count
 * valid records as EVT_FAULT_LOG_ENTRY (one record per frame) with the
 * supplied response seq, then a single EVT_FAULT_LOG_END terminator.
 *
 * The log holds 8192 slots; we only ever return the most-recent N (cap
 * 32 so the caller doesn't accidentally drag the comms link for tens of
 * seconds). All reads run inside persist_task — single-owner of SPI3. */
static void handle_get_fault_log(uint8_t max_count, uint8_t seq)
{
    if (max_count == 0u) max_count = 1u;
    if (max_count > 32u) max_count = 32u;

    uint32_t head = event_log_head_slot();
    uint32_t emitted = 0;

    for (uint32_t i = 0; i < EVENT_LOG_TOTAL_RECORDS && emitted < max_count; ++i) {
        uint32_t slot = (head + EVENT_LOG_TOTAL_RECORDS - 1u - i)
                         % EVENT_LOG_TOTAL_RECORDS;
        struct event_record rec;
        int rc = event_log_read_nth(slot, &rec);
        if (rc != 0) continue;   /* blank or corrupt -- skip */
        (void)comms_publish_event_seq(EVT_FAULT_LOG_ENTRY, seq,
                                      &rec, sizeof rec);
        ++emitted;
    }

    /* Terminator carries the count actually emitted so the caller can
     * size its UI list without parsing every record. */
    uint8_t end_payload[2] = { (uint8_t)emitted, max_count };
    (void)comms_publish_event_seq(EVT_FAULT_LOG_END, seq,
                                  end_payload, sizeof end_payload);
}

static void handle_get_lifetime_kwh(uint8_t seq)
{
    uint64_t total_mwh = 0;
    for (uint32_t s = 0; s < SESSION_LOG_TOTAL_RECORDS; ++s) {
        struct session_record rec;
        if (session_log_read_nth(s, &rec) == 0) {
            total_mwh += rec.mwh_delivered;
        }
    }
    if (total_mwh > 0xFFFFFFFFull) total_mwh = 0xFFFFFFFFull;
    uint32_t mwh32 = (uint32_t)total_mwh;
    (void)comms_publish_event_seq(EVT_LIFETIME_KWH, seq,
                                  &mwh32, sizeof mwh32);
}

static void persist_task_run(void *arg)
{
    (void)arg;
    struct persist_req req;
    for (;;) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) == pdTRUE) {
            switch (req.type) {
            case PERSIST_REQ_EVENT:
                if (event_log_append(&req.payload.event) != 0) {
                    printk("persist: event_log_append FAIL\n");
                }
                break;
            case PERSIST_REQ_SESSION:
                if (session_log_append(&req.payload.session) != 0) {
                    printk("persist: session_log_append FAIL\n");
                }
                break;
            case PERSIST_REQ_CRASH_STATE_RESET:
                (void)crash_state_reset_alive();
                break;
            case PERSIST_REQ_BOOT_CONFIG_AMPS:
                (void)boot_config_set_advertised_amps(req.payload.amps);
                break;
            case PERSIST_REQ_CALIBRATION:
                (void)calibration_set_cp(req.payload.cal.anchor_raw,
                                         req.payload.cal.slope_num,
                                         req.payload.cal.slope_den);
                break;
            case PERSIST_REQ_BL0939_CAL:
                (void)calibration_set_bl0939(req.payload.bl0939_cal.v_uv_per_raw,
                                             req.payload.bl0939_cal.ia_ua_per_raw,
                                             req.payload.bl0939_cal.ib_ua_per_raw,
                                             req.payload.bl0939_cal.pa_mw_per_raw);
                break;
            case PERSIST_REQ_GET_FAULT_LOG:
                handle_get_fault_log(req.payload.fault_log.max_count,
                                     req.payload.fault_log.seq);
                break;
            case PERSIST_REQ_GET_LIFETIME_KWH:
                handle_get_lifetime_kwh(req.payload.seq);
                break;
            }
            s_overflow_warned = 0;
        }
    }
}

void persist_task_create(void)
{
    s_queue = xQueueCreate(PERSIST_QUEUE_DEPTH, sizeof(struct persist_req));
    if (s_queue == NULL) {
        printk("persist: xQueueCreate FAIL\n");
        return;
    }
    TaskHandle_t h = NULL;
    xTaskCreate(persist_task_run,
                "persist",
                PERSIST_TASK_STACK_WORDS,
                NULL,
                PERSIST_TASK_PRIORITY,
                &h);
    stack_watch_register("persist", h, PERSIST_TASK_STACK_WORDS);
}
