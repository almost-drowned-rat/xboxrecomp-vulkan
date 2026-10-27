/* range_alloc.h -- first-fit ranges inside one fixed-size block.
 *
 * nv2a_vk sub-allocates texture memory from large device-local blocks with
 * it (one vkAllocateMemory per texture cost ~0.12 ms; a level start creates
 * ~150 textures). Pure bookkeeping, no Vulkan: the free ranges of a block,
 * sorted by offset, never adjacent (freeing merges neighbours). */
#ifndef RANGE_ALLOC_H
#define RANGE_ALLOC_H

#include <stdint.h>

typedef struct {
    uint64_t off, size;
} RaRange;

typedef struct {
    uint64_t size;              /* the block */
    uint64_t used;              /* bytes handed out (alignment padding stays free) */
    RaRange *free;
    uint32_t nfree, cap;
} RangeAlloc;

/* 0 when out of memory. */
int  ra_init(RangeAlloc *a, uint64_t size);
void ra_destroy(RangeAlloc *a);
/* `size` bytes at a multiple of `align` (a power of two): 1 and *off, or 0
 * when no free range fits. */
int  ra_alloc(RangeAlloc *a, uint64_t size, uint64_t align, uint64_t *off);
/* Gives back exactly what ra_alloc handed out. 0 when out of memory for the
 * range list (the range stays lost; the block is still consistent). */
int  ra_free(RangeAlloc *a, uint64_t off, uint64_t size);

#endif
