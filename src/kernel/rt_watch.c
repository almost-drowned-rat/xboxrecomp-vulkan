/*
 * rt_watch.c -- debug: catch CPU access to render targets (RECOMP_RT_WATCH=1).
 *
 * The Vulkan renderer keeps surfaces on the GPU; guest memory under a render
 * target holds what the guest last wrote there, not the rendered image. A
 * title that locks a render target and reads it on the CPU (screenshots, save
 * thumbnails, occlusion tricks) would read stale memory. To find out whether
 * it does: after every flip the renderer re-arms the guest pages of each
 * surface it drew into (xbox_RtWatchArm), in both CPU aliases of the
 * contiguous window (0x80000000 + P and the tiled 0xF0000000 + P); the
 * first CPU touch of an armed page faults, is logged (rate-limited) by the
 * host's exception handler through xbox_RtWatchFault, unprotected, and
 * resumed. Off by default; costs a VirtualProtect per armed range per frame.
 */
#include "xbox_memory_layout.h"
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RT_PAGE   0x1000u
#define RT_RANGES 64

static int s_on = -1;
static struct { uintptr_t lo, hi; uint32_t surf; } s_armed[RT_RANGES * 3];
static int s_narmed;
static unsigned s_logged, s_hits;
static CRITICAL_SECTION s_lock;

int xbox_RtWatchOn(void)
{
    if (s_on < 0) {
        const char *e = getenv("RECOMP_RT_WATCH");
        s_on = e && *e && *e != '0';
        if (s_on) {
            InitializeCriticalSection(&s_lock);
            fprintf(stderr, "  RT watch: CPU access to render targets is logged (debug)\n");
        }
    }
    return s_on;
}

static void arm_one(uintptr_t host, uint32_t bytes, uint32_t surf)
{
    DWORD old;
    uintptr_t lo = host & ~(uintptr_t)(RT_PAGE - 1);
    uintptr_t hi = (host + bytes + RT_PAGE - 1) & ~(uintptr_t)(RT_PAGE - 1);
    int i;
    for (i = 0; i < s_narmed; i++)
        if (s_armed[i].lo == lo && s_armed[i].hi == hi)
            break;
    if (i == s_narmed) {
        if (s_narmed >= (int)(sizeof s_armed / sizeof s_armed[0]))
            return;
        s_narmed++;
    }
    {
        static int told;
        if (told < 6) {
            told++;
            fprintf(stderr, "  RT watch: arm surface 0x%08X host %p..%p\n", surf, (void *)lo, (void *)hi);
        }
    }
    if (VirtualProtect((void *)lo, hi - lo, PAGE_NOACCESS, &old)) {
        s_armed[i].lo = lo;
        s_armed[i].hi = hi;
        s_armed[i].surf = surf;
    } else {
        static int failed;
        if (failed++ < 4)
            fprintf(stderr, "  RT watch: cannot protect %p (error %lu)\n", (void *)lo,
                    (unsigned long)GetLastError());
        if (i == s_narmed - 1)
            s_narmed--;        /* not mapped in this alias */
    }
}

/* Size of the contiguous block that starts at addr, 0 if none does. */
extern uint32_t xbox_ContiguousBlockSize(uint32_t addr);

void xbox_RtWatchArm(uint32_t gpu_va, uint32_t bytes)
{
    uintptr_t off = (uintptr_t)xbox_GetMemoryOffset();
    uint32_t p = gpu_va & 0x0FFFFFFFu;
    uint32_t blk;
    if (!xbox_RtWatchOn() || !bytes)
        return;
    /* Never past the allocation the surface lives in. The renderer's extent
     * is width x height x 4 with the tallest clip the address ever had, and
     * with reclaim the same address comes back as a smaller block: THPS 2X's
     * 128 KB target at 0x810A0000 (career, back to the front end) was armed
     * into the next block, and every texture loaded there logged as a CPU
     * write of a render target. */
    blk = xbox_ContiguousBlockSize(XBOX_CONTIG_BASE + p);
    if (blk && bytes > blk)
        bytes = blk;
    EnterCriticalSection(&s_lock);
    if (p + bytes <= XBOX_CONTIG_SIZE) {
        arm_one(off + XBOX_CONTIG_BASE + p, bytes, gpu_va);
        arm_one(off + XBOX_TILED_BASE + p, bytes, gpu_va);
    }
    /* Not the RAM view at off + P: the contiguous window has storage of its
     * own (xbox_memory_layout.c), and low RAM there is the title's code and
     * data -- arming it would trap everything. */
    LeaveCriticalSection(&s_lock);
}

/* A contiguous block goes back (MmFreeContiguousMemory): whatever was a
 * render target there is not one any more. With reclaim the block is handed
 * out again, and the next owner's first write (a texture loaded into it)
 * would log as a CPU write of the old surface. Every armed range touching the
 * block is unprotected and dropped; the renderer re-arms a surface it draws
 * into again. Page tracking hears about it as a host write, since the pages
 * just lost the read-only state it may have given them. */
void xbox_RtWatchRelease(uint32_t contig_va, uint32_t size)
{
    uintptr_t off = (uintptr_t)xbox_GetMemoryOffset();
    uint32_t p = contig_va & 0x0FFFFFFFu;
    uintptr_t lo[2], hi[2];
    int i, k, n = 0;
    if (s_on <= 0 || !size || p + size > XBOX_CONTIG_SIZE)
        return;
    lo[0] = off + XBOX_CONTIG_BASE + p;
    lo[1] = off + XBOX_TILED_BASE + p;
    hi[0] = lo[0] + size;
    hi[1] = lo[1] + size;
    EnterCriticalSection(&s_lock);
    for (i = 0; i < s_narmed; ) {
        int overlap = 0;
        for (k = 0; k < 2; k++)
            if (s_armed[i].lo < hi[k] && lo[k] < s_armed[i].hi)
                overlap = 1;
        if (overlap) {
            DWORD old;
            VirtualProtect((void *)s_armed[i].lo, s_armed[i].hi - s_armed[i].lo, PAGE_READWRITE, &old);
            xbox_PageTrackHostWrite((const void *)s_armed[i].lo, s_armed[i].hi - s_armed[i].lo);
            s_armed[i] = s_armed[--s_narmed];
            n++;
            continue;
        }
        i++;
    }
    LeaveCriticalSection(&s_lock);
    if (n) {
        static unsigned told;
        if (told++ < 6)
            fprintf(stderr, "  RT watch: block 0x%08X (%u bytes) freed, %d armed range(s) dropped\n",
                    contig_va, size, n);
    }
}

int xbox_RtWatchFault(uintptr_t host_addr, int write, uintptr_t host_pc)
{
    uintptr_t off = (uintptr_t)xbox_GetMemoryOffset();
    int i, hit = -1;
    if (s_on <= 0)
        return 0;
    EnterCriticalSection(&s_lock);
    for (i = 0; i < s_narmed; i++)
        if (host_addr >= s_armed[i].lo && host_addr < s_armed[i].hi) {
            hit = i;
            break;
        }
    if (hit >= 0) {
        DWORD old;
        uintptr_t page = host_addr & ~(uintptr_t)(RT_PAGE - 1);
        /* Only this page goes back to read/write; the rest of the surface
         * stays armed until the next flip re-arms the whole range. */
        VirtualProtect((void *)page, RT_PAGE, PAGE_READWRITE, &old);
        s_hits++;
        if (s_logged < 64 || (s_hits % 1000) == 0) {
            s_logged++;
            fprintf(stderr, "[RTWATCH] CPU %s of render target 0x%08X at guest 0x%08X, thread %lu, host pc %p (hit %u)\n",
                    write ? "write" : "read", s_armed[hit].surf, (uint32_t)(host_addr - off),
                    (unsigned long)GetCurrentThreadId(), (void *)host_pc, s_hits);
            fflush(stderr);
        }
    }
    LeaveCriticalSection(&s_lock);
    return hit >= 0;
}
