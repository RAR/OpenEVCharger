/* Read-only accessor layer over the Delta SysV shared-memory segment.
 *
 * Two ways to obtain a `struct shmem`:
 *   - shmem_attach()    — on-device: shmget()+shmat() of the live segment.
 *   - shmem_load_file() — host tests: read a fixture file into a buffer.
 * Either way, the accessors below are identical and never write the segment.
 */
#ifndef SHMEM_H
#define SHMEM_H

#include <stddef.h>

struct shmem {
    const unsigned char *base;   /* segment / buffer base */
    size_t               size;   /* bytes available at base */
    int                  shmid;  /* >=0 if attached via shmem_attach, else -1 */
    unsigned char       *owned;  /* non-NULL if shmem_load_file malloc'd it */
};

/* On-device: attach the live segment read-only. Returns 0 on success,
 * -1 if the segment does not exist yet (caller should retry/backoff). */
int  shmem_attach(struct shmem *sm);

/* Host tests: load a fixture file. Returns 0 on success, -1 on error. */
int  shmem_load_file(struct shmem *sm, const char *path);

/* Detach / free. Safe to call on a zeroed struct. */
void shmem_release(struct shmem *sm);

/* Read a single byte. Out-of-range offsets return 0 (defensive, never crash). */
unsigned char shmem_u8(const struct shmem *sm, size_t off);

/* Copy `len` bytes from `off` into `dst`. Out-of-range bytes are zero-filled. */
void shmem_copy(const struct shmem *sm, size_t off, void *dst, size_t len);

#endif /* SHMEM_H */
