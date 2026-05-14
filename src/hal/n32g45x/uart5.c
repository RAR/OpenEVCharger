/* src/hal/n32g45x/uart5.c — STUB. The shared src/hal/uart5.h interface is
 * not yet implemented for the N32G45x. Present only so the Nexcyber
 * production target links. See src/hal/oevc_hal_stub.h. */
#include "hal/uart5.h"
#include "oevc_hal_stub.h"

void   uart5_init(void)                              { OEVC_HAL_STUB(); }
size_t uart5_rx_pop(uint8_t *out, size_t cap)        { OEVC_HAL_STUB(); (void)out; (void)cap; return 0; }
size_t uart5_send(const void *buf, size_t len)       { OEVC_HAL_STUB(); (void)buf; (void)len; return 0; }
