#include "rfid_authlist.h"
#include "pingpong.h"
#include "../hal/uart.h"
#include <string.h>

static struct rfid_authlist_record s_rec;

static int store(void)
{
    s_rec.version = RFID_AUTHLIST_VERSION;
    if (s_rec.count > RFID_AUTHLIST_MAX) s_rec.count = RFID_AUTHLIST_MAX;
    /* Zero unused slots so the persisted image is deterministic and
     * a future format extension that reuses the trailing slots can
     * tell "never written" from "deliberate zero". */
    for (uint8_t i = s_rec.count; i < RFID_AUTHLIST_MAX; ++i) {
        s_rec.uids[i] = 0;
    }
    uint8_t  slot = 0;
    uint32_t counter = 0;
    int rc = pingpong_store(RFID_AUTHLIST_SLOT_A, RFID_AUTHLIST_SLOT_B,
                            &s_rec, sizeof s_rec, &slot, &counter);
    if (rc < 0) {
        printk("rfid_authlist: store FAIL rc=%d\n", rc);
        return rc;
    }
    printk("rfid_authlist: stored -> slot %c (counter=%u, count=%u)\n",
           'A' + slot, (unsigned)counter, (unsigned)s_rec.count);
    return 0;
}

int rfid_authlist_load(void)
{
    uint8_t  slot = 0;
    uint32_t counter = 0;
    int rc = pingpong_load(RFID_AUTHLIST_SLOT_A, RFID_AUTHLIST_SLOT_B,
                           &s_rec, sizeof s_rec, &slot, &counter);
    if (rc < 0) {
        printk("rfid_authlist: pingpong_load FAIL rc=%d\n", rc);
        return rc;
    }
    if (rc == 1) {
        memset(&s_rec, 0, sizeof s_rec);
        s_rec.version = RFID_AUTHLIST_VERSION;
        s_rec.count = 0;
        rc = store();
        if (rc < 0) return rc;
        printk("rfid_authlist: empty defaults written\n");
        return 0;
    }
    if (s_rec.version != RFID_AUTHLIST_VERSION) {
        printk("rfid_authlist: unknown version=%u, using as-is\n",
               (unsigned)s_rec.version);
    }
    if (s_rec.count > RFID_AUTHLIST_MAX) {
        printk("rfid_authlist: corrupt count=%u, clamping\n",
               (unsigned)s_rec.count);
        s_rec.count = RFID_AUTHLIST_MAX;
    }
    printk("rfid_authlist: loaded from slot %c (counter=%u, count=%u)\n",
           'A' + slot, (unsigned)counter, (unsigned)s_rec.count);
    return 0;
}

uint8_t rfid_authlist_count(void)
{
    return s_rec.count;
}

int rfid_authlist_get_nth(uint8_t idx, uint32_t *out_uid)
{
    if (idx >= s_rec.count || out_uid == NULL) return -1;
    *out_uid = s_rec.uids[idx];
    return 0;
}

int rfid_authlist_contains(uint32_t uid)
{
    if (uid == 0u) return 0;
    for (uint8_t i = 0; i < s_rec.count; ++i) {
        if (s_rec.uids[i] == uid) return 1;
    }
    return 0;
}

int rfid_authlist_add(uint32_t uid)
{
    if (uid == 0u) return -3;
    if (rfid_authlist_contains(uid)) return 0;
    if (s_rec.count >= RFID_AUTHLIST_MAX) return -1;
    s_rec.uids[s_rec.count++] = uid;
    if (store() < 0) {
        --s_rec.count;
        return -2;
    }
    return 1;
}

int rfid_authlist_remove(uint32_t uid)
{
    if (uid == 0u) return 0;
    int found = -1;
    for (uint8_t i = 0; i < s_rec.count; ++i) {
        if (s_rec.uids[i] == uid) { found = (int)i; break; }
    }
    if (found < 0) return 0;
    for (uint8_t i = (uint8_t)found + 1; i < s_rec.count; ++i) {
        s_rec.uids[i - 1] = s_rec.uids[i];
    }
    --s_rec.count;
    int rc = store();
    if (rc < 0) return rc;
    return 1;
}

int rfid_authlist_clear(void)
{
    if (s_rec.count == 0) return 0;
    s_rec.count = 0;
    return store();
}
