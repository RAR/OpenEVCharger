/* src/hal/n32g45x/gfci_shared_stub.c — STUB of the shared src/hal/gfci.h
 * interface. The N32G45x has a real GFCI driver, but against a divergent
 * API (gfci_nx.h: Nexcyber multi-part CT + heartbeat topology). Genuine
 * reconciliation of the two APIs is M5+ future work. Present only so the
 * Nexcyber production target links. See src/hal/oevc_hal_stub.h. */
#include "hal/gfci.h"
#include "oevc_hal_stub.h"

void gfci_init(void)          { OEVC_HAL_STUB(); }
int  gfci_fault_active(void)  { OEVC_HAL_STUB(); return 0; }
int  gfci_self_test(gfci_cal_diag_t *diag)
{
    OEVC_HAL_STUB();
    if (diag) {
        diag->rc = 0;
        diag->pe3_idle_level = 0;
        diag->saw_assert = 0;
        diag->saw_release = 0;
        diag->first_edge_ms = 0;
        diag->release_edge_ms = 0;
    }
    return 0;
}

int  gfci_self_test_inverted(gfci_cal_diag_t *diag)
{
    /* Same shape as the primary stub — Nexcyber's GFCI subsystem uses a
     * different topology (CT + module IC + heartbeat) and the inverted-
     * polarity probe will need its own implementation when that HAL
     * gets wired through. */
    return gfci_self_test(diag);
}
