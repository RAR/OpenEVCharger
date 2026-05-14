#include "shmem.h"
#include "shmem_offsets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>

int shmem_attach(struct shmem *sm)
{
    memset(sm, 0, sizeof(*sm));
    sm->shmid = -1;
    /* No IPC_CREAT: if the segment is absent we must NOT create it. */
    int id = shmget(SHMEM_KEY, SHMEM_SIZE, 0);
    if (id < 0)
        return -1;
    void *p = shmat(id, NULL, SHM_RDONLY);
    if (p == (void *)-1)
        return -1;
    sm->base  = (const unsigned char *)p;
    sm->size  = SHMEM_SIZE;
    sm->shmid = id;
    return 0;
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
    sm->base  = buf;
    sm->owned = buf;
    sm->size  = SHMEM_SIZE;
    return 0;
}

void shmem_release(struct shmem *sm)
{
    if (sm->shmid >= 0 && sm->base)
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

void shmem_copy(const struct shmem *sm, size_t off, void *dst, size_t len)
{
    unsigned char *d = dst;
    for (size_t i = 0; i < len; i++)
        d[i] = shmem_u8(sm, off + i);
}
