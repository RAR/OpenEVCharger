#include "system_state.h"
#include <string.h>

static volatile struct openbhzd_state s_state;

void system_state_publish(const struct openbhzd_state *s)
{
    /* Word-aligned struct + single writer ⇒ readers see a consistent
     * struct except for ≤ 1 field crossing a tick boundary. Sufficient
     * for telemetry. */
    memcpy((void *)&s_state, s, sizeof(*s));
}

struct openbhzd_state system_state_snapshot(void)
{
    struct openbhzd_state out;
    memcpy(&out, (const void *)&s_state, sizeof(out));
    return out;
}
