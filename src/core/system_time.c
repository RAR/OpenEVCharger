#include "system_time.h"

static uint32_t s_base_unix_s   = 0;
static uint32_t s_base_tick_ms  = 0;
static int      s_set           = 0;

void system_time_set(uint32_t unix_seconds, uint32_t tick_ms_now)
{
    if (unix_seconds == 0u) {
        /* Caller explicitly clearing — keep is_set=0, leave bases at
         * defaults so a subsequent system_time_now returns 0. */
        s_base_unix_s  = 0u;
        s_base_tick_ms = 0u;
        s_set          = 0;
        return;
    }
    s_base_unix_s  = unix_seconds;
    s_base_tick_ms = tick_ms_now;
    s_set          = 1;
}

uint32_t system_time_now(uint32_t tick_ms_now)
{
    if (!s_set) return 0u;
    /* Unsigned subtraction: tick wrap is automatic. */
    uint32_t delta_ms = tick_ms_now - s_base_tick_ms;
    return s_base_unix_s + (delta_ms / 1000u);
}

int system_time_is_set(void)
{
    return s_set;
}
