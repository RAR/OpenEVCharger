/* Northbound adapter interface — the seam between the bridge core and whatever
 * it reports to. `mqtt_adapter` is the v1 implementation; an OCPP 1.6-J adapter
 * is a planned second implementation that conforms to this same vtable.
 *
 * Lifecycle:  init() once -> publish_state()/tick() on the poll loop -> shutdown().
 */
#ifndef NORTHBOUND_H
#define NORTHBOUND_H

#include "charger_state.h"

struct northbound {
    /* Opaque per-adapter context. */
    void *ctx;

    /* Bring the adapter up (open sockets, etc.). Returns 0 on success. */
    int  (*init)(struct northbound *nb);

    /* Publish state. `dirty` is the OR of CS_DIRTY_* bits; if `full` is
     * non-zero the adapter must publish every field regardless of `dirty`
     * (used on first connect and after a reconnect). Returns 0 on success. */
    int  (*publish_state)(struct northbound *nb,
                          const struct charger_state *cs,
                          unsigned int dirty, int full);

    /* Called every poll iteration for housekeeping (keepalive, reconnect).
     * Returns 0 normally, non-zero if the adapter wants the loop to treat the
     * link as down (so the next publish_state is called with full=1). */
    int  (*tick)(struct northbound *nb);

    /* Tear down (publish offline, close sockets, free ctx). */
    void (*shutdown)(struct northbound *nb);
};

#endif /* NORTHBOUND_H */
