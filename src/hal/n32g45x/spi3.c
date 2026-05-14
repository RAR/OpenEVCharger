/* src/hal/n32g45x/spi3.c — STUB. The shared src/hal/spi3.h interface is
 * not yet implemented for the N32G45x. Present only so the Nexcyber
 * production target links. See src/hal/oevc_hal_stub.h. */
#include "hal/spi3.h"
#include "oevc_hal_stub.h"

void    spi3_init(void)            { OEVC_HAL_STUB(); }
uint8_t spi3_xfer(uint8_t tx)     { OEVC_HAL_STUB(); (void)tx; return 0; }
void    spi3_cs_assert(void)       { OEVC_HAL_STUB(); }
void    spi3_cs_deassert(void)     { OEVC_HAL_STUB(); }
