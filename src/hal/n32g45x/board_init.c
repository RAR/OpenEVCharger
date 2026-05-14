/* src/hal/n32g45x/board_init.c — N32G45x board hooks. STUB: the production
 * target's boot path is not yet bench-validated on this chip. The bench
 * harness (boards/nexcyber-zbu011k/bench/bringup_main.c) owns the real N32
 * bring-up. See src/hal/oevc_hal_stub.h. */
#include "hal/board_init.h"
#include "oevc_hal_stub.h"

void board_early_init(void)      { OEVC_HAL_STUB(); }
void board_debug_pins_init(void) { OEVC_HAL_STUB(); }
void board_fc41d_release(void)   { OEVC_HAL_STUB(); }
