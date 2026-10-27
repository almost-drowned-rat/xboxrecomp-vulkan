/* range_alloc unit test: fixed cases, then random alloc/free checked against a
 * byte map of the block (no overlap, alignment kept, free list sorted and never
 * adjacent, everything merges back into one range at the end). */
#include "range_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)

static void check_list(const RangeAlloc *a)
{
    uint32_t i;
    uint64_t fr = 0;
    for (i = 0; i < a->nfree; i++) {
        CHECK(a->free[i].size > 0);
        CHECK(a->free[i].off + a->free[i].size <= a->size);
        if (i)
            CHECK(a->free[i - 1].off + a->free[i - 1].size < a->free[i].off);   /* sorted, not adjacent */
        fr += a->free[i].size;
    }
    CHECK(fr + a->used == a->size);
}

static void fixed_cases(void)
{
    RangeAlloc a;
    uint64_t o1, o2, o3, o4;
    CHECK(ra_init(&a, 1024));
    CHECK(ra_alloc(&a, 100, 1, &o1) && o1 == 0);
    CHECK(ra_alloc(&a, 100, 256, &o2) && o2 == 256);         /* padding 100..255 stays free */
    CHECK(a.nfree == 2);
    CHECK(ra_alloc(&a, 156, 4, &o3) && o3 == 100);           /* fills the padding exactly */
    CHECK(a.nfree == 1);
    CHECK(!ra_alloc(&a, 2000, 1, &o4));                      /* too big */
    CHECK(!ra_alloc(&a, 10, 3, &o4));                        /* align not a power of two */
    CHECK(ra_alloc(&a, 668, 1, &o4) && o4 == 356);           /* the rest */
    CHECK(a.nfree == 0 && a.used == 1024);
    CHECK(!ra_alloc(&a, 1, 1, &o4));
    CHECK(ra_free(&a, o2, 100));                             /* hole in the middle */
    CHECK(ra_free(&a, o1, 100));
    CHECK(ra_free(&a, o4, 668));                             /* merges with o2's range */
    CHECK(a.nfree == 2);
    check_list(&a);
    CHECK(ra_free(&a, o3, 156));                             /* joins both neighbours */
    CHECK(a.nfree == 1 && a.free[0].off == 0 && a.free[0].size == 1024 && a.used == 0);
    ra_destroy(&a);
}

#define BLOCK (1u << 20)
#define LIVE 400

static void random_cases(void)
{
    static unsigned char map[BLOCK];
    struct { uint64_t off, size; } live[LIVE];
    RangeAlloc a;
    unsigned seed = 12345, step, n = 0, ok = 0, full = 0;
    CHECK(ra_init(&a, BLOCK));
    memset(map, 0, sizeof map);
    for (step = 0; step < 200000 && !s_fail; step++) {
        seed = seed * 1103515245u + 12345u;
        if (n < LIVE && ((seed >> 16) % 3 != 0 || n == 0)) {
            uint64_t size = 1 + (seed >> 8) % 8192, align = 1ull << ((seed >> 4) % 13), off, k;
            if (ra_alloc(&a, size, align, &off)) {
                CHECK(off % align == 0 && off + size <= BLOCK);
                for (k = 0; k < size; k++) {
                    CHECK(!map[off + k]);
                    map[off + k] = 1;
                }
                live[n].off = off;
                live[n].size = size;
                n++;
                ok++;
            } else {
                full++;
            }
        } else {
            unsigned i = (seed >> 12) % n;
            uint64_t k;
            for (k = 0; k < live[i].size; k++)
                map[live[i].off + k] = 0;
            CHECK(ra_free(&a, live[i].off, live[i].size));
            live[i] = live[--n];
        }
        if (step % 997 == 0)
            check_list(&a);
    }
    while (n) {
        n--;
        CHECK(ra_free(&a, live[n].off, live[n].size));
    }
    CHECK(a.nfree == 1 && a.free[0].size == BLOCK && a.used == 0);
    printf("random: %u allocations, %u refused (block full)\n", ok, full);
    ra_destroy(&a);
}

int main(void)
{
    fixed_cases();
    random_cases();
    printf(s_fail ? "range_alloc: %d check(s) FAILED\n" : "range_alloc: all checks passed\n", s_fail);
    return s_fail != 0;
}
