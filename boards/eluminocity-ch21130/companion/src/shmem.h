/* Accessor layer over the Delta SysV shared-memory segment.
 *
 * Two ways to obtain a `struct shmem`:
 *   - shmem_attach()    — on-device, RO: shmget()+shmat(SHM_RDONLY).
 *   - shmem_attach_rw() — on-device, RW: shmget()+shmat(0). Used when the
 *     bridge has v0.3 write controls enabled.
 *   - shmem_load_file() — host tests: read a fixture file into a malloc()'d
 *     buffer (always writable in tests).
 * Read accessors are identical regardless of attach mode. Write helpers
 * succeed only when the segment is RW (or owned/malloc'd).
 */
#ifndef SHMEM_H
#define SHMEM_H

#include <stddef.h>
#include <stdint.h>

struct shmem {
    /* `base` is the segment / buffer base. For RW attaches and host-test
     * buffers this storage is writable; cast away const inside shmem.c
     * when performing bounds-checked writes. The const here is a hint to
     * callers that read-helpers don't mutate, not a guarantee about the
     * underlying mapping. */
    const unsigned char *base;
    size_t               size;   /* bytes available at base */
    int                  shmid;  /* >=0 if attached via shmem_attach*, else -1 */
    unsigned char       *owned;  /* non-NULL if shmem_load_file malloc'd it */
    int                  writable; /* 1 if RW attach or owned buffer; 0 if RO */
};

/* On-device: attach the live segment read-only. Returns 0 on success,
 * -1 if the segment does not exist yet (caller should retry/backoff). */
int  shmem_attach(struct shmem *sm);

/* On-device: attach the live segment read-write. Same fail modes as
 * shmem_attach. Differs only in dropping SHM_RDONLY from the shmat flags. */
int  shmem_attach_rw(struct shmem *sm);

/* Host tests: load a fixture file. Returns 0 on success, -1 on error.
 * The resulting buffer is writable (test code may use shmem_write_* helpers). */
int  shmem_load_file(struct shmem *sm, const char *path);

/* Detach / free. Safe to call on a zeroed struct. */
void shmem_release(struct shmem *sm);

/* Read a single byte. Out-of-range offsets return 0 (defensive, never crash). */
unsigned char shmem_u8(const struct shmem *sm, size_t off);

/* Read a 16-bit little-endian word, byte-by-byte (ARMv5 may trap unaligned
 * 2-byte loads). Out-of-range bytes are read as 0. */
unsigned short shmem_u16_le(const struct shmem *sm, size_t off);

/* Read a 32-bit little-endian word, byte-by-byte. Out-of-range bytes are 0. */
unsigned int shmem_u32_le(const struct shmem *sm, size_t off);

/* Copy `len` bytes from `off` into `dst`. Out-of-range bytes are zero-filled. */
void shmem_copy(const struct shmem *sm, size_t off, void *dst, size_t len);

/* Bounds-checked little-endian writes. Out-of-range writes are silently
 * dropped (consistent with the read helpers' "OOB returns 0" pattern).
 * Writes to a non-writable mapping (RO attach) are also dropped.
 * Returns 0 on success, -1 if the offset is OOB or the mapping is RO. */
int shmem_write_u8    (struct shmem *sm, unsigned off, uint8_t  v);
int shmem_write_u16_le(struct shmem *sm, unsigned off, uint16_t v);
int shmem_write_u32_le(struct shmem *sm, unsigned off, uint32_t v);

#endif /* SHMEM_H */
