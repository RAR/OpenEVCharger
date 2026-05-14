/* src/hal/n32g45x/relay_shared_stub.c — STUB of the shared src/hal/relay.h
 * interface. The N32G45x has a real relay driver, but against a divergent
 * API (relay_nx.h: SR-latch close-pulse + hold model). Genuine
 * reconciliation of the two APIs is M5+ future work. Present only so the
 * Nexcyber production target links. See src/hal/oevc_hal_stub.h. */
#include "hal/relay.h"
#include "oevc_hal_stub.h"

void     relay_main_open(void)           { OEVC_HAL_STUB(); }
void     relay_main_close(void)          { OEVC_HAL_STUB(); }
int      relay_main_commanded(void)      { OEVC_HAL_STUB(); return 0; }
int      relay_main_sense_closed(void)   { OEVC_HAL_STUB(); return 0; }
uint16_t relay_main_sense_raw(void)      { OEVC_HAL_STUB(); return 0; }
void     relay_force_open_latch(void)    { OEVC_HAL_STUB(); }
void     relay_force_open_release(void)  { OEVC_HAL_STUB(); }
int      relay_force_open_active(void)   { OEVC_HAL_STUB(); return 0; }
void     relay_aux_open(void)            { OEVC_HAL_STUB(); }
void     relay_aux_close(void)           { OEVC_HAL_STUB(); }
int      relay_aux_commanded(void)       { OEVC_HAL_STUB(); return 0; }
