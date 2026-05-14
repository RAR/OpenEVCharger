/* N32G45x reset-cause HAL — STUB.
 *
 * The N32 production target is currently a compile/link gate only (see
 * src/hal/oevc_hal_stub.h). The real decode of the Nations RCC reset-
 * source flags is owed by a later task; until then this traps so a
 * debugger breaks in cleanly if it is ever reached at runtime. */

#include "hal/reset_cause.h"
#include "oevc_hal_stub.h"

reset_cause_t reset_cause_get_and_clear(void)
{
    OEVC_HAL_STUB();
    return RESET_CAUSE_UNKNOWN;
}

const char *reset_cause_str(reset_cause_t c)
{
    (void)c;
    OEVC_HAL_STUB();
    return "unknown";
}
