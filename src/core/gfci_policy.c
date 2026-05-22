/* gfci_policy_step() is defined as static inline in gfci_policy.h so
 * the firmware inlines it straight into safety_task::check_gfci. This
 * translation unit exists only to give the build system a stable file
 * to compile (and room for any future stateful helpers). Currently a
 * no-op, mirroring core/over_temp.c. */

#include "gfci_policy.h"

/* Force one external reference to the header so editors / static
 * analysers tracking includes don't flag this file as orphaned. */
const unsigned gfci_policy_persist_ticks_c = GFCI_PERSIST_TICKS;
