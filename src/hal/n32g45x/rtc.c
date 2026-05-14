/* src/hal/n32g45x/rtc.c — STUB. The shared src/hal/rtc.h interface is
 * not yet implemented for the N32G45x. Present only so the Nexcyber
 * production target links. See src/hal/oevc_hal_stub.h. */
#include "hal/rtc.h"
#include "oevc_hal_stub.h"

void rtc_init(void)                        { OEVC_HAL_STUB(); }
int  rtc_load_unix(uint32_t *out)          { OEVC_HAL_STUB(); (void)out; return 0; }
void rtc_store_unix(uint32_t unix_seconds) { OEVC_HAL_STUB(); (void)unix_seconds; }
