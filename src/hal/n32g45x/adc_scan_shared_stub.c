/* src/hal/n32g45x/adc_scan_shared_stub.c — STUB of the shared
 * src/hal/adc_scan.h interface. The N32G45x has a real ADC scan driver,
 * but against a divergent API (adc_scan_nx.h: Nexcyber-specific channel
 * layout). Genuine reconciliation of the two APIs is M5+ future work.
 * Present only so the Nexcyber production target links.
 * See src/hal/oevc_hal_stub.h. */
#include "hal/adc_scan.h"
#include "oevc_hal_stub.h"

void     adc_scan_init(void)                       { OEVC_HAL_STUB(); }
void     adc_scan_latest(uint16_t out[ADC_RANKS])  { OEVC_HAL_STUB(); (void)out; }
uint16_t adc_scan_rank(unsigned rank)              { OEVC_HAL_STUB(); (void)rank; return 0; }
