#include "stack_watch.h"

#if OPENBHZD_STACK_WATCH

#include "hal/uart.h"

#define STACK_WATCH_MAX 6u

struct stack_watch_entry {
    const char  *name;
    TaskHandle_t handle;
    uint16_t     configured_words;
    uint16_t     min_free_seen;     /* smallest free-word reading observed */
};

static struct stack_watch_entry s_entries[STACK_WATCH_MAX];
static uint8_t s_n;

void stack_watch_register(const char *name, TaskHandle_t handle,
                          uint16_t configured_words)
{
    if (s_n >= STACK_WATCH_MAX) return;
    s_entries[s_n].name = name;
    s_entries[s_n].handle = handle;
    s_entries[s_n].configured_words = configured_words;
    s_entries[s_n].min_free_seen = configured_words;
    s_n++;
}

void stack_watch_dump(void)
{
    /* uxTaskGetStackHighWaterMark returns FREE words (smallest free-word
     * reading observed since task start). To make the dump unambiguous —
     * an earlier format printed "<name>=<free>/<total>" which got misread
     * as used/total in persist_task.h — print the derived used count
     * directly, and also track the deepest low-water-mark so far. A new
     * worst-ever fires its own line so a fault-path that bumps usage
     * shows up in logs even between dump cadences. */
    printk("stack:");
    for (uint8_t i = 0; i < s_n; ++i) {
        UBaseType_t free_w = uxTaskGetStackHighWaterMark(s_entries[i].handle);
        uint16_t total = s_entries[i].configured_words;
        uint16_t used  = (free_w >= total) ? 0u : (uint16_t)(total - free_w);
        if ((uint16_t)free_w < s_entries[i].min_free_seen) {
            s_entries[i].min_free_seen = (uint16_t)free_w;
            /* New worst-ever — surface immediately. */
            printk(" [NEW PEAK %s used=%u/%u]",
                   s_entries[i].name, (unsigned)used, (unsigned)total);
        }
        printk(" %s=%u/%u",
               s_entries[i].name, (unsigned)used, (unsigned)total);
    }
    printk("\n");
}

#endif /* OPENBHZD_STACK_WATCH */
