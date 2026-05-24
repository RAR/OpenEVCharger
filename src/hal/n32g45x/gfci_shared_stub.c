/* src/hal/n32g45x/gfci_shared_stub.c — STUB of the shared src/hal/gfci.h
 * interface. The N32G45x has a real GFCI driver, but against a divergent
 * API (gfci_nx.h: Nexcyber multi-part CT + heartbeat topology). Genuine
 * reconciliation of the two APIs is M5+ future work. Present only so the
 * Nexcyber production target links. See src/hal/oevc_hal_stub.h. */
#include "hal/gfci.h"
#include "oevc_hal_stub.h"

void gfci_init(void)               { OEVC_HAL_STUB(); }
int  gfci_fault_active(void)       { OEVC_HAL_STUB(); return 0; }
int  gfci_in_handshake_window(void){ OEVC_HAL_STUB(); return 0; }
int  gfci_self_test(void)          { OEVC_HAL_STUB(); return 0; }
void gfci_refresh_task(void *arg)  { (void)arg; OEVC_HAL_STUB(); }
