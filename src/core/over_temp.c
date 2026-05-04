/* over_temp_step() is defined as static inline in over_temp.h so the
 * firmware can inline it directly into safety_task::check_over_temp
 * (avoids the 5-arg frame + non-inlinable call). This translation unit
 * exists so the build system has a stable target file to compile and
 * to keep room for any future stateful helpers (e.g. a runtime mask
 * accessor backed by boot_config). Currently a no-op. */

#include "over_temp.h"

/* Force one external reference to the header so editors / static
 * analysers tracking includes don't flag this file as orphaned. */
const unsigned over_temp_persist_ticks_c = OT_PERSIST_TICKS;
