/* range_alloc.c -- first-fit ranges inside one fixed-size block (see range_alloc.h). */

#include "range_alloc.h"

#include <stdlib.h>
#include <string.h>

static int ra_reserve(RangeAlloc *a, uint32_t n)
{
    RaRange *p;
    uint32_t cap;
    if (n <= a->cap)
        return 1;
    cap = a->cap ? a->cap * 2 : 16;
    while (cap < n)
        cap *= 2;
    p = (RaRange *)realloc(a->free, cap * sizeof *p);
    if (!p)
        return 0;
    a->free = p;
    a->cap = cap;
    return 1;
}

int ra_init(RangeAlloc *a, uint64_t size)
{
    memset(a, 0, sizeof *a);
    a->size = size;
    if (!ra_reserve(a, 16))
        return 0;
    a->free[0].off = 0;
    a->free[0].size = size;
    a->nfree = 1;
    return 1;
}

void ra_destroy(RangeAlloc *a)
{
    free(a->free);
    memset(a, 0, sizeof *a);
}

int ra_alloc(RangeAlloc *a, uint64_t size, uint64_t align, uint64_t *off)
{
    uint32_t i;
    if (!size || !align || (align & (align - 1)))
        return 0;
    for (i = 0; i < a->nfree; i++) {
        RaRange *r = &a->free[i];
        uint64_t start = (r->off + align - 1) & ~(align - 1);
        uint64_t end = r->off + r->size;
        if (start < r->off || start > end || end - start < size)
            continue;
        if (start == r->off && start + size == end) {        /* the whole range */
            memmove(r, r + 1, (a->nfree - i - 1) * sizeof *r);
            a->nfree--;
        } else if (start == r->off) {                        /* from its front */
            r->off += size;
            r->size -= size;
        } else if (start + size == end) {                    /* from its back: the padding stays */
            r->size = start - r->off;
        } else {                                             /* from the middle: split in two */
            if (!ra_reserve(a, a->nfree + 1))
                return 0;
            r = &a->free[i];
            memmove(r + 2, r + 1, (a->nfree - i - 1) * sizeof *r);
            r[1].off = start + size;
            r[1].size = end - (start + size);
            r->size = start - r->off;
            a->nfree++;
        }
        a->used += size;
        *off = start;
        return 1;
    }
    return 0;
}

int ra_free(RangeAlloc *a, uint64_t off, uint64_t size)
{
    uint32_t i = 0;
    int left, right;
    while (i < a->nfree && a->free[i].off < off)
        i++;
    a->used -= size;
    left = i > 0 && a->free[i - 1].off + a->free[i - 1].size == off;
    right = i < a->nfree && off + size == a->free[i].off;
    if (left && right) {
        a->free[i - 1].size += size + a->free[i].size;
        memmove(&a->free[i], &a->free[i + 1], (a->nfree - i - 1) * sizeof a->free[0]);
        a->nfree--;
    } else if (left) {
        a->free[i - 1].size += size;
    } else if (right) {
        a->free[i].off = off;
        a->free[i].size += size;
    } else {
        if (!ra_reserve(a, a->nfree + 1))
            return 0;
        memmove(&a->free[i + 1], &a->free[i], (a->nfree - i) * sizeof a->free[0]);
        a->free[i].off = off;
        a->free[i].size = size;
        a->nfree++;
    }
    return 1;
}
