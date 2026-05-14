/* src/hal/n32g45x/wdg.c — STUB. The shared src/hal/wdg.h interface is
 * not yet implemented for the N32G45x. Present only so the Nexcyber
 * production target links. See src/hal/oevc_hal_stub.h. */
#include "hal/wdg.h"
#include "oevc_hal_stub.h"

void wdg_init(void)            { OEVC_HAL_STUB(); }
void wdg_kick(void)            { OEVC_HAL_STUB(); }
int  wdg_was_last_reset(void)  { OEVC_HAL_STUB(); return 0; }
