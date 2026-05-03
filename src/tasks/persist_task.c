#include "persist_task.h"
#include "queue.h"
#include "../hal/uart.h"
#include <string.h>

typedef enum {
    PERSIST_REQ_EVENT,
    PERSIST_REQ_SESSION,
} persist_req_type_t;

struct persist_req {
    persist_req_type_t type;
    union {
        struct event_record   event;
        struct session_record session;
    } payload;
};

static QueueHandle_t   s_queue;
static volatile uint8_t s_overflow_warned = 0;

int persist_post_event(const struct event_record *rec)
{
    if (s_queue == NULL || rec == NULL) return -1;
    struct persist_req req;
    req.type = PERSIST_REQ_EVENT;
    memcpy(&req.payload.event, rec, sizeof *rec);
    if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
        if (!s_overflow_warned) {
            printk("persist: queue full -- events dropped\n");
            s_overflow_warned = 1;
        }
        return -1;
    }
    return 0;
}

int persist_post_session(const struct session_record *rec)
{
    if (s_queue == NULL || rec == NULL) return -1;
    struct persist_req req;
    req.type = PERSIST_REQ_SESSION;
    memcpy(&req.payload.session, rec, sizeof *rec);
    if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
        if (!s_overflow_warned) {
            printk("persist: queue full -- sessions dropped\n");
            s_overflow_warned = 1;
        }
        return -1;
    }
    return 0;
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
    xTaskCreate(persist_task_run,
                "persist",
                PERSIST_TASK_STACK_WORDS,
                NULL,
                PERSIST_TASK_PRIORITY,
                NULL);
}
