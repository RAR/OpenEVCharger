#include "persist_task.h"
#include "comms_task.h"
#include "queue.h"
#include "../hal/uart.h"
#include "../persist/crash_state.h"
#include "../persist/boot_config.h"
#include "../persist/calibration.h"
#include "../persist/rfid_authlist.h"
#include "../persist/ota_stage.h"
#include "safety_task.h"
#include "../proto/commands.h"
#include "../proto/tlv.h"
#include "../diag/stack_watch.h"
#include <string.h>

/* OTA chunk frames carry up to TLV_PAYLOAD_MAX - (session_id+offset) =
 * 56 - 8 = 48 B of data. The CHUNK queue slot inlines the bytes so the
 * persist_task drains them strictly in order, single-owner of SPI3. */
#define OTA_CHUNK_MAX_DATA   48u

typedef enum {
    PERSIST_REQ_EVENT,
    PERSIST_REQ_SESSION,
    PERSIST_REQ_CRASH_STATE_RESET,
    PERSIST_REQ_BOOT_CONFIG_AMPS,
    PERSIST_REQ_CALIBRATION,
    PERSIST_REQ_BL0939_CAL,
    PERSIST_REQ_GET_FAULT_LOG,
    PERSIST_REQ_GET_LIFETIME_KWH,
    PERSIST_REQ_RFID_AUTHLIST_ADD,
    PERSIST_REQ_RFID_AUTHLIST_REMOVE,
    PERSIST_REQ_RFID_AUTHLIST_CLEAR,
    PERSIST_REQ_RFID_AUTHLIST_GET_LIST,
    PERSIST_REQ_REQUIRE_RFID_AUTH,
    PERSIST_REQ_GFCI_POLICY,
    PERSIST_REQ_OTA_BEGIN,
    PERSIST_REQ_OTA_CHUNK,
    PERSIST_REQ_OTA_COMMIT,
    PERSIST_REQ_OTA_ABORT,
} persist_req_type_t;

struct __attribute__((packed)) cal_args {
    int16_t anchor_raw;
    int16_t slope_num;
    int16_t slope_den;
};

struct __attribute__((packed)) bl0939_cal_args {
    int16_t v_uv_per_raw;
    int16_t ia_na_per_raw;  /* nA/raw (cal v2). FC41D wire payload too. */
    int16_t ib_ua_per_raw;
    int16_t pa_uw_per_raw;
    int32_t freq_const;     /* cal v3. 0 = use compiled default. */
};

struct __attribute__((packed)) get_fault_log_args {
    uint8_t max_count;
    uint8_t seq;
};

struct __attribute__((packed)) ota_begin_args {
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t session_id;
    uint8_t  seq;
};

struct __attribute__((packed)) ota_chunk_args {
    uint32_t session_id;
    uint32_t offset;
    uint8_t  seq;
    uint8_t  data_len;
    uint8_t  data[OTA_CHUNK_MAX_DATA];
};

/* OTA_COMMIT and OTA_ABORT have identical FC41D-side payloads. The
 * persist queue carries the same shape for both; the tag in
 * persist_req.type discriminates. */
struct __attribute__((packed)) ota_abort_args {
    uint32_t session_id;
    uint8_t  seq;
};
typedef struct ota_abort_args ota_commit_args;

struct persist_req {
    persist_req_type_t type;
    union {
        struct event_record   event;
        struct session_record session;
        uint8_t               amps;
        struct cal_args       cal;
        struct bl0939_cal_args bl0939_cal;
        struct get_fault_log_args fault_log;
        uint8_t               seq;        /* for GET_LIFETIME_KWH / GET_LIST */
        uint32_t              rfid_uid;   /* for AUTHLIST_ADD / REMOVE */
        struct ota_begin_args ota_begin;
        struct ota_chunk_args ota_chunk;
        struct ota_abort_args ota_abort;
    } payload;
};

static QueueHandle_t   s_queue;
static volatile uint8_t s_overflow_warned = 0;

/* OTA session state — owned by persist_task. session_id == 0 means
 * "no session active"; FC41D MUST pick a non-zero token. */
static struct {
    uint32_t session_id;
    uint32_t expected_size;
    uint32_t expected_crc32;
    uint32_t next_offset;
    struct ota_crc_ctx running_crc;
} s_ota = { 0 };

static inline int ota_in_session(void) { return s_ota.session_id != 0u; }

static void ota_session_reset(void)
{
    s_ota.session_id     = 0u;
    s_ota.expected_size  = 0u;
    s_ota.expected_crc32 = 0u;
    s_ota.next_offset    = 0u;
    ota_crc_init(&s_ota.running_crc);
}

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

int persist_post_require_rfid_auth(uint8_t enable)
{
    struct persist_req req;
    req.type = PERSIST_REQ_REQUIRE_RFID_AUTH;
    req.payload.amps = enable ? 1u : 0u;   /* re-use the u8 slot */
    return post(&req);
}

int persist_post_gfci_policy(uint8_t policy)
{
    struct persist_req req;
    req.type = PERSIST_REQ_GFCI_POLICY;
    req.payload.amps = policy;   /* re-use the u8 slot */
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
                            int16_t ia_na_per_raw,
                            int16_t ib_ua_per_raw,
                            int16_t pa_uw_per_raw,
                            int32_t freq_const)
{
    struct persist_req req;
    req.type = PERSIST_REQ_BL0939_CAL;
    req.payload.bl0939_cal.v_uv_per_raw  = v_uv_per_raw;
    req.payload.bl0939_cal.ia_na_per_raw = ia_na_per_raw;
    req.payload.bl0939_cal.ib_ua_per_raw = ib_ua_per_raw;
    req.payload.bl0939_cal.pa_uw_per_raw = pa_uw_per_raw;
    req.payload.bl0939_cal.freq_const    = freq_const;
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

int persist_post_rfid_authlist_add(uint32_t uid)
{
    struct persist_req req;
    req.type = PERSIST_REQ_RFID_AUTHLIST_ADD;
    req.payload.rfid_uid = uid;
    return post(&req);
}

int persist_post_rfid_authlist_remove(uint32_t uid)
{
    struct persist_req req;
    req.type = PERSIST_REQ_RFID_AUTHLIST_REMOVE;
    req.payload.rfid_uid = uid;
    return post(&req);
}

int persist_post_rfid_authlist_clear(void)
{
    struct persist_req req;
    req.type = PERSIST_REQ_RFID_AUTHLIST_CLEAR;
    return post(&req);
}

int persist_post_rfid_authlist_get_list(uint8_t seq)
{
    struct persist_req req;
    req.type = PERSIST_REQ_RFID_AUTHLIST_GET_LIST;
    req.payload.seq = seq;
    return post(&req);
}

int persist_post_ota_begin(uint32_t image_size,
                           uint32_t image_crc32,
                           uint32_t session_id,
                           uint8_t  seq)
{
    struct persist_req req;
    req.type = PERSIST_REQ_OTA_BEGIN;
    req.payload.ota_begin.image_size  = image_size;
    req.payload.ota_begin.image_crc32 = image_crc32;
    req.payload.ota_begin.session_id  = session_id;
    req.payload.ota_begin.seq         = seq;
    return post(&req);
}

int persist_post_ota_chunk(uint32_t session_id,
                           uint32_t offset,
                           const uint8_t *data,
                           uint8_t  data_len,
                           uint8_t  seq)
{
    if (data == NULL || data_len == 0 || data_len > OTA_CHUNK_MAX_DATA) {
        return -1;
    }
    struct persist_req req;
    req.type = PERSIST_REQ_OTA_CHUNK;
    req.payload.ota_chunk.session_id = session_id;
    req.payload.ota_chunk.offset     = offset;
    req.payload.ota_chunk.seq        = seq;
    req.payload.ota_chunk.data_len   = data_len;
    memcpy(req.payload.ota_chunk.data, data, data_len);
    /* Zero the tail so memcmp-able regardless of caller's slack. */
    if (data_len < OTA_CHUNK_MAX_DATA) {
        memset(req.payload.ota_chunk.data + data_len, 0,
               OTA_CHUNK_MAX_DATA - data_len);
    }
    return post(&req);
}

int persist_post_ota_commit(uint32_t session_id, uint8_t seq)
{
    struct persist_req req;
    req.type = PERSIST_REQ_OTA_COMMIT;
    req.payload.ota_abort.session_id = session_id;
    req.payload.ota_abort.seq        = seq;
    return post(&req);
}

int persist_post_ota_abort(uint32_t session_id, uint8_t seq)
{
    struct persist_req req;
    req.type = PERSIST_REQ_OTA_ABORT;
    req.payload.ota_abort.session_id = session_id;
    req.payload.ota_abort.seq        = seq;
    return post(&req);
}

/* Walk the cached UID list and emit one EVT_RFID_LIST_ENTRY frame per
 * UID, then a single EVT_RFID_LIST_END terminator. SPI3 isn't actually
 * touched here (the list lives in RAM after rfid_authlist_load), but
 * routing through persist_task keeps the publish logic next to its
 * peers and avoids splitting the helper API. */
static void handle_rfid_get_list(uint8_t seq)
{
    uint8_t count = rfid_authlist_count();
    for (uint8_t i = 0; i < count; ++i) {
        uint32_t uid = 0;
        if (rfid_authlist_get_nth(i, &uid) != 0) continue;
        struct __attribute__((packed)) {
            uint8_t  idx;
            uint8_t  count;
            uint32_t uid;
        } entry = { .idx = i, .count = count, .uid = uid };
        (void)comms_publish_event_seq(EVT_RFID_LIST_ENTRY, seq,
                                      &entry, sizeof entry);
    }
    (void)comms_publish_event_seq(EVT_RFID_LIST_END, seq,
                                  &count, sizeof count);
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

static void handle_ota_begin(const struct ota_begin_args *a)
{
    struct __attribute__((packed)) {
        uint32_t session_id;
        uint8_t  status;
        uint8_t  chunk_size_max;
        uint16_t reserved;
    } ack = {
        .session_id     = a->session_id,
        .chunk_size_max = OTA_CHUNK_MAX_DATA,
        .reserved       = 0,
    };

    if (a->session_id == 0u || a->image_size == 0u) {
        ack.status = OTA_STATUS_INVALID_PAYLOAD;
        goto reply;
    }
    if (a->image_size > OTA_STAGE_MAX_IMAGE_SIZE) {
        ack.status = OTA_STATUS_TOO_LARGE;
        goto reply;
    }

    /* A second BEGIN cancels any in-flight session. We log the override
     * so the FC41D log makes sense if the previous client died mid-stream. */
    if (ota_in_session() && s_ota.session_id != a->session_id) {
        printk("ota: BEGIN preempts session=0x%08x next_off=%u "
               "(new session=0x%08x size=%u)\n",
               (unsigned)s_ota.session_id, (unsigned)s_ota.next_offset,
               (unsigned)a->session_id,    (unsigned)a->image_size);
    }

    int rc = ota_stage_begin(a->image_size);
    if (rc != 0) {
        printk("ota: stage erase FAIL rc=%d size=%u\n",
               rc, (unsigned)a->image_size);
        ack.status = OTA_STATUS_ERASE_FAIL;
        ota_session_reset();
        goto reply;
    }

    ota_session_reset();
    s_ota.session_id     = a->session_id;
    s_ota.expected_size  = a->image_size;
    s_ota.expected_crc32 = a->image_crc32;
    ack.status = OTA_STATUS_OK;
    printk("ota: BEGIN session=0x%08x size=%u crc32=0x%08x\n",
           (unsigned)a->session_id, (unsigned)a->image_size,
           (unsigned)a->image_crc32);

reply:
    (void)comms_publish_event_seq(EVT_OTA_BEGIN_ACK, a->seq,
                                  &ack, sizeof ack);
}

static void handle_ota_chunk(const struct ota_chunk_args *c)
{
    struct __attribute__((packed)) {
        uint32_t session_id;
        uint32_t next_offset;
        uint32_t running_crc32;
        uint8_t  status;
    } ack = {
        .session_id    = c->session_id,
        .next_offset   = s_ota.next_offset,
        .running_crc32 = ota_crc_finalize(&s_ota.running_crc),
        .status        = OTA_STATUS_OK,
    };

    if (!ota_in_session()) {
        ack.status = OTA_STATUS_NO_SESSION;
        goto reply;
    }
    if (c->session_id != s_ota.session_id) {
        ack.status = OTA_STATUS_SESSION_INVALID;
        goto reply;
    }
    if (c->offset != s_ota.next_offset) {
        /* Out-of-order chunk: client should retry from next_offset.
         * Don't roll the running CRC. */
        ack.status = OTA_STATUS_OFFSET_MISMATCH;
        goto reply;
    }
    if ((uint64_t)c->offset + (uint64_t)c->data_len >
        (uint64_t)s_ota.expected_size) {
        ack.status = OTA_STATUS_OVERSIZE;
        goto reply;
    }

    int rc = ota_stage_write(c->offset, c->data, c->data_len);
    if (rc != 0) {
        printk("ota: chunk write FAIL rc=%d off=%u len=%u\n",
               rc, (unsigned)c->offset, (unsigned)c->data_len);
        ack.status = OTA_STATUS_WRITE_ERROR;
        goto reply;
    }

    ota_crc_update(&s_ota.running_crc, c->data, c->data_len);
    s_ota.next_offset = c->offset + c->data_len;
    ack.next_offset   = s_ota.next_offset;
    ack.running_crc32 = ota_crc_finalize(&s_ota.running_crc);

reply:
    (void)comms_publish_event_seq(EVT_OTA_CHUNK_ACK, c->seq,
                                  &ack, sizeof ack);
}

static void handle_ota_commit(const ota_commit_args *c)
{
    struct __attribute__((packed)) {
        uint32_t session_id;
        uint8_t  status;
    } ack = { .session_id = c->session_id };

    if (!ota_in_session()) {
        ack.status = OTA_STATUS_NO_SESSION;
        goto reply;
    }
    if (c->session_id != s_ota.session_id) {
        ack.status = OTA_STATUS_SESSION_INVALID;
        goto reply;
    }
    /* Refuse to commit a partial upload — the FC41D side's bug, not ours.
     * Surface it as OFFSET_MISMATCH so the client retries from
     * next_offset (still carried in the prior CHUNK_ACK). */
    if (s_ota.next_offset != s_ota.expected_size) {
        printk("ota: COMMIT premature: got %u of %u bytes\n",
               (unsigned)s_ota.next_offset,
               (unsigned)s_ota.expected_size);
        ack.status = OTA_STATUS_OFFSET_MISMATCH;
        goto reply;
    }

    /* Re-read from W25Q and recompute CRC. ota_stage_compute_crc reads
     * straight off flash — independent of the running incremental CRC
     * we tracked during the upload, which catches bit-rot in the
     * staging area between write and commit. */
    uint32_t staged_crc = ota_stage_compute_crc(s_ota.expected_size);
    if (staged_crc != s_ota.expected_crc32) {
        printk("ota: COMMIT CRC mismatch: staged=0x%08x expected=0x%08x\n",
               (unsigned)staged_crc, (unsigned)s_ota.expected_crc32);
        ack.status = OTA_STATUS_CRC_MISMATCH;
        goto reply;
    }

    int rc = ota_stage_mark_pending(s_ota.expected_size,
                                    s_ota.expected_crc32);
    if (rc != 0) {
        printk("ota: pending-flag persist FAIL rc=%d\n", rc);
        ack.status = OTA_STATUS_PERSIST_FAIL;
        goto reply;
    }

    printk("ota: COMMIT OK session=0x%08x size=%u crc=0x%08x — "
           "pending flag set; reboot to activate\n",
           (unsigned)c->session_id,
           (unsigned)s_ota.expected_size,
           (unsigned)s_ota.expected_crc32);
    ack.status = OTA_STATUS_OK;
    /* Session state stays around until ABORT or another BEGIN —
     * deliberately, so a duplicate COMMIT is idempotent and a power
     * cycle resumes from the same flag. */

reply:
    (void)comms_publish_event_seq(EVT_OTA_COMMITTED, c->seq,
                                  &ack, sizeof ack);
}

static void handle_ota_abort(const struct ota_abort_args *a)
{
    /* Echo regardless — caller doesn't need to know whether a session
     * was actually live. Reset state if the token matches; otherwise
     * leave any in-flight session alone (a stale ABORT shouldn't kill
     * a fresh BEGIN that arrived before this dispatched). */
    if (ota_in_session() && a->session_id == s_ota.session_id) {
        printk("ota: ABORT session=0x%08x at off=%u\n",
               (unsigned)a->session_id, (unsigned)s_ota.next_offset);
        ota_session_reset();
    }
    struct __attribute__((packed)) {
        uint32_t session_id;
    } ack = { .session_id = a->session_id };
    (void)comms_publish_event_seq(EVT_OTA_ABORTED, a->seq,
                                  &ack, sizeof ack);
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
                                             req.payload.bl0939_cal.ia_na_per_raw,
                                             req.payload.bl0939_cal.ib_ua_per_raw,
                                             req.payload.bl0939_cal.pa_uw_per_raw,
                                             req.payload.bl0939_cal.freq_const);
                break;
            case PERSIST_REQ_GET_FAULT_LOG:
                handle_get_fault_log(req.payload.fault_log.max_count,
                                     req.payload.fault_log.seq);
                break;
            case PERSIST_REQ_GET_LIFETIME_KWH:
                handle_get_lifetime_kwh(req.payload.seq);
                break;
            case PERSIST_REQ_RFID_AUTHLIST_ADD: {
                int rc = rfid_authlist_add(req.payload.rfid_uid);
                printk("rfid_authlist: add 0x%08x -> rc=%d (count=%u)\n",
                       (unsigned)req.payload.rfid_uid, rc,
                       (unsigned)rfid_authlist_count());
                break;
            }
            case PERSIST_REQ_RFID_AUTHLIST_REMOVE: {
                int rc = rfid_authlist_remove(req.payload.rfid_uid);
                printk("rfid_authlist: remove 0x%08x -> rc=%d (count=%u)\n",
                       (unsigned)req.payload.rfid_uid, rc,
                       (unsigned)rfid_authlist_count());
                break;
            }
            case PERSIST_REQ_RFID_AUTHLIST_CLEAR: {
                int rc = rfid_authlist_clear();
                printk("rfid_authlist: clear -> rc=%d\n", rc);
                break;
            }
            case PERSIST_REQ_RFID_AUTHLIST_GET_LIST:
                handle_rfid_get_list(req.payload.seq);
                break;
            case PERSIST_REQ_REQUIRE_RFID_AUTH: {
                int rc = boot_config_set_require_rfid_auth(req.payload.amps);
                if (rc < 0) {
                    printk("persist: require_rfid_auth store FAIL rc=%d\n", rc);
                } else {
                    /* Notify HA via the live config event regardless of
                     * whether the persist write was a no-op. */
                    (void)safety_request_publish_rfid_config();
                }
                break;
            }
            case PERSIST_REQ_GFCI_POLICY: {
                int rc = boot_config_set_gfci_fault_policy(req.payload.amps);
                if (rc < 0) {
                    printk("persist: gfci_policy store FAIL rc=%d\n", rc);
                }
                /* Publish the live policy regardless — on a rejected
                 * value HA still gets the unchanged truth. */
                (void)safety_request_publish_gfci_policy();
                break;
            }
            case PERSIST_REQ_OTA_BEGIN:
                handle_ota_begin(&req.payload.ota_begin);
                break;
            case PERSIST_REQ_OTA_CHUNK:
                handle_ota_chunk(&req.payload.ota_chunk);
                break;
            case PERSIST_REQ_OTA_COMMIT:
                handle_ota_commit(&req.payload.ota_abort);
                break;
            case PERSIST_REQ_OTA_ABORT:
                handle_ota_abort(&req.payload.ota_abort);
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
