/* src/hal/n32g45x/adc_inject.c — STUB. The shared src/hal/adc_inject.h
 * interface is not yet implemented for the N32G45x. Present only so the
 * Nexcyber production target links. See src/hal/oevc_hal_stub.h. */
#include "hal/adc_inject.h"
#include "oevc_hal_stub.h"

void     adc_inject_init(void)  { OEVC_HAL_STUB(); }
uint16_t cp_high_raw(void)      { OEVC_HAL_STUB(); return 0; }
int32_t  cp_high_mv(void)       { OEVC_HAL_STUB(); return 0; }
uint16_t cp_low_raw(void)       { OEVC_HAL_STUB(); return 0; }
int32_t  cp_low_mv(void)        { OEVC_HAL_STUB(); return 0; }
