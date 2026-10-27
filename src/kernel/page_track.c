/*
 * page_track.c -- which guest pages of the contiguous window changed (A8, M4 option A stage 2).
 *
 * GPU resources built from guest memory -- textures, vertex data -- only need
 * re-reading when the guest wrote the pages they came from. Instead of hashing
 * or copying their bytes every frame, a consumer asks xbox_PageTrackCheck for a
 * range: the pages are armed read-only (both CPU aliases of the contiguous
 * window: 0x80000000 + P and the tiled 0xF0000000 + P, which are views of one
 * section), and the first guest write to an armed page faults; the host's
 * exception handler hands it to xbox_PageTrackFault, which bumps that page's
 * write generation, unprotects the page and lets the write go on. A check
 * compares the sum of the range's generations with the stamp the consumer kept.
 *
 * Order, so no write is lost: the range is armed *before* the stamp is taken
 * and before the consumer reads the bytes. A write in between faults, bumps the
 * generation and makes the next check fail -- at worst one redundant re-read.
 *
 * Writes the exception handler cannot see -- the OS writing into guest memory
 * (ReadFile in NtReadFile) gets an error instead of a fault on a read-only page
 * -- go through xbox_PageTrackHostWrite first.
 *
 * Pages written in many consecutive frames (dynamic vertex buffers, movie
 * textures) stop being armed ("streaming"): a fault per page per frame would
 * cost more than the copy it saves. Their checks always say "changed", and they
 * are given another chance every few seconds.
 *
 * Enabled by the host once its exception handler forwards faults here
 * (xbox_PageTrackEnable); RECOMP_PAGE_TRACK=0 keeps it off. Only pages inside
 * the contiguous window are tracked; anything else always reads as changed.
 */
#include "xbox_memory_layout.h"
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PT_PAGE        0x1000u
#define PT_PAGES       (XBOX_CONTIG_SIZE / PT_PAGE)
#define PT_STREAM_RUN  3        /* written in this many consecutive frames -> streaming */
#define PT_RETRY       600      /* frames until streaming pages are tried again */

static int      s_on = -1;
static uint32_t s_gen[PT_PAGES];          /* write generation */
static uint8_t  s_armed[PT_PAGES];
static uint8_t  s_stream[PT_PAGES];
static uint8_t  s_run[PT_PAGES];          /* consecutive frames with a write */
static uint32_t s_wframe[PT_PAGES];       /* frame of the last write + 1 */
static uint32_t s_frame;
static CRITICAL_SECTION s_lock;
static uint32_t s_st_faults, s_st_arms, s_st_hostw, s_st_checks, s_st_same;
static int      s_stats = -1;

int xbox_PageTrackOn(void)
{
    return s_on > 0;
}

void xbox_PageTrackEnable(void)
{
    const char *e = getenv("RECOMP_PAGE_TRACK");
    if (s_on >= 0)
        return;
    s_on = !(e && *e == '0');
    if (s_on) {
        InitializeCriticalSection(&s_lock);
        fprintf(stderr, "  Page tracking: contiguous window, %u pages (RECOMP_PAGE_TRACK=0 turns it off)\n",
                (unsigned)PT_PAGES);
    }
    e = getenv("RECOMP_PAGE_TRACK_STATS");
    s_stats = e && *e == '1';
}

/* Host addresses of page index i in the two aliases (0 if that alias is not mapped). */
static uintptr_t pt_contig(uint32_t i)
{
    return (uintptr_t)xbox_GetMemoryOffset() + XBOX_CONTIG_BASE + (uintptr_t)i * PT_PAGE;
}

static uintptr_t pt_tiled(uint32_t i)
{
    if ((uint64_t)i * PT_PAGE >= xbox_TiledViewBytes())
        return 0;
    return (uintptr_t)xbox_GetMemoryOffset() + XBOX_TILED_BASE + (uintptr_t)i * PT_PAGE;
}

static void pt_protect(uint32_t first, uint32_t n, DWORD prot)
{
    DWORD old;
    uintptr_t t;
    VirtualProtect((void *)pt_contig(first), (SIZE_T)n * PT_PAGE, prot, &old);
    t = pt_tiled(first);
    if (t && pt_tiled(first + n - 1))
        VirtualProtect((void *)t, (SIZE_T)n * PT_PAGE, prot, &old);
}

/* Guest VA -> page range in the contiguous window; 0 if (partly) outside. */
static int pt_range(uint32_t va, uint32_t len, uint32_t *first, uint32_t *last)
{
    uint32_t p;
    if (!len)
        return 0;
    if (va >= XBOX_CONTIG_BASE && va - XBOX_CONTIG_BASE < XBOX_CONTIG_SIZE)
        p = va - XBOX_CONTIG_BASE;
    else if (va >= XBOX_TILED_BASE && va - XBOX_TILED_BASE < XBOX_CONTIG_SIZE)
        p = va - XBOX_TILED_BASE;
    else
        return 0;
    if ((uint64_t)p + len > XBOX_CONTIG_SIZE)
        return 0;
    *first = p / PT_PAGE;
    *last = (p + len - 1) / PT_PAGE;
    return 1;
}

static void pt_written(uint32_t i)
{
    s_gen[i]++;
    if (s_wframe[i] != s_frame + 1) {
        s_run[i] = s_wframe[i] == s_frame ? (uint8_t)(s_run[i] + 1) : 1;  /* last write was the frame before */
        s_wframe[i] = s_frame + 1;
        if (s_run[i] >= PT_STREAM_RUN)
            s_stream[i] = 1;
    }
}

int xbox_PageTrackCheck(uint32_t va, uint32_t len, uint64_t *stamp)
{
    uint32_t first, last, i, run0 = 0, nrun = 0;
    uint64_t sum = 0;
    int stream = 0;

    if (s_on <= 0 || !pt_range(va, len, &first, &last))
        return 0;
    EnterCriticalSection(&s_lock);
    s_st_checks++;
    for (i = first; i <= last; i++) {
        if (!s_stream[i] && !s_armed[i]) {
            if (!nrun)
                run0 = i;
            nrun++;
            s_armed[i] = 1;
            continue;
        }
        if (s_stream[i])
            stream = 1;
        /* A run of pages to arm ends at any page that is not one of them
         * (armed already, or streaming -- which must stay writable). */
        if (nrun) {
            pt_protect(run0, nrun, PAGE_READONLY);
            s_st_arms += nrun;
            nrun = 0;
        }
    }
    if (nrun) {
        pt_protect(run0, nrun, PAGE_READONLY);
        s_st_arms += nrun;
    }
    for (i = first; i <= last; i++)
        sum += s_gen[i] + ((uint64_t)i << 40);
    LeaveCriticalSection(&s_lock);
    if (!stream && *stamp == sum) {
        s_st_same++;
        return 1;
    }
    *stamp = stream ? 0 : sum;
    return 0;
}

uint32_t xbox_PageTrackLastWrite(uint32_t va, uint32_t len)
{
    uint32_t first, last, i, w = 0;
    if (s_on <= 0 || !pt_range(va, len, &first, &last))
        return 0xFFFFFFFFu;
    for (i = first; i <= last; i++) {
        if (s_stream[i])
            return 0xFFFFFFFFu;
        if (s_wframe[i] > w)
            w = s_wframe[i];
    }
    return w;
}

/* Lock-free: 1 if the page holding va is armed (any write since will fault)
 * and not streaming, with its write generation; 0 = re-check it with
 * xbox_PageTrackCheck. A write racing this read is the title's own race. */
int xbox_PageTrackPeek(uint32_t va, uint32_t *gen)
{
    uint32_t first, last;
    if (s_on <= 0 || !pt_range(va, 1, &first, &last))
        return 0;
    if (s_stream[first] || !s_armed[first])
        return 0;
    *gen = s_gen[first];
    return 1;
}

uint32_t xbox_PageTrackFrameNo(void)
{
    return s_frame + 1;
}

static int pt_host_page(uintptr_t host, uint32_t *idx)
{
    uintptr_t off = (uintptr_t)xbox_GetMemoryOffset();
    uintptr_t c = off + XBOX_CONTIG_BASE, t = off + XBOX_TILED_BASE;
    if (host >= c && host < c + XBOX_CONTIG_SIZE) {
        *idx = (uint32_t)((host - c) / PT_PAGE);
        return 1;
    }
    if (host >= t && host < t + XBOX_CONTIG_SIZE && (uint64_t)(host - t) < xbox_TiledViewBytes()) {
        *idx = (uint32_t)((host - t) / PT_PAGE);
        return 1;
    }
    return 0;
}

int xbox_PageTrackFault(uintptr_t host_addr)
{
    uint32_t i;
    if (s_on <= 0 || !pt_host_page(host_addr, &i))
        return 0;
    EnterCriticalSection(&s_lock);
    if (!s_armed[i]) {
        LeaveCriticalSection(&s_lock);
        return 0;
    }
    s_armed[i] = 0;
    pt_written(i);
    pt_protect(i, 1, PAGE_READWRITE);
    s_st_faults++;
    LeaveCriticalSection(&s_lock);
    return 1;
}

void xbox_PageTrackHostWrite(const void *host, size_t len)
{
    uint32_t a, b, i;
    if (s_on <= 0 || !len)
        return;
    if (!pt_host_page((uintptr_t)host, &a) || !pt_host_page((uintptr_t)host + len - 1, &b) || b < a)
        return;
    EnterCriticalSection(&s_lock);
    for (i = a; i <= b; i++) {
        if (s_armed[i]) {
            s_armed[i] = 0;
            pt_protect(i, 1, PAGE_READWRITE);
        }
        pt_written(i);
    }
    s_st_hostw++;
    LeaveCriticalSection(&s_lock);
}

void xbox_PageTrackFrame(void)
{
    uint32_t i;
    if (s_on <= 0)
        return;
    EnterCriticalSection(&s_lock);
    s_frame++;
    if (s_frame % PT_RETRY == 0) {
        uint32_t n = 0;
        for (i = 0; i < PT_PAGES; i++)
            if (s_stream[i]) {
                s_stream[i] = 0;
                s_run[i] = 0;
                n++;
            }
        (void)n;
    }
    if (s_stats > 0 && s_frame % 300 == 0) {
        uint32_t armed = 0, stream = 0;
        for (i = 0; i < PT_PAGES; i++) {
            armed += s_armed[i];
            stream += s_stream[i];
        }
        fprintf(stderr, "[PAGES] frame %u: %u checks (%u unchanged), %u faults, %u pages armed, %u host writes;"
                " now %u armed, %u streaming\n", s_frame, s_st_checks, s_st_same, s_st_faults, s_st_arms,
                s_st_hostw, armed, stream);
        s_st_checks = s_st_same = s_st_faults = s_st_arms = s_st_hostw = 0;
    }
    LeaveCriticalSection(&s_lock);
}
