#include "shmem.h"
#include "shmem_offsets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>

static int attach_with_flags(struct shmem *sm, int shmflg, int writable)
{
    memset(sm, 0, sizeof(*sm));
    sm->shmid = -1;
    /* No IPC_CREAT: if the segment is absent we must NOT create it. */
    int id = shmget(SHMEM_KEY, SHMEM_SIZE, 0);
    if (id < 0)
        return -1;
    void *p = shmat(id, NULL, shmflg);
    if (p == (void *)-1)
        return -1;
    sm->base     = (const unsigned char *)p;
    sm->size     = SHMEM_SIZE;
    sm->shmid    = id;
    sm->writable = writable;
    return 0;
}

int shmem_attach(struct shmem *sm)
{
    return attach_with_flags(sm, SHM_RDONLY, 0);
}

int shmem_attach_rw(struct shmem *sm)
{
    return attach_with_flags(sm, 0, 1);
}

int shmem_load_file(struct shmem *sm, const char *path)
{
    memset(sm, 0, sizeof(*sm));
    sm->shmid = -1;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    unsigned char *buf = malloc(SHMEM_SIZE);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t n = fread(buf, 1, SHMEM_SIZE, f);
    fclose(f);
    if (n != SHMEM_SIZE) {
        free(buf);
        return -1;
    }
    sm->base     = buf;
    sm->owned    = buf;
    sm->size     = SHMEM_SIZE;
    sm->writable = 1;             /* host fixture buffers are always writable */
    return 0;
}

void shmem_release(struct shmem *sm)
{
    if (sm->base && sm->shmid >= 0)
        shmdt(sm->base);
    if (sm->owned)
        free(sm->owned);
    memset(sm, 0, sizeof(*sm));
    sm->shmid = -1;
}

unsigned char shmem_u8(const struct shmem *sm, size_t off)
{
    if (!sm->base || off >= sm->size)
        return 0;
    return sm->base[off];
}

unsigned short shmem_u16_le(const struct shmem *sm, size_t off)
{
    /* Byte-wise: ARMv5 traps unaligned u16 loads at certain CP15 configs,
     * and the producer marshalls byte-by-byte anyway. */
    unsigned b0 = shmem_u8(sm, off);
    unsigned b1 = shmem_u8(sm, off + 1);
    return (unsigned short)(b0 | (b1 << 8));
}

unsigned int shmem_u32_le(const struct shmem *sm, size_t off)
{
    unsigned b0 = shmem_u8(sm, off);
    unsigned b1 = shmem_u8(sm, off + 1);
    unsigned b2 = shmem_u8(sm, off + 2);
    unsigned b3 = shmem_u8(sm, off + 3);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

void shmem_copy(const struct shmem *sm, size_t off, void *dst, size_t len)
{
    unsigned char *d = dst;
    if (!sm->base || off >= sm->size) {
        memset(dst, 0, len);
        return;
    }
    for (size_t i = 0; i < len; i++)
        d[i] = shmem_u8(sm, off + i);
}

/* Cast through this single helper so reviewers can grep for every write-back
 * into the segment. `base` is typed `const` purely as a guard against
 * accidental writes from the read accessors; the underlying mapping is
 * writable when the bridge attached RW (or for malloc'd host fixtures). */
static unsigned char *writable_ptr(struct shmem *sm, unsigned off, size_t width)
{
    if (!sm->base || !sm->writable)
        return NULL;
    if ((size_t)off + width > sm->size)
        return NULL;
    return (unsigned char *)sm->base + off;
}

int shmem_write_u8(struct shmem *sm, unsigned off, uint8_t v)
{
    unsigned char *p = writable_ptr(sm, off, 1);
    if (!p)
        return -1;
    p[0] = v;
    return 0;
}

int shmem_write_u16_le(struct shmem *sm, unsigned off, uint16_t v)
{
    unsigned char *p = writable_ptr(sm, off, 2);
    if (!p)
        return -1;
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    return 0;
}

int shmem_write_u32_le(struct shmem *sm, unsigned off, uint32_t v)
{
    unsigned char *p = writable_ptr(sm, off, 4);
    if (!p)
        return -1;
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
    return 0;
}
