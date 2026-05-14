/* src/hal/n32g45x/ws2812.c — STUB. The shared src/hal/ws2812.h interface is
 * not yet implemented for the N32G45x. Present only so the Nexcyber
 * production target links. See src/hal/oevc_hal_stub.h. */
#include "hal/ws2812.h"
#include "oevc_hal_stub.h"

void ws2812_init(void)                                              { OEVC_HAL_STUB(); }
int  ws2812_busy(void)                                              { OEVC_HAL_STUB(); return 0; }
void ws2812_set_pixel(unsigned idx, uint8_t r, uint8_t g, uint8_t b) { OEVC_HAL_STUB(); (void)idx; (void)r; (void)g; (void)b; }
void ws2812_clear(void)                                             { OEVC_HAL_STUB(); }
void ws2812_show(void)                                              { OEVC_HAL_STUB(); }
