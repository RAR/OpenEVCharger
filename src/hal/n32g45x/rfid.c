/* src/hal/n32g45x/rfid.c — STUB. The shared src/hal/rfid.h interface is
 * not yet implemented for the N32G45x. Present only so the Nexcyber
 * production target links. See src/hal/oevc_hal_stub.h. */
#include "hal/rfid.h"
#include "oevc_hal_stub.h"

void   rfid_init(void)                         { OEVC_HAL_STUB(); }
size_t rfid_rx_pop(uint8_t *out, size_t cap)   { OEVC_HAL_STUB(); (void)out; (void)cap; return 0; }
void   rfid_send_keepalive(void)               { OEVC_HAL_STUB(); }
