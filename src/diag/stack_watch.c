#include "stack_watch.h"

#if OPENBHZD_STACK_WATCH

#include "hal/uart.h"

#define STACK_WATCH_MAX 6u

struct stack_watch_entry {
    const char  *name;
    TaskHandle_t handle;
    uint16_t     configured_words;
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
    s_n++;
}

void stack_watch_dump(void)
{
    /* Per-task headroom in stack words (uxTaskGetStackHighWaterMark
     * returns the smallest free-words observed since the task started).
     * Idle task is omitted: configMINIMAL_STACK_SIZE=128 W is generous
     * enough that idle is never the bottleneck, and surfacing its handle
     * would require enabling INCLUDE_xTaskGetIdleTaskHandle. */
    printk("stack:");
    for (uint8_t i = 0; i < s_n; ++i) {
        UBaseType_t lo = uxTaskGetStackHighWaterMark(s_entries[i].handle);
        printk(" %s=%u/%u",
               s_entries[i].name,
               (unsigned)lo,
               (unsigned)s_entries[i].configured_words);
    }
    printk("\n");
}

#endif /* OPENBHZD_STACK_WATCH */
