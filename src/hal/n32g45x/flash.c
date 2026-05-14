/* src/hal/n32g45x/flash.c — STUB. The shared src/hal/flash.h interface is
 * not yet implemented for the N32G45x. Present only so the Nexcyber
 * production target links. See src/hal/oevc_hal_stub.h. */
#include "hal/flash.h"
#include "oevc_hal_stub.h"

void flash_copy_ramfunc_to_ram(void)       { OEVC_HAL_STUB(); }
int  flash_apply_pending_ota_image(void)   { OEVC_HAL_STUB(); return 0; }
