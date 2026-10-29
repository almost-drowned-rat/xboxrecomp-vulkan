/*
 * nv2a_vk.c -- a Vulkan 1.3 renderer for the pushbuffer executor.
 *
 * The same job as nv2a_gl.c, on Vulkan: registered as the executor's back end
 * (nv2a_backend.h, draw_raw), it turns the method shadow into draws. Built
 * instead of nv2a_gl for the Switch's NVK (mesa-switch, linked statically)
 * and, on Linux, any Vulkan driver (lavapipe runs it headless).
 *
 *   surfaces   one image per colour-surface address (B8G8R8A8, rows top-first
 *              like guest memory), depth per zeta address (as nv2a_gl). All
 *              images live in VK_IMAGE_LAYOUT_GENERAL: a surface is drawn
 *              into, sampled and blitted without layout changes, ordered by
 *              memory barriers between rendering passes.
 *   textures   the GL renderer's cache and decoders; uploaded through the
 *              frame's staging ring. DXT stays compressed (BC1/2/3).
 *   shaders    gl_vsh.c / gl_psh.c in their Vulkan dialect, compiled to
 *              SPIR-V at run time by glslang, cached by what generated them.
 *   pipelines  keyed by the shader pair, blend state, colour mask, whether
 *              there is a depth buffer and the topology class; everything
 *              else (depth, stencil, cull, winding, topology, vertex layout,
 *              viewport) is dynamic state. Without
 *              VK_EXT_vertex_input_dynamic_state the vertex formats join the
 *              key and only the strides stay dynamic (s_baked_vi).
 *   uniforms   two std140 blocks per draw in the frame ring, textures as
 *              combined image samplers, all through push descriptors.
 *
 * Two frames in flight. Everything runs on the executor's thread.
 *
 * Runtime switches (the GL renderer's names where they mean the same):
 *   RECOMP_GL_DUMP=<prefix>[,every]  write presented frames as BMP
 *   RECOMP_GL_TRACE=1                shader sources on compile errors
 *   RECOMP_GL_SCALE=<k>              render surfaces at k times the title's
 *                                    resolution, fractions allowed (1.5;
 *                                    0.5..5, default 1); overrides the
 *                                    port's option (nv2a_vk_set_render_scale)
 *   RECOMP_GL_SCALE_X=<k>            horizontal scale, if not the same
 *                                    (0.5..6.67 = 5 x 4/3)
 *   RECOMP_VK_SQUARE=0               widescreen without 4/3 more columns on
 *                                    surfaces without horizontal AA
 *   RECOMP_VK_MIPS=0                 sample level 0 only (no mip chains)
 *   RECOMP_VK_ANISO=<n>              anisotropic filtering of mipmapped
 *                                    textures (default 16; 1 = off)
 *   RECOMP_VK_LOD_BIAS=<b>           mip bias of mipmapped textures
 *                                    (default -0.25: a little sharper)
 *   RECOMP_VK_FXAA=0                 present with a plain blit, no FXAA
 *   RECOMP_DISPLAY_MODE=<m>          windowed | borderless | fullscreen;
 *                                    overrides the port's option
 *                                    (nv2a_vk_set_display_mode)
 *   RECOMP_SHADER_PRESET=<path>      a slang preset (.slangp) through
 *                                    librashader (shader_pass.c); "" = none;
 *                                    overrides nv2a_vk_set_shader_preset
 *   RECOMP_VK_FAIL_ALLOC_MB=<n>      test: images above n MB fail to
 *                                    allocate (the render-scale / MSAA
 *                                    fallbacks)
 *   RECOMP_VK_NO_DYN_VERTEX_INPUT=1  bake the vertex formats into the
 *                                    pipelines as on devices without
 *                                    VK_EXT_vertex_input_dynamic_state (tests)
 *   RECOMP_VK_VALIDATION=1           (Linux) enable the Khronos validation layer
 *   RECOMP_VK_HEADLESS=1             (Linux) no window, no present
 */
#include "nv2a_vk.h"
#include "../nv2a_gl/gl_psh.h"
#include "../nv2a_gl/gl_vsh.h"
#include "../kernel/nv2a_backend.h"
#include "range_alloc.h"

#if defined(__SWITCH__)
#include <switch.h>
#define VK_USE_PLATFORM_VI_NN 1
#endif
#include <vulkan/vulkan.h>
#if !defined(__SWITCH__)
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include "shader_pass.h"
#endif
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#if defined(__SWITCH__)
#include <pthread.h>
#endif
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define LOGE(...) fprintf(stderr, "  [VK] " __VA_ARGS__)

static int s_state;            /* 0 untried, 1 ready, -1 failed */
static int s_trace;
static int s_debug = -1;       /* RECOMP_DEBUG: renderer debug lines (dropped surfaces) */
/* RECOMP_VK_TRACE=<n>: name every step of the first n draws, clears and
 * flips (a crash inside the driver then ends the log at the step). */
static int s_vtrace;
#define VT(...) do { if (s_vtrace > 0) { fprintf(stderr, "  [VKT] " __VA_ARGS__); fflush(stderr); } } while (0)
static uint32_t s_frame;
/* RECOMP_TEX_CHANGES=1: log every cached texture whose guest bytes changed
 * (re-uploaded). Finds textures a title frees while still drawing them. */
static int s_tex_changes = -1;
static int s_drew_any;
static int fxaa_on(void);
static const uint32_t *s_regs;
static const Nv2aRawBatch *s_batch;

/* Hitch accounting, as in nv2a_gl. */
static uint32_t s_hz_progs, s_hz_texs, s_hz_draws;
static uint64_t s_hz_prog_ns, s_hz_tex_ns, s_hz_tex_bytes;
static uint64_t hz_now(void)
{
#if defined(__SWITCH__)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#else
    return SDL_GetTicksNS();
#endif
}

/* ── Device ───────────────────────────────────────────────────────── */

static VkInstance       s_inst;
static VkPhysicalDevice s_pd;
static VkDevice         s_dev;
static VkQueue          s_queue;
static uint32_t         s_qfam;
static VkPhysicalDeviceMemoryProperties s_memprops;
static VkDeviceSize     s_ubo_align = 256;
static uint32_t         s_ubo_range = 16384;
static VkFormat         s_depth_fmt = VK_FORMAT_D24_UNORM_S8_UINT;
static int              s_have_bc, s_have_depth_clamp;

static PFN_vkCmdPushDescriptorSetKHR  p_push_desc;
static PFN_vkCmdSetVertexInputEXT     p_vertex_input;
static PFN_vkCmdBindVertexBuffers2    p_bind_vb2;
/* 1: the vertex formats are baked into the pipelines (the device lacks
 * VK_EXT_vertex_input_dynamic_state, e.g. AMD Polaris on Windows, or
 * RECOMP_VK_NO_DYN_VERTEX_INPUT=1); only the strides stay dynamic (core 1.3). */
static int s_baked_vi;
static PFN_vkCmdBeginRendering        p_begin_rendering;
static PFN_vkCmdEndRendering          p_end_rendering;
static PFN_vkCmdSetCullMode           p_cull_mode;
static PFN_vkCmdSetFrontFace          p_front_face;
static PFN_vkCmdSetPrimitiveTopology  p_topology;
static PFN_vkCmdSetDepthTestEnable    p_depth_test;
static PFN_vkCmdSetDepthWriteEnable   p_depth_write;
static PFN_vkCmdSetDepthCompareOp     p_depth_op;
static PFN_vkCmdSetStencilTestEnable  p_stencil_test;
static PFN_vkCmdSetStencilOp          p_stencil_op;

/* Presentation. */
static VkSurfaceKHR   s_surface;
static VkSwapchainKHR s_swapchain;
static VkFormat       s_sc_format;
static VkExtent2D     s_sc_extent;
static uint32_t       s_sc_count;
static VkImage        s_sc_images[8];
static VkImageView    s_sc_views[8];
static int            s_headless;
/* Window events for the host (nv2a_vk.h). Read on the render thread, which
 * pumps the window's events in vk_flip. */
static void (*s_key_handler)(int scancode, int down);
static void (*s_quit_handler)(void);
/* The renderer cannot start (nv2a_vk_set_fatal_handler): the port's exit, the
 * message box title and a help line (e.g. the README's requirements link). */
static void (*s_fatal_handler)(void);
static const char *s_fatal_title;
static const char *s_fatal_help;
/* A screenshot asked for by name (vk_screenshot). */
static char s_shot_name[32];
static volatile int s_shot_pending;
#if !defined(__SWITCH__)
static SDL_Window    *s_win;
static SDL_ThreadID   s_win_thread;     /* the NV2A thread, which made s_win */
#endif
static volatile float s_display_hz;     /* nv2a_vk_display_hz; 0 = not known */

float nv2a_vk_display_hz(void)
{
    return s_display_hz;
}

#if !defined(__SWITCH__)
/* The window's display refresh, on the thread that owns the window (SDL video
 * calls belong there). The exact rational rate when SDL has it (59.94). */
static void display_hz_update(void)
{
    SDL_DisplayID d = s_win ? SDL_GetDisplayForWindow(s_win) : 0;
    const SDL_DisplayMode *m = d ? SDL_GetCurrentDisplayMode(d) : NULL;
    float hz = 0.0f;
    if (m)
        hz = m->refresh_rate_numerator > 0 && m->refresh_rate_denominator > 0
           ? (float)m->refresh_rate_numerator / (float)m->refresh_rate_denominator : m->refresh_rate;
    if (hz != s_display_hz) {
        s_display_hz = hz;
        LOGE("display refresh: %.2f Hz\n", hz);
    }
}
#endif

/* ── Display mode and the windowed rectangle (nv2a_vk.h) ─────────────
 * The port's choice (s_dm_opt) is applied on the render thread (dm_apply, at
 * window creation and every flip); RECOMP_DISPLAY_MODE wins. The windowed
 * rectangle is recorded on move / resize events while windowed and handed to
 * the port through a counter (nv2a_vk_window_rect) -- the port saves it on its
 * own thread, so its settings store needs no lock. */
static volatile int s_dm_opt = NV2A_VK_DISPLAY_WINDOWED;
static int s_dm_env = -2;                   /* -2 not read, -1 not set, else forced */
static int s_dm_cur = -1;                   /* as applied; -1 before the window */
static int s_wr_want[4];                    /* nv2a_vk_set_window_rect: x, y, w, h */
static int s_wr_have;
static volatile int s_wr[4];
static volatile unsigned s_wr_gen;

void nv2a_vk_set_display_mode(int mode)
{
    s_dm_opt = mode >= NV2A_VK_DISPLAY_WINDOWED && mode <= NV2A_VK_DISPLAY_FULLSCREEN ? mode
             : NV2A_VK_DISPLAY_WINDOWED;
}

void nv2a_vk_set_window_rect(int x, int y, int w, int h)
{
    s_wr_want[0] = x; s_wr_want[1] = y; s_wr_want[2] = w; s_wr_want[3] = h;
    s_wr_have = w > 0 && h > 0;
}

unsigned nv2a_vk_window_rect(int *x, int *y, int *w, int *h)
{
    unsigned g;
    do {                                    /* written on the render thread: a consistent set */
        g = s_wr_gen;
        *x = s_wr[0]; *y = s_wr[1]; *w = s_wr[2]; *h = s_wr[3];
    } while ((g & 1u) || g != s_wr_gen);
    return g;
}

#if !defined(__SWITCH__)
static int dm_mode(void)
{
    if (s_dm_env == -2) {
        const char *e = getenv("RECOMP_DISPLAY_MODE");
        s_dm_env = !e || !*e ? -1 : !strcmp(e, "borderless") ? NV2A_VK_DISPLAY_BORDERLESS
                 : !strcmp(e, "fullscreen") ? NV2A_VK_DISPLAY_FULLSCREEN : NV2A_VK_DISPLAY_WINDOWED;
    }
    return s_dm_env >= 0 ? s_dm_env : s_dm_opt;
}

/* the rectangle a port saved, if its centre is on a display */
static int wr_on_screen(const int *r)
{
    int n = 0, i, cx = r[0] + r[2] / 2, cy = r[1] + r[3] / 2, on = 0;
    SDL_DisplayID *ds = SDL_GetDisplays(&n);
    for (i = 0; ds && i < n && !on; i++) {
        SDL_Rect b;
        if (SDL_GetDisplayUsableBounds(ds[i], &b))
            on = cx >= b.x && cx < b.x + b.w && cy >= b.y && cy < b.y + b.h;
    }
    SDL_free(ds);
    return on;
}

/* the windowed rectangle after a move / resize (not maximised, not fullscreen) */
static void wr_record(void)
{
    int x, y, w, h;
    if (s_dm_cur != NV2A_VK_DISPLAY_WINDOWED || (SDL_GetWindowFlags(s_win) & (SDL_WINDOW_FULLSCREEN |
                                                                              SDL_WINDOW_MAXIMIZED |
                                                                              SDL_WINDOW_MINIMIZED)))
        return;
    if (!SDL_GetWindowPosition(s_win, &x, &y) || !SDL_GetWindowSize(s_win, &w, &h) || w <= 0 || h <= 0)
        return;
    if (x == s_wr[0] && y == s_wr[1] && w == s_wr[2] && h == s_wr[3])
        return;
    s_wr_gen++;                             /* odd while writing (nv2a_vk_window_rect reads around it) */
    s_wr[0] = x; s_wr[1] = y; s_wr[2] = w; s_wr[3] = h;
    s_wr_gen++;
}

/* the display mode, when it changed (render thread) */
static void dm_apply(void)
{
    static const char *const name[3] = { "windowed", "borderless", "fullscreen (exclusive)" };
    int m = dm_mode(), ok = 1;
    if (!s_win || m == s_dm_cur)
        return;
    if (m == NV2A_VK_DISPLAY_WINDOWED) {
        ok = SDL_SetWindowFullscreen(s_win, false);
    } else {
        const SDL_DisplayMode *mode = NULL;
        if (m == NV2A_VK_DISPLAY_FULLSCREEN)
            mode = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(s_win));   /* the native mode */
        ok = SDL_SetWindowFullscreenMode(s_win, mode) && SDL_SetWindowFullscreen(s_win, true);
    }
    s_dm_cur = m;
    LOGE("display mode: %s%s%s\n", name[m], s_dm_env >= 0 ? " (RECOMP_DISPLAY_MODE)" : "",
         ok ? "" : " -- SDL refused it");
    if (!ok)
        LOGE("SDL: %s\n", SDL_GetError());
}
#endif

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;
    for (i = 0; i < s_memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (s_memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static VkDeviceMemory alloc_mem(VkMemoryRequirements rq, VkMemoryPropertyFlags want)
{
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkDeviceMemory m = VK_NULL_HANDLE;
    ai.allocationSize = rq.size;
    ai.memoryTypeIndex = mem_type(rq.memoryTypeBits, want);
    if (ai.memoryTypeIndex == UINT32_MAX && (want & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
        ai.memoryTypeIndex = mem_type(rq.memoryTypeBits, want & ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX)
        return VK_NULL_HANDLE;
    if (vkAllocateMemory(s_dev, &ai, NULL, &m) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return m;
}

/* ── Texture memory: ranges of large blocks ───────────────────────
 *
 * One vkAllocateMemory per texture cost ~0.12 ms, and a level start creates
 * ~150 textures (THPS 2X: ~17.5 ms of a 40-50 ms frame, M5 b D1). Textures
 * take their memory from device-local blocks instead (first fit,
 * range_alloc.c). Block size: RECOMP_VK_TEX_BLOCK_MB, else TEX_BLOCK_MB, but
 * at most 1/32 of the device-local heap (its budget with
 * VK_EXT_memory_budget), at least 4 MB; a texture over 1/8 of a block gets an
 * allocation of its own, and so do render targets, depth buffers and cube
 * maps. Number of blocks: the texture budget (RECOMP_GL_TEX_MB, tex_account,
 * which evicts the least recently used textures) in blocks, plus one for
 * ranges still waiting in the garbage and for gaps. When they are all full a
 * texture gets an allocation of its own (logged once); when that fails too
 * the texture is drawn with the dummy one (logged). A range goes back with
 * the frame's garbage (after its fence); an emptied block is freed while
 * another one is left. RECOMP_VK_TEX_BLOCKS=0: one allocation per texture. */
#define TEX_BLOCK_MB       16
#define TEX_BLOCK_SLOTS    64

typedef struct {
    uint32_t     blk;                   /* 0: an allocation of its own; n: a range of block n-1 */
    VkDeviceSize off, size;
} TexMem;

typedef struct {
    VkDeviceMemory mem;
    uint32_t       type;
    RangeAlloc     ra;
} TexBlock;
static TexBlock s_tblk[TEX_BLOCK_SLOTS];
static VkDeviceSize s_tblk_bytes;       /* 0 until the first texture; then the block size */
static uint32_t s_tblk_cap;             /* blocks allowed (<= TEX_BLOCK_SLOTS); 0 = off */

static uint64_t vram_free(uint64_t held, uint64_t *used_out);

/* RECOMP_GL_TEX_MB, default 256 MB: what tex_account keeps textures within. */
static uint64_t tex_budget(void)
{
    static uint64_t budget;
    if (!budget) {
        const char *e = getenv("RECOMP_GL_TEX_MB");
        budget = (uint64_t)(e && atoi(e) > 0 ? atoi(e) : 256) << 20;
    }
    return budget;
}

static void tex_blocks_setup(void)
{
    const char *e = getenv("RECOMP_VK_TEX_BLOCKS"), *m = getenv("RECOMP_VK_TEX_BLOCK_MB");
    uint64_t used = 0, heap = vram_free(0, &used) + used, sz;
    s_tblk_bytes = 1;
    if (e && e[0] == '0' && !e[1])
        return;
    if (m && atoi(m) > 0) {
        sz = (uint64_t)atoi(m) << 20;
    } else {
        sz = (uint64_t)TEX_BLOCK_MB << 20;
        while (sz > (4ull << 20) && sz > heap / 32)
            sz >>= 1;
    }
    s_tblk_bytes = sz;
    s_tblk_cap = (uint32_t)((tex_budget() + sz - 1) / sz) + 1;
    if (s_tblk_cap > TEX_BLOCK_SLOTS)
        s_tblk_cap = TEX_BLOCK_SLOTS;
    LOGE("texture blocks: %llu MB each, up to %u (texture budget %llu MB, device-local %llu MB)\n",
         (unsigned long long)(sz >> 20), s_tblk_cap, (unsigned long long)(tex_budget() >> 20),
         (unsigned long long)(heap >> 20));
}

static int tex_mem_alloc(VkMemoryRequirements rq, TexMem *tm)
{
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    uint32_t type, i, slot = TEX_BLOCK_SLOTS;
    uint64_t off;
    TexBlock *b;

    if (!s_tblk_bytes)
        tex_blocks_setup();
    if (!s_tblk_cap || rq.size > s_tblk_bytes / 8)
        return 0;
    type = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
        return 0;
    for (i = 0; i < s_tblk_cap; i++) {
        b = &s_tblk[i];
        if (!b->mem) {
            if (slot == TEX_BLOCK_SLOTS)
                slot = i;
        } else if (b->type == type && ra_alloc(&b->ra, rq.size, rq.alignment, &off)) {
            tm->blk = i + 1; tm->off = off; tm->size = rq.size;
            return 1;
        }
    }
    if (slot == TEX_BLOCK_SLOTS) {
        static int told;
        if (!told++)
            LOGE("texture blocks full (%u x %llu MB): further textures get allocations of their own\n",
                 s_tblk_cap, (unsigned long long)(s_tblk_bytes >> 20));
        return 0;
    }
    b = &s_tblk[slot];
    ai.allocationSize = s_tblk_bytes;
    ai.memoryTypeIndex = type;
    if (!ra_init(&b->ra, s_tblk_bytes))
        return 0;
    if (vkAllocateMemory(s_dev, &ai, NULL, &b->mem) != VK_SUCCESS) {
        ra_destroy(&b->ra);
        memset(b, 0, sizeof *b);
        return 0;
    }
    b->type = type;
    if (!ra_alloc(&b->ra, rq.size, rq.alignment, &off))
        return 0;                       /* not reached: an empty block takes 1/8 of itself at any alignment */
    tm->blk = slot + 1; tm->off = off; tm->size = rq.size;
    return 1;
}

static void tex_mem_release(const TexMem *tm)
{
    TexBlock *b = &s_tblk[tm->blk - 1];
    uint32_t i;
    ra_free(&b->ra, tm->off, tm->size);
    if (b->ra.used)
        return;
    for (i = 0; i < TEX_BLOCK_SLOTS; i++)
        if (s_tblk[i].mem && &s_tblk[i] != b)
            break;
    if (i == TEX_BLOCK_SLOTS)
        return;                         /* the last block stays */
    vkFreeMemory(s_dev, b->mem, NULL);
    ra_destroy(&b->ra);
    memset(b, 0, sizeof *b);
}

/* ── Frames in flight ─────────────────────────────────────────────── */

#define VK_FRAMES 2
#define RING_BYTES (48u << 20)

typedef struct {
    VkImage image; VkImageView view; VkDeviceMemory mem; VkPipeline pipe;
    TexMem tm;                  /* tm.blk: a texture's range of a block */
} Garbage;

typedef struct {
    VkCommandPool   pool;
    VkCommandBuffer cb;
    VkFence         fence;
    VkSemaphore     acquired, rendered;
    VkBuffer        ring;
    VkDeviceMemory  ring_mem;
    uint8_t        *ring_ptr;
    VkDeviceSize    ring_off;
    int             submitted;
    int             stamped;      /* timestamps written this frame (frame_time.h) */
    int             stamping;     /* the begin stamp went in (split counted at frame_begin) */
    Garbage        *garbage;
    uint32_t        ngarbage, garbage_cap;
} Frame;

static Frame  s_fr[VK_FRAMES];
static Frame *s_f;             /* the frame being recorded */
static uint32_t s_fi;
/* Frames in flight, RECOMP_VK_FRAMES=1|2 (default 2). 1 waits for the GPU
 * before recording the next frame: one frame less input-to-present latency,
 * the CPU and GPU no longer overlap, one 48 MB ring less. */
static uint32_t s_nframes = VK_FRAMES;
static VkCommandBuffer s_cb;   /* s_f->cb while recording */
static uint32_t s_cb_gen;      /* bumped at every vkBeginCommandBuffer */

/* Frame-time counter (kernel frame_time.h; declared here so this library
 * keeps no include dependency on the kernel). */
enum { FT_GPU_WAIT = 2, FT_PRESENT = 3 };
extern int  xbox_ft_on(void);
extern void xbox_ft_push(int category);
extern void xbox_ft_pop(void);
/* Page tracking of the contiguous window (kernel page_track.c). */
extern int  xbox_PageTrackCheck(uint32_t va, uint32_t len, uint64_t *stamp);
extern void xbox_PageTrackFrame(void);
extern uint32_t xbox_PageTrackLastWrite(uint32_t va, uint32_t len);
extern uint32_t xbox_PageTrackFrameNo(void);
extern int  xbox_PageTrackPeek(uint32_t va, uint32_t *gen);
extern void xbox_ft_gpu(double ms);
extern int  xbox_ft_frame(void);
extern void xbox_ft_want(int on);
extern int  xbox_ft_last_text(char *buf, size_t n);
extern unsigned xbox_ft_window_text(char *buf, size_t n);
extern void xbox_ft_acquire(double ms);
extern long long xbox_ft_now(void);
extern double xbox_ft_ms(long long since);
/* a [hitch] line noted at the start of vk_flip, printed after xbox_ft_frame
 * closed that frame (with its split, file reads and sleeps) */
static char s_hitch[256];
extern int  xbox_RtWatchOn(void);
extern void xbox_RtWatchArm(uint32_t gpu_va, uint32_t bytes);
extern void xbox_SetContiguousFreeHook(void (*fn)(uint32_t addr, uint32_t size));
static VkQueryPool s_ts_pool;  /* 2 timestamps per frame slot, written while the split is counted */
static double      s_ts_ms;    /* ms per timestamp tick */
static uint64_t    s_ts_mask;
static int s_dyn_valid;
static int s_pipe_foreign;      /* a pipeline not from pipe_get is bound */

static void garbage_add_pipe(VkPipeline pipe);
static void garbage_add(VkImage img, VkImageView view, VkDeviceMemory mem)
{
    Frame *f = s_f;
    if (f->ngarbage == f->garbage_cap) {
        f->garbage_cap = f->garbage_cap ? f->garbage_cap * 2 : 64;
        f->garbage = (Garbage *)realloc(f->garbage, f->garbage_cap * sizeof *f->garbage);
    }
    f->garbage[f->ngarbage].image = img;
    f->garbage[f->ngarbage].view = view;
    f->garbage[f->ngarbage].mem = mem;
    f->garbage[f->ngarbage].pipe = VK_NULL_HANDLE;
    f->garbage[f->ngarbage].tm.blk = 0;
    f->ngarbage++;
}

/* A texture: its own allocation, or its range of a block. */
static void garbage_add_tex(VkImage img, VkImageView view, VkDeviceMemory mem, const TexMem *tm)
{
    garbage_add(img, view, mem);
    s_f->garbage[s_f->ngarbage - 1].tm = *tm;
}

static void garbage_add_pipe(VkPipeline pipe)
{
    garbage_add(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
    s_f->garbage[s_f->ngarbage - 1].pipe = pipe;
}

static void garbage_free(Frame *f)
{
    uint32_t i;
    for (i = 0; i < f->ngarbage; i++) {
        if (f->garbage[i].view) vkDestroyImageView(s_dev, f->garbage[i].view, NULL);
        if (f->garbage[i].image) vkDestroyImage(s_dev, f->garbage[i].image, NULL);
        if (f->garbage[i].mem) vkFreeMemory(s_dev, f->garbage[i].mem, NULL);
        if (f->garbage[i].tm.blk) tex_mem_release(&f->garbage[i].tm);
        if (f->garbage[i].pipe) vkDestroyPipeline(s_dev, f->garbage[i].pipe, NULL);
    }
    f->ngarbage = 0;
}

static void frame_begin(void)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    s_f = &s_fr[s_fi];
    if (s_f->submitted) {
        xbox_ft_push(FT_GPU_WAIT);
        vkWaitForFences(s_dev, 1, &s_f->fence, VK_TRUE, UINT64_MAX);
        xbox_ft_pop();
        vkResetFences(s_dev, 1, &s_f->fence);
        s_f->submitted = 0;
        if (s_f->stamped && s_ts_pool) {
            uint64_t ts[2];
            if (vkGetQueryPoolResults(s_dev, s_ts_pool, s_fi * 2, 2, sizeof ts, ts, sizeof ts[0],
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                xbox_ft_gpu((double)((ts[1] - ts[0]) & s_ts_mask) * s_ts_ms);
        }
        s_f->stamped = 0;
    }
    garbage_free(s_f);
    s_f->ring_off = 0;
    vkResetCommandPool(s_dev, s_f->pool, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s_f->cb, &bi);
    s_f->stamping = s_ts_pool && xbox_ft_on();
    if (s_f->stamping) {
        vkCmdResetQueryPool(s_f->cb, s_ts_pool, s_fi * 2, 2);
        vkCmdWriteTimestamp(s_f->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_ts_pool, s_fi * 2);
    }
    s_cb = s_f->cb;
    s_cb_gen++;
    s_dyn_valid = 0;
}

/* Space in this frame's ring: returns the offset, *ptr the mapping. */
static void flush_frame_wait(void);
static VkDeviceSize ring_alloc(VkDeviceSize size, VkDeviceSize align, void **ptr)
{
    VkDeviceSize at = (s_f->ring_off + align - 1) & ~(align - 1);
    if (at + size > RING_BYTES) {
        /* A frame larger than the ring: finish what is recorded, wait for
         * it, and start over in the same ring. Slow, but only in extremes. */
        static int warned;
        if (!warned++)
            LOGE("frame ring (%u MB) full: flushing mid-frame\n", RING_BYTES >> 20);
        flush_frame_wait();
        at = 0;
        if (size > RING_BYTES) {
            *ptr = NULL;
            return 0;
        }
    }
    s_f->ring_off = at + size;
    *ptr = s_f->ring_ptr + at;
    return at;
}

/* ── Rendering passes and barriers ────────────────────────────────── */

/* w, h are the surface in the title's pixels (anti-aliasing included) --
 * what the shaders, clip rectangles and texture lookups work in. pw, ph are
 * the image: w, h times the render scale. */
typedef struct {
    uint32_t va, w, h, pw, ph;
    VkImage image; VkImageView view; VkDeviceMemory mem;
    uint32_t used;
    uint32_t aa_sx, aa_sy;
    int dirty;                 /* written since the last barrier */
    uint32_t gen;              /* bumped by every draw and clear into it */
    uint32_t bytes;            /* guest bytes of its last use: row pitch x clip height (RT watch) */
    uint32_t row;              /* row pitch in bytes (bpp included) */
    uint32_t span;             /* guest bytes for the freed-block test: row x tallest clip, clamped to its block */
    /* ANTIALIASING = MSAA (ms_set): the multisampled image the passes draw
     * into, resolved into image at every pass end; ms = its sample count, 0
     * none. Only the presented surface gets one. */
    VkImage ms_image; VkImageView ms_view; VkDeviceMemory ms_mem;
    uint32_t ms;
} VkSurf;

/* the boot animation's frame (nv2a_vk_boot_frame): presented in place of a
 * title surface while s_boot_on */
static VkSurf s_boot;
static int s_boot_on;

typedef struct {
    uint32_t va, pw, ph;
    VkImage image; VkImageView view; VkDeviceMemory mem;
    uint32_t used;
    uint32_t bytes;            /* guest bytes (the colour target's extent; zeta pitch is the same) */
    uint32_t ms;               /* sample count of a multisampled surface's depth, 0 single */
} VkDepthBuf;

#define VK_MAX_SURF 32
#define VK_MAX_DEPTH 16

/* RECOMP_GL_SCALE, as in nv2a_gl: only the pixels behind a surface grow, so
 * the viewport, clear rectangles, read-backs and the present blit scale and
 * nothing else does. Capped per surface by the device's image limits.
 * Horizontal: s_scale times s_xratio, 1 unless RECOMP_GL_SCALE_X sets the
 * horizontal scale outright (a game option's rescale keeps the ratio).
 * s_square: widescreen shows the title's 640x480 at 16:9, so surfaces
 * without horizontal AA get 4/3 more columns -- square pixels (races: 1.5x
 * -> 1280x720). The front end's 1280x480 (aa 2x1) is wide enough already.
 * RECOMP_VK_SQUARE=0 off; RECOMP_GL_SCALE_X overrides it. */
static double   s_scale = 1.0, s_scale_x = 1.0, s_xratio = 1.0, s_xratio_env = 1.0;
static int      s_square, s_square_env;

/* The largest render scale: 5x the title's lines (480 -> 2400; RENDER
 * RESOLUTION, porting_guide.md M6). X may add 4/3 for 16:9. Maintainer,
 * 2026-10-23: capped at 5x -- THPS 2X on an RTX 3050 Ti held 60 fps at 5x 16:9
 * (8533x2400 back buffer) but missed vblanks at 5.5x and 6x 16:9 (frames
 * waiting on the GPU at ~5 ms of timed GPU work). VK_SCALE_STEPS: the menu's
 * 0.25 steps above 1x (17 values). */
#define VK_SCALE_MAX 5.0
#define VK_SCALE_STEPS 16

/* The port's RENDER RESOLUTION option (nv2a_vk_set_render_scale): the
 * vertical factor in thousandths (1.875 = 1600x900 at 16:9), 0 = none (RECOMP_GL_SCALE / nv2a_vk_scale_pct
 * decide), and whether gameplay is 16:9 -- then frames the port has not
 * marked 4:3 (xbox_SetPresentWide) get 4/3 more columns. Written by any
 * thread, applied at the next flip. RECOMP_GL_SCALE[_X] set = s_scale_env:
 * the env wins and the option is ignored. */
static volatile int s_opt_scale_q, s_opt_wide;
static int      s_scale_env;

/* The port's "menus stretched" option (nv2a_vk_set_stretch_43): with 16:9
 * on, the frames the port marks 4:3 (menus, movies) fill the 16:9 window
 * instead of being pillarboxed, as the Xbox dashboard's widescreen setting
 * did for 4:3 titles. RECOMP_STRETCH_43=1|0 forces it. Read at every flip. */
static volatile int s_stretch_opt;
static int      s_stretch_env = -1;              /* -2 not set, else forced */

static int stretch_43(void)
{
    if (s_stretch_env == -1) {
        const char *e = getenv("RECOMP_STRETCH_43");
        s_stretch_env = !e || !*e ? -2 : *e == '1';
    }
    return s_stretch_env == -2 ? s_stretch_opt : s_stretch_env;
}

void nv2a_vk_set_stretch_43(int on)
{
    s_stretch_opt = on ? 1 : 0;
}

/* Which option values fit the device, for the menu to grey out: bit i =
 * 1.00x + 0.25 x i (the 17 values), [0] at 4:3, [1] at 16:9. Worked out on
 * the render thread at the flips (scale_fits), 0 until the first one;
 * s_fit_bad keeps the factors whose surfaces failed to allocate. */
static volatile uint32_t s_fit_mask[2];
static uint32_t s_fit_bad[2];
static volatile int s_fit_known;
static int      s_have_budget;   /* VK_EXT_memory_budget: the driver's own VRAM figures */

/* Picture options set at run time (NFSU2: Options -> Video), read by the
 * renderer: FXAA on/off (at every present), anisotropic level 1..16 (per
 * sampler key, capped by RECOMP_VK_ANISO and the device) and square pixels
 * on/off (at the next flip, like nv2a_vk_scale_pct). The env switches
 * (RECOMP_VK_FXAA=0, RECOMP_VK_SQUARE=0, ...) still force them off. */
volatile int nv2a_vk_fxaa = 1;
/* RECOMP_VK_FFP_LIGHT=0: fixed-function draws keep their vertex colours
 * unlit, as before the lighting was in the vertex shader (A/B checks). */
static int s_ffp_light = 1;
static int s_ffp_mark;         /* RECOMP_VK_FFP_MARK=1: lit fixed-function draws in magenta (debug) */
/* RECOMP_VK_TEXGEN=0: fixed-function texgen off (the vertex's own coordinates, as
 * before); =t<n> off every other n frames (default 600), for on/off pictures. */
static int s_ffp_texgen = 1;
static uint32_t s_ffp_texgen_period = 600;
volatile int nv2a_vk_aniso = 16;
volatile int nv2a_vk_square = 1;
static uint32_t s_max_size = 4096;

/* Render scale asked for at run time, in percent (a game option: NFSU2's
 * Options -> Video); 0 = none, keep RECOMP_GL_SCALE; 50..500. Applied at the
 * next flip (rescale_surfaces). nv2a_vk_set_render_scale takes precedence. */
volatile int nv2a_vk_scale_pct;

/* The stored size of a w x h surface at the current scale, capped per axis
 * by the device's image limits. */
static void stored_size(uint32_t w, uint32_t h, uint32_t aa_sx, uint32_t *pw, uint32_t *ph)
{
    double kx = s_scale_x * (s_square && aa_sx <= 1 ? 4.0 / 3.0 : 1.0), ky = s_scale;
    if (w * kx > (double)s_max_size || h * ky > (double)s_max_size) {
        static int told;
        if (!told++)
            fprintf(stderr, "  [VK] render scale %g x %g: a %ux%u surface exceeds the device's %u -- capped\n",
                    kx, ky, w, h, s_max_size);
    }
    if (w * kx > (double)s_max_size)
        kx = (double)s_max_size / w;
    if (h * ky > (double)s_max_size)
        ky = (double)s_max_size / h;
    *pw = (uint32_t)(w * kx + 0.5); *ph = (uint32_t)(h * ky + 0.5);
    if (!*pw) *pw = 1;
    if (!*ph) *ph = 1;
}

/* v title pixels of a surface l wide, in the p stored pixels behind it. */
static uint32_t to_stored(uint32_t v, uint32_t p, uint32_t l)
{
    return (uint32_t)(((uint64_t)v * p + l / 2) / l);
}
static VkSurf     s_surf[VK_MAX_SURF];
static VkDepthBuf s_depth[VK_MAX_DEPTH];

/* ── Render scale: what fits the device ─────────────────────────────── */

static int debug_on(void);

/* Device-local memory still free: the driver's budget minus its usage with
 * VK_EXT_memory_budget, otherwise 80 % of the largest device-local heap minus
 * the held bytes (what this renderer's surfaces and depth buffers use). */
static uint64_t vram_free(uint64_t held, uint64_t *used_out)
{
    uint64_t total = 0, used = 0;
    uint32_t i, best = UINT32_MAX;

    if (s_have_budget) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT b =
            { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
        VkPhysicalDeviceMemoryProperties2 p = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &b };
        vkGetPhysicalDeviceMemoryProperties2(s_pd, &p);
        for (i = 0; i < p.memoryProperties.memoryHeapCount; i++)
            if ((p.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    && (best == UINT32_MAX || p.memoryProperties.memoryHeaps[i].size
                                              > p.memoryProperties.memoryHeaps[best].size))
                best = i;
        if (best != UINT32_MAX) {
            total = b.heapBudget[best];
            used = b.heapUsage[best];
        }
    } else {
        for (i = 0; i < s_memprops.memoryHeapCount; i++)
            if ((s_memprops.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    && s_memprops.memoryHeaps[i].size > total)
                total = s_memprops.memoryHeaps[i].size;
        total = total / 5 * 4;
        used = held;
    }
    if (used_out)
        *used_out = used;
    return total > used ? total - used : 0;
}

static uint64_t depth_bpp(void)
{
    return s_depth_fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ? 8u : 4u;
}

static uint64_t held_bytes(void)
{
    uint64_t held = 0;
    int i;
    for (i = 0; i < VK_MAX_SURF; i++)
        if (s_surf[i].image)
            held += (uint64_t)s_surf[i].pw * s_surf[i].ph * 4u;
    for (i = 0; i < VK_MAX_DEPTH; i++)
        if (s_depth[i].image)
            held += (uint64_t)s_depth[i].pw * s_depth[i].ph * depth_bpp();
    return held;
}

/* Whether the surfaces held now fit at kx x ky: every one within the
 * device's image limits. No free-memory estimate (maintainer, 2026-10-26: no
 * VRAM limit, the player decides) -- an image that then fails to allocate
 * sends the whole render scale back to the step before (rescale_surfaces) and
 * marks the factor unfit. Before the first surface a 640x480 frame stands in. */
static int scale_fits(double kx, double ky)
{
    double w, h;
    int i, any = 0;

    for (i = 0; i < VK_MAX_SURF; i++) {
        const VkSurf *s = &s_surf[i];
        if (!s->image)
            continue;
        w = s->w * kx + 0.5; h = s->h * ky + 0.5;
        if (w > (double)s_max_size || h > (double)s_max_size)
            return 0;
        any = 1;
    }
    if (!any && (640 * kx > (double)s_max_size || 480 * ky > (double)s_max_size))
        return 0;
    return 1;
}

/* The fit masks for the menu (s_fit_mask), at a flip. */
static void update_fit_mask(void)
{
    int a, i;
    for (a = 0; a < 2; a++) {
        uint32_t m = 0;
        for (i = 0; i <= VK_SCALE_STEPS; i++) {
            double f = 1.0 + 0.25 * i;
            if (scale_fits(f * (a ? 4.0 / 3.0 : 1.0), f))
                m |= 1u << i;
        }
        s_fit_mask[a] = m & ~s_fit_bad[a];
    }
    s_fit_known = 1;
}

/* An image failed to allocate at the current scale: that factor and every
 * larger one are unfit from now on (the port's menu refuses them). */
static int    s_scale_fail;                     /* an image failed since the last flip */
static double s_good_k = 1.0, s_good_xr = 1.0;   /* the last scale a flip ran without one */
static int    s_good_sq;

static void scale_failed(void)
{
    int a = s_xratio > 1.01, i = (int)((s_scale - 1.0) / 0.25 + 0.5);
    s_scale_fail = 1;
    if (s_scale <= 1.0)
        return;
    if (i > VK_SCALE_STEPS) i = VK_SCALE_STEPS;
    s_fit_bad[a] |= ~0u << i;
    s_fit_mask[a] &= ~s_fit_bad[a];
    fprintf(stderr, "  [VK] render scale %g x %g: out of device memory -- %gx and above marked unfit\n",
            s_scale_x, s_scale, 1.0 + 0.25 * i);
}

void nv2a_vk_set_render_scale(double factor, int wide)
{
    s_opt_wide = wide ? 1 : 0;
    s_opt_scale_q = factor > 0.0 ? (int)(factor * 1000.0 + 0.5) : 0;
}

int nv2a_vk_render_scale_fits(double factor, int wide)
{
    int i = (int)ceil((factor - 1.0) / 0.25 - 1e-6);   /* between two steps: the larger one */
    if (!(factor >= 1.0) || factor > VK_SCALE_MAX)
        return 0;
    if (!s_fit_known)
        return 1;
    return (int)((s_fit_mask[wide ? 1 : 0] >> i) & 1u);
}

static VkSurf    *s_last;
/* The surface a flip shows: the last one drawn into with the display's pitch
 * (AvSetDisplayMode), i.e. a framebuffer the CRTC could scan out. s_last --
 * the last target of any kind -- is only the fallback: the pushbuffer flip
 * runs at FLIP_STALL, which D3D sends just before the next frame's first
 * draw into the back buffer, so off-screen passes of the next frame come
 * first. THPS 2X renders its skater shadows (256x256, 16 bit) there, after
 * resolving the 2x-wide AA back buffer into a front buffer; showing s_last
 * put the shadow texture on screen. */
static VkSurf    *s_scan;
static uint32_t   s_scan_pitch;

static int         s_in_render;
static VkSurf     *s_rt;
static VkDepthBuf *s_rt_depth;

static void barrier_all(void)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    int i;
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    for (i = 0; i < VK_MAX_SURF; i++)
        s_surf[i].dirty = 0;
}

static void end_rendering(void)
{
    if (s_in_render) {
        p_end_rendering(s_cb);
        s_in_render = 0;
    }
}

static VkRect2D s_sc_cur;               /* scissor set in this pass */

static void set_viewport(uint32_t pw, uint32_t ph)
{
    VkViewport vp = { 0, 0, (float)pw, (float)ph, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { pw, ph } };
    vkCmdSetViewport(s_cb, 0, 1, &vp);
    vkCmdSetScissor(s_cb, 0, 1, &sc);
    s_sc_cur = sc;
}

/* SET_SURFACE_CLIP as the scissor, in s's stored pixels. NFSU2's split
 * screen draws each player's world with the clip set to that player's
 * half; the vertex programs place it there but do not stop at the edge.
 * Same as nv2a_gl.c (and xemu). Call inside the pass (begin_rendering
 * resets it to the whole surface). */
static void set_surface_scissor(const uint32_t *r, const VkSurf *s)
{
    uint32_t x0 = (r[0x200 / 4] & 0xFFFF) * s->aa_sx, x1 = x0 + (r[0x200 / 4] >> 16) * s->aa_sx;
    uint32_t y0 = (r[0x204 / 4] & 0xFFFF) * s->aa_sy, y1 = y0 + (r[0x204 / 4] >> 16) * s->aa_sy;
    VkRect2D sc = { { 0, 0 }, { s->pw, s->ph } };
    int windowed = 0;

    /* SET_WINDOW_CLIP, inclusive (SET_WINDOW_CLIP_TYPE 0): a pixel is kept
     * inside window 0 (xemu psh.c: XMIN..XMAX inclusive, in surface pixels
     * before anti-aliasing). Titles set window 0 only (THPS 2X: menu panels;
     * the other seven stay 0 = one pixel at the origin), so the window joins
     * the scissor instead of a per-pixel test. Exclusive windows: not
     * applied. */
    if (r[0x2B4 / 4] == 0 && (r[0x2C0 / 4] || r[0x2E0 / 4])) {
        uint32_t wx0 = (r[0x2C0 / 4] & 0xFFF) * s->aa_sx, wx1 = (((r[0x2C0 / 4] >> 16) & 0xFFF) + 1) * s->aa_sx;
        uint32_t wy0 = (r[0x2E0 / 4] & 0xFFF) * s->aa_sy, wy1 = (((r[0x2E0 / 4] >> 16) & 0xFFF) + 1) * s->aa_sy;
        if (x1 <= x0 || y1 <= y0) {          /* no surface clip: the window alone */
            x0 = 0; y0 = 0; x1 = s->w; y1 = s->h;
        }
        if (wx0 > x0) x0 = wx0;
        if (wy0 > y0) y0 = wy0;
        if (wx1 < x1) x1 = wx1;
        if (wy1 < y1) y1 = wy1;
        windowed = 1;
    }
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (windowed && (x1 <= x0 || y1 <= y0)) {
        sc.extent.width = sc.extent.height = 0;   /* nothing inside the window */
    } else if (x1 > x0 && y1 > y0) {
        sc.offset.x = (int32_t)to_stored(x0, s->pw, s->w);
        sc.offset.y = (int32_t)to_stored(y0, s->ph, s->h);
        sc.extent.width = to_stored(x1, s->pw, s->w) - (uint32_t)sc.offset.x;
        sc.extent.height = to_stored(y1, s->ph, s->h) - (uint32_t)sc.offset.y;
    }
    if (memcmp(&sc, &s_sc_cur, sizeof sc)) {
        vkCmdSetScissor(s_cb, 0, 1, &sc);
        s_sc_cur = sc;
    }
}

static void begin_rendering(VkSurf *s, VkDepthBuf *d)
{
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };

    if (d && d->ms != s->ms)
        d = NULL;                       /* a depth of the other sample count (MSAA just switched) */
    if (s_in_render && s_rt == s && s_rt_depth == d)
        return;
    end_rendering();
    barrier_all();
    ca.imageView = s->view;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (s->ms) {
        /* MSAA: the passes draw into the multisampled image (kept, loaded by
         * the next pass) and resolve into the surface's own image when the
         * pass ends -- everything else reads that one */
        ca.imageView = s->ms_view;
        ca.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        ca.resolveImageView = s->view;
        ca.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    ri.renderArea.extent.width = s->pw;
    ri.renderArea.extent.height = s->ph;
    if (d) {
        if (d->pw < ri.renderArea.extent.width) ri.renderArea.extent.width = d->pw;
        if (d->ph < ri.renderArea.extent.height) ri.renderArea.extent.height = d->ph;
    }
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    if (d) {
        da.imageView = d->view;
        da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = &da;
        ri.pStencilAttachment = &da;
    }
    p_begin_rendering(s_cb, &ri);
    s_in_render = 1;
    s_rt = s;
    s_rt_depth = d;
    set_viewport(s->pw, s->ph);
}

/* An image in GENERAL layout, from UNDEFINED; recorded outside rendering. */
static void image_to_general(VkImage img, VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    end_rendering();
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* make_image_ms sets it for one image (MSAA), 1 otherwise */
static VkSampleCountFlagBits s_mk_samples = VK_SAMPLE_COUNT_1_BIT;

/* MSAA state (ms_flip): the sample counts the device has for colour and depth
 * (bits 2 / 4), the ones that failed to allocate this run, a failure since
 * the last flip, and the count that last worked (the fallback) */
static uint32_t s_ms_max, s_ms_bad, s_ms_fail, s_ms_good;

/* layers: 1 = a 2D image, 6 = a cube map (cube view); levels: mip levels.
 * tm (textures): memory from a block when it fits (*mem stays null), else an
 * allocation of its own (tm->blk 0); NULL: always one of its own. */
static int make_image_ml(uint32_t w, uint32_t h, uint32_t layers, uint32_t levels, VkFormat fmt,
                         VkImageUsageFlags usage, VkImageAspectFlags aspect,
                         VkImage *img, VkImageView *view, VkDeviceMemory *mem, TexMem *tm)
{
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryRequirements rq;

    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent.width = w;
    ci.extent.height = h;
    ci.extent.depth = 1;
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    if (layers == 6)
        ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.samples = s_mk_samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    {
        /* RECOMP_VK_FAIL_ALLOC_MB=<n> (test switch): every image above n MB
         * (4 bytes a pixel and sample) fails as out of device memory would,
         * to show the fallbacks of the render scale and MSAA. */
        static double fail_mb = -1.0;
        if (fail_mb < 0.0) {
            const char *e = getenv("RECOMP_VK_FAIL_ALLOC_MB");
            fail_mb = e && *e ? atof(e) : 0.0;
        }
        if (fail_mb > 0.0 && (double)w * h * layers * (double)ci.samples * 4.0 > fail_mb * 1048576.0) {
            LOGE("RECOMP_VK_FAIL_ALLOC_MB: %ux%u x%u refused (test switch)\n", w, h, (unsigned)ci.samples);
            *img = VK_NULL_HANDLE;
            return 0;
        }
    }
    if (vkCreateImage(s_dev, &ci, NULL, img) != VK_SUCCESS)
        return 0;
    vkGetImageMemoryRequirements(s_dev, *img, &rq);
    *mem = VK_NULL_HANDLE;
    if (tm) {
        tm->blk = 0;
        if (tex_mem_alloc(rq, tm)
            && vkBindImageMemory(s_dev, *img, s_tblk[tm->blk - 1].mem, tm->off) != VK_SUCCESS) {
            tex_mem_release(tm);
            tm->blk = 0;
        }
    }
    if (!tm || !tm->blk) {
        *mem = alloc_mem(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!*mem || vkBindImageMemory(s_dev, *img, *mem, 0) != VK_SUCCESS) {
            vkDestroyImage(s_dev, *img, NULL);
            if (*mem) vkFreeMemory(s_dev, *mem, NULL);
            *img = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE;
            return 0;
        }
    }
    vi.image = *img;
    vi.viewType = layers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT;
    vi.subresourceRange.levelCount = levels;
    vi.subresourceRange.layerCount = layers;
    if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (vkCreateImageView(s_dev, &vi, NULL, view) != VK_SUCCESS) {
        vkDestroyImage(s_dev, *img, NULL);
        if (*mem) vkFreeMemory(s_dev, *mem, NULL);
        if (tm && tm->blk) tex_mem_release(tm);
        *img = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE;
        if (tm) tm->blk = 0;
        return 0;
    }
    image_to_general(*img, aspect);
    return 1;
}

static int make_image_n(uint32_t w, uint32_t h, uint32_t layers, VkFormat fmt,
                        VkImageUsageFlags usage, VkImageAspectFlags aspect,
                        VkImage *img, VkImageView *view, VkDeviceMemory *mem)
{
    return make_image_ml(w, h, layers, 1, fmt, usage, aspect, img, view, mem, NULL);
}

static int make_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect, VkImage *img, VkImageView *view,
                      VkDeviceMemory *mem)
{
    return make_image_n(w, h, 1, fmt, usage, aspect, img, view, mem);
}

/* a surface's multisampled image goes (with the surface, or MSAA off) */
static void ms_drop(VkSurf *s)
{
    if (s->ms_image)
        garbage_add(s->ms_image, s->ms_view, s->ms_mem);
    s->ms_image = VK_NULL_HANDLE; s->ms_view = VK_NULL_HANDLE; s->ms_mem = VK_NULL_HANDLE;
    s->ms = 0;
}

/* a multisampled 2D image (samples 2 / 4) */
static int make_image_ms(uint32_t w, uint32_t h, uint32_t samples, VkFormat fmt, VkImageUsageFlags usage,
                         VkImageAspectFlags aspect, VkImage *img, VkImageView *view, VkDeviceMemory *mem)
{
    int ok;
    s_mk_samples = (VkSampleCountFlagBits)samples;
    ok = make_image_n(w, h, 1, fmt, usage, aspect, img, view, mem);
    s_mk_samples = VK_SAMPLE_COUNT_1_BIT;
    return ok;
}

/* ── Surfaces ─────────────────────────────────────────────────────── */

static VkSurf *surf_find(uint32_t va)
{
    int i;
    for (i = 0; i < VK_MAX_SURF; i++)
        if (s_surf[i].image && s_surf[i].va == va)
            return &s_surf[i];
    return NULL;
}

static void clear_image_color(VkImage img, float r, float g, float b, float a)
{
    VkClearColorValue c = { { r, g, b, a } };
    VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                   VK_REMAINING_ARRAY_LAYERS };
    end_rendering();
    vkCmdClearColorImage(s_cb, img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &rg);
}

static VkSurf *surf_get(uint32_t va, uint32_t w, uint32_t h, uint32_t aa_sx, uint32_t aa_sy)
{
    VkSurf *s = surf_find(va), *victim = NULL;
    int i;

    /* h is the surface clip, not the allocation (NFSU2's split screen clips
     * the back buffer to each player's half): keep the surface while the
     * clip fits, grow it when it does not. Same as nv2a_gl.c. */
    if (s && s->w == w && s->h >= h) {
        s->used = s_frame;
        return s;
    }
    if (s && s->w == w && s->h > h)
        h = s->h;
    if (!s) {
        for (i = 0; i < VK_MAX_SURF; i++) {
            if (!s_surf[i].image) { victim = &s_surf[i]; break; }
            if (!victim || s_surf[i].used < victim->used) victim = &s_surf[i];
        }
        s = victim;
    }
    if (s->image) {
        if (s_rt == s) end_rendering(), s_rt = NULL;
        if (s_last == s) s_last = NULL;
        if (s_scan == s) s_scan = NULL;
        garbage_add(s->image, s->view, s->mem);
        ms_drop(s);
    }
    memset(s, 0, sizeof *s);
    s->va = va; s->w = w; s->h = h; s->used = s_frame;
    s->aa_sx = aa_sx; s->aa_sy = aa_sy;
    stored_size(w, h, aa_sx, &s->pw, &s->ph);
    if (!make_image(s->pw, s->ph, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &s->image, &s->view, &s->mem)) {
        /* Out of device memory at this render scale: this surface falls back
         * to the title's own size (every coordinate is per surface, so a
         * mixed set still draws right) and the factor is marked unfit. */
        int scaled = s->pw != w || s->ph != h;
        LOGE("surface 0x%08X %ux%u: image creation failed at %ux%u%s\n", va, w, h, s->pw, s->ph,
             scaled ? " -- falling back to the native size" : "");
        scale_failed();
        s->pw = w; s->ph = h;
        if (!scaled || !make_image(w, h, VK_FORMAT_B8G8R8A8_UNORM,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT, &s->image, &s->view, &s->mem)) {
            memset(s, 0, sizeof *s);
            return NULL;
        }
    }
    clear_image_color(s->image, 0, 0, 0, 1);
    if (s_trace || debug_on())
        LOGE("surface 0x%08X %ux%u (aa %ux%u, stored %ux%u)\n", va, w, h, aa_sx, aa_sy,
             s->pw, s->ph);
    return s;
}

/* A new render scale (nv2a_vk_scale_pct), at a flip, outside rendering:
 * every surface is rebuilt at its new stored size with its pixels scaled
 * over -- render targets the title keeps across frames (glow accumulators,
 * reflection faces, the frame being presented) do not go black -- and the
 * depth buffers are dropped; depth_get makes new ones on the next draw. */
static void rescale_surfaces(double k)
{
    int i, copied = 0;

    s_scale = k;
    s_scale_x = k * s_xratio;
    fprintf(stderr, "  [VK] rendering at %g x %g%s\n", s_scale_x, k,
            s_square ? ", square pixels" : "");
    for (i = 0; i < VK_MAX_SURF; i++) {
        VkSurf *s = &s_surf[i];
        VkImage img; VkImageView view; VkDeviceMemory mem;
        VkImageBlit bl;
        uint32_t pw, ph;
        if (!s->image)
            continue;
        stored_size(s->w, s->h, s->aa_sx, &pw, &ph);
        if (pw == s->pw && ph == s->ph)
            continue;
        if (!make_image(pw, ph, VK_FORMAT_B8G8R8A8_UNORM,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, &img, &view, &mem)) {
            LOGE("surface 0x%08X %ux%u: image creation failed -- keeps %ux%u\n", s->va, pw, ph,
                 s->pw, s->ph);
            scale_failed();
            continue;
        }
        if (!copied) {
            barrier_all();
            copied = 1;
        }
        memset(&bl, 0, sizeof bl);
        bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.srcSubresource.layerCount = 1;
        bl.srcOffsets[1].x = (int32_t)s->pw;
        bl.srcOffsets[1].y = (int32_t)s->ph;
        bl.srcOffsets[1].z = 1;
        bl.dstSubresource = bl.srcSubresource;
        bl.dstOffsets[1].x = (int32_t)pw;
        bl.dstOffsets[1].y = (int32_t)ph;
        bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(s_cb, s->image, VK_IMAGE_LAYOUT_GENERAL, img,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        garbage_add(s->image, s->view, s->mem);
        ms_drop(s);                     /* MSAA: made again at the new size at the next flip */
        s->image = img; s->view = view; s->mem = mem;
        s->pw = pw; s->ph = ph;
        s->gen++;                       /* cube maps built from it refill */
    }
    for (i = 0; i < VK_MAX_DEPTH; i++)
        if (s_depth[i].image) {
            garbage_add(s_depth[i].image, s_depth[i].view, s_depth[i].mem);
            memset(&s_depth[i], 0, sizeof s_depth[i]);
        }
    if (copied)
        barrier_all();
}

static VkDepthBuf *depth_get(uint32_t va, uint32_t pw, uint32_t ph, uint32_t ms)
{
    VkDepthBuf *d, *victim = NULL;
    int i, ok;

    for (i = 0; i < VK_MAX_DEPTH; i++) {
        if (s_depth[i].image && s_depth[i].va == va && s_depth[i].pw == pw && s_depth[i].ph == ph
            && s_depth[i].ms == ms) {
            s_depth[i].used = s_frame;
            return &s_depth[i];
        }
        if (!victim || (victim->image && (!s_depth[i].image || s_depth[i].used < victim->used)))
            victim = &s_depth[i];
    }
    d = victim;
    if (d->image) {
        if (s_rt_depth == d) end_rendering(), s_rt = NULL, s_rt_depth = NULL;
        garbage_add(d->image, d->view, d->mem);
    }
    memset(d, 0, sizeof *d);
    d->va = va; d->pw = pw; d->ph = ph; d->used = s_frame;
    if (ms)
        ok = make_image_ms(pw, ph, ms, s_depth_fmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                           &d->image, &d->view, &d->mem);
    else
        ok = make_image(pw, ph, s_depth_fmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                        &d->image, &d->view, &d->mem);
    if (!ok) {
        LOGE("depth 0x%08X %ux%u%s: image creation failed\n", va, pw, ph, ms ? " (MSAA)" : "");
        if (ms)
            s_ms_fail |= ms;                /* MSAA back to the count before (ms_flip) */
        else
            scale_failed();
        memset(d, 0, sizeof *d);
        return NULL;
    }
    d->ms = ms;
    {
        VkClearDepthStencilValue z = { 1.0f, 0 };
        VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
        vkCmdClearDepthStencilImage(s_cb, d->image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg);
    }
    if (s_trace)
        LOGE("depth 0x%08X %ux%u\n", va, pw, ph);
    return d;
}

/* ── Freed contiguous memory ─────────────────────────────────────────
 *
 * Surfaces and depth buffers are keyed on their guest address. With the
 * kernel's reclaim (the default) a title frees a render target's block and
 * the address comes back as something else -- a texture, a smaller target.
 * A surface left in the cache then shadows the new owner: a texture at that
 * address samples the old GPU image instead of the bytes the title wrote.
 * MmFreeContiguousMemory reports each freed block here (any guest thread);
 * the render path drains the queue before it picks a target and at the flip,
 * and drops every surface and depth buffer that overlaps a freed block. */
#define FREEQ_MAX 256
typedef struct { uint32_t lo, hi; } FreeSpan;
static FreeSpan s_freeq[FREEQ_MAX];
static int s_freeq_n, s_freeq_over;
static uint32_t s_freeq_over_lo, s_freeq_over_hi;
static volatile int s_freeq_lock;
static unsigned s_drop_surf, s_drop_depth, s_drop_tex;
static unsigned tex_drop_freed(const FreeSpan *q, int n);   /* textures section below */
extern uint32_t xbox_ContiguousBlockSize(uint32_t addr);

static void freeq_lock(void)   { while (__atomic_exchange_n(&s_freeq_lock, 1, __ATOMIC_ACQUIRE)) ; }
static void freeq_unlock(void) { __atomic_store_n(&s_freeq_lock, 0, __ATOMIC_RELEASE); }

static void on_contig_free(uint32_t addr, uint32_t size)
{
    uint32_t lo = addr & 0x0FFFFFFFu, hi = lo + size;
    freeq_lock();
    if (s_freeq_n < FREEQ_MAX) {
        s_freeq[s_freeq_n].lo = lo;
        s_freeq[s_freeq_n].hi = hi;
        s_freeq_n++;
    } else {                    /* full: one span covering the rest, dropped conservatively */
        if (!s_freeq_over || lo < s_freeq_over_lo) s_freeq_over_lo = lo;
        if (!s_freeq_over || hi > s_freeq_over_hi) s_freeq_over_hi = hi;
        s_freeq_over = 1;
    }
    freeq_unlock();
}

static int debug_on(void)
{
    if (s_debug < 0) {
        const char *e = getenv("RECOMP_DEBUG");
        s_debug = (e && *e && *e != '0') || s_trace;
    }
    return s_debug;
}

static void drain_frees(void)
{
    static FreeSpan q[FREEQ_MAX + 1];
    int n, i, j;

    if (!__atomic_load_n(&s_freeq_n, __ATOMIC_RELAXED) && !__atomic_load_n(&s_freeq_over, __ATOMIC_RELAXED))
        return;
    freeq_lock();
    n = s_freeq_n;
    memcpy(q, s_freeq, (size_t)n * sizeof q[0]);
    if (s_freeq_over) {
        q[n].lo = s_freeq_over_lo;
        q[n].hi = s_freeq_over_hi;
        n++;
    }
    s_freeq_n = 0;
    s_freeq_over = 0;
    freeq_unlock();

    for (i = 0; i < VK_MAX_SURF; i++) {
        VkSurf *s = &s_surf[i];
        uint32_t lo, hi;
        if (!s->image)
            continue;
        lo = s->va & 0x0FFFFFFFu;
        hi = lo + (s->span ? s->span : 1);
        for (j = 0; j < n; j++)
            if (lo < q[j].hi && q[j].lo < hi)
                break;
        if (j == n)
            continue;
        if (debug_on())
            LOGE("surface 0x%08X %ux%u (%u bytes) dropped: block 0x%08X..0x%08X freed\n",
                 s->va, s->w, s->h, s->span, q[j].lo | 0x80000000u, (q[j].hi - 1) | 0x80000000u);
        if (s_rt == s) end_rendering(), s_rt = NULL;
        if (s_last == s) s_last = NULL;
        if (s_scan == s) s_scan = NULL;
        garbage_add(s->image, s->view, s->mem);
        ms_drop(s);
        memset(s, 0, sizeof *s);
        s_drop_surf++;
    }
    for (i = 0; i < VK_MAX_DEPTH; i++) {
        VkDepthBuf *d = &s_depth[i];
        uint32_t lo, hi;
        if (!d->image)
            continue;
        lo = d->va & 0x0FFFFFFFu;
        hi = lo + (d->bytes ? d->bytes : 1);
        for (j = 0; j < n; j++)
            if (lo < q[j].hi && q[j].lo < hi)
                break;
        if (j == n)
            continue;
        if (debug_on())
            LOGE("depth 0x%08X %ux%u (%u bytes) dropped: block 0x%08X..0x%08X freed\n",
                 d->va, d->pw, d->ph, d->bytes, q[j].lo | 0x80000000u, (q[j].hi - 1) | 0x80000000u);
        if (s_rt_depth == d) end_rendering(), s_rt = NULL, s_rt_depth = NULL;
        garbage_add(d->image, d->view, d->mem);
        memset(d, 0, sizeof *d);
        s_drop_depth++;
    }
    {
        unsigned t = tex_drop_freed(q, n);
        s_drop_tex += t;
        if (debug_on() && t)
            LOGE("freed blocks so far: %u surfaces, %u depth buffers, %u textures dropped\n",
                 s_drop_surf, s_drop_depth, s_drop_tex);
    }
}

/* The target of a draw or clear: surface (created on first use) and the
 * depth buffer of its zeta address. */
static VkSurf *target(const Nv2aSurface *sf, uint32_t zeta_va, VkDepthBuf **dout)
{
    uint32_t bpp = sf->bytes_per_pixel ? sf->bytes_per_pixel : 4;
    uint32_t w = sf->pitch ? sf->pitch / bpp : sf->width;
    uint32_t h = sf->clip_y + sf->height;   /* the clip need not start at 0 */
    VkSurf *s;

    *dout = NULL;
    drain_frees();
    if (w < sf->clip_x + sf->width) w = sf->clip_x + sf->width;
    if (!w || !h || !sf->color_va || w > 4096 || h > 4096)
        return NULL;
    if (s_trace) {
        static uint32_t last[6];
        uint32_t cur[6] = { sf->color_va, sf->pitch, sf->clip_x, sf->clip_y, sf->width, sf->height };
        if (memcmp(cur, last, sizeof cur)) {
            memcpy(last, cur, sizeof cur);
            LOGE("target 0x%08X pitch %u bpp %u clip %u,%u %ux%u -> %ux%u\n", sf->color_va,
                 sf->pitch, bpp, sf->clip_x, sf->clip_y, sf->width, sf->height, w, h);
        }
    }
    s = surf_get(sf->color_va, w, h, sf->aa_sx ? sf->aa_sx : 1, sf->aa_sy ? sf->aa_sy : 1);
    if (s) {
        /* Guest extent from this use: the real row pitch (bpp included) times
         * the clip height -- not the image's width x tallest clip x 4. */
        uint32_t row = sf->pitch ? sf->pitch : w * bpp;
        s->bytes = row * h;
        /* The freed-block test is conservative: the tallest clip the surface
         * was ever used with, but never past the block it starts (looked up
         * only when the extent changes -- the arena scan is not per draw). */
        if (s->row != row || s->span < row * s->h) {
            uint32_t span = row * s->h;
            uint32_t blk = xbox_ContiguousBlockSize(0x80000000u | (s->va & 0x0FFFFFFFu));
            s->row = row;
            s->span = (blk && span > blk) ? blk : span;
        }
    }
    if (s && zeta_va) {
        *dout = depth_get(zeta_va, s->pw, s->ph, s->ms);
        if (*dout)
            (*dout)->bytes = s->span;
    }
    if (s) {
        s_last = s;
        if (s_scan_pitch && sf->pitch == s_scan_pitch)
            s_scan = s;
    }
    return s;
}

/* ── Textures ─────────────────────────────────────────────────────── */

typedef struct {
    uint32_t va, color, w, h, pitch, hash, bytes;
    uint32_t face;              /* cube map: bytes per face; 0 = 2D */
    uint32_t levels;            /* mip levels in the image */
    VkImage image; VkImageView view; VkDeviceMemory mem;
    TexMem tm;                  /* mem null: a range of a block (tex_mem_alloc) */
    VkFormat fmt;
    uint32_t used, checked;
    uint64_t pstamp;            /* page-tracking stamp of its bytes (page_track.c) */
} VkTex;

/* RECOMP_VK_MIPS=0: level 0 only (the old look). RECOMP_VK_ANISO=<n>:
 * anisotropic filtering of mipmapped textures, 1 = off (default 16, capped
 * by the device). RECOMP_VK_LOD_BIAS: their mip bias. */
static int   s_mips = 1;
static float s_aniso = 16.0f, s_lod_bias = -0.25f;

#define VK_MAX_TEX 1024
#define TEX_BUCKETS 2048
static VkTex    s_tex[VK_MAX_TEX];
static uint16_t s_tex_head[TEX_BUCKETS], s_tex_next[VK_MAX_TEX];
static uint64_t s_tex_bytes;
static uint32_t s_palette_reg, s_palette_va;
static VkImage  s_dummy_img;
static VkImageView s_dummy_view;
static VkDeviceMemory s_dummy_mem;
/* A transparent black cube, for a cube map stage with nothing to sample. */
static VkImage  s_dummy_cube_img;
static VkImageView s_dummy_cube_view;
static VkDeviceMemory s_dummy_cube_mem;

/* ── FPS counter ─────────────────────────────────────────────────────
 * Off by default (not a settings overlay): up to two lines in the top-left
 * corner of the window, frames per second and the longest frame in ms, both
 * over the last half second. The port's option (nv2a_vk_set_fps_hud) picks
 * which, at run time; RECOMP_FPS_HUD forces it: 1 both lines (as before the
 * option), fps, ms, 0 off. Drawn with transfer blits from two 1x1 images (no
 * pipeline, no font texture; made at start even while off, so the option can
 * turn it on) after the picture is in the swapchain image, so screenshots
 * and surface dumps never contain it. */
static int s_hud = -1;              /* env: -2 not set, else the forced mode */
static volatile int s_hud_opt;      /* the port's mode, NV2A_VK_HUD_* */
static VkImage s_hud_img[2];        /* 0 black, 1 white */
static VkImageView s_hud_view[2];
static VkDeviceMemory s_hud_mem[2];
static uint64_t s_hud_t0, s_hud_last;
static uint32_t s_hud_n;
static uint64_t s_hud_max;
static char s_hud_txt[2][8];

/* the mode in use: NV2A_VK_HUD_OFF / _FPS / _BOTH / _MS */
static int hud_mode(void)
{
    if (s_hud == -1) {
        const char *e = getenv("RECOMP_FPS_HUD");
        s_hud = !e ? -2 : !strcmp(e, "1") ? NV2A_VK_HUD_BOTH : !strcmp(e, "fps") ? NV2A_VK_HUD_FPS
              : !strcmp(e, "ms") ? NV2A_VK_HUD_MS : NV2A_VK_HUD_OFF;
    }
    return s_hud == -2 ? s_hud_opt : s_hud;
}

static int hud_on(void)
{
    return hud_mode() != NV2A_VK_HUD_OFF;
}

void nv2a_vk_set_fps_hud(int mode)
{
    s_hud_opt = mode >= NV2A_VK_HUD_OFF && mode <= NV2A_VK_HUD_MS ? mode : NV2A_VK_HUD_OFF;
}

/* Per flip: count frames, keep the longest gap, refresh the text twice a second. */
static void hud_tick(void)
{
    uint64_t now = hz_now();
    if (s_hud_last) {
        uint64_t dt = now - s_hud_last;
        if (dt > s_hud_max)
            s_hud_max = dt;
        s_hud_n++;
    } else {
        s_hud_t0 = now;
    }
    s_hud_last = now;
    if (now - s_hud_t0 >= 500000000ull && s_hud_n) {
        double fps = (double)s_hud_n * 1e9 / (double)(now - s_hud_t0);
        double ms = (double)s_hud_max / 1e6;
        snprintf(s_hud_txt[0], sizeof s_hud_txt[0], "%u", (unsigned)(fps + 0.5));
        snprintf(s_hud_txt[1], sizeof s_hud_txt[1], "%.1f", ms > 999.0 ? 999.0 : ms);
        s_hud_t0 = now;
        s_hud_n = 0;
        s_hud_max = 0;
    }
}

/* 3x5 digits, rows top first, bit 2 = left column. */
static const uint8_t s_hud_font[10][5] = {
    { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 }, { 5, 5, 7, 1, 1 },
    { 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 }, { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 },
};

static void hud_rect(VkImageBlit *bl, int x, int y, int w, int h)
{
    memset(bl, 0, sizeof *bl);
    bl->srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bl->srcSubresource.layerCount = 1;
    bl->srcOffsets[1].x = 1;
    bl->srcOffsets[1].y = 1;
    bl->srcOffsets[1].z = 1;
    bl->dstSubresource = bl->srcSubresource;
    bl->dstOffsets[0].x = x;
    bl->dstOffsets[0].y = y;
    bl->dstOffsets[1].x = x + w;
    bl->dstOffsets[1].y = y + h;
    bl->dstOffsets[1].z = 1;
}

/* dst is in TRANSFER_DST_OPTIMAL and was just written by transfers. */
static void hud_draw(VkImage dst)
{
    VkImageBlit bl[128];
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    int u = (int)s_sc_extent.height / 180, n = 0, line, w = 0, mode = hud_mode();
    /* the lines shown: fps (txt[0]) and / or the longest frame (txt[1]) */
    const char *txt[2] = { mode == NV2A_VK_HUD_MS ? s_hud_txt[1] : s_hud_txt[0],
                           mode == NV2A_VK_HUD_BOTH ? s_hud_txt[1] : "" };
    int lines = mode == NV2A_VK_HUD_BOTH ? 2 : 1;
    if (!s_hud_img[0] || !s_hud_txt[0][0])
        return;
    if (u < 2)
        u = 2;
    for (line = 0; line < lines; line++) {
        const char *c;
        int cw = 0;
        for (c = txt[line]; *c; c++)
            cw += *c == '.' ? 2 * u : 4 * u;
        if (cw > w)
            w = cw;
    }
    if ((int)s_sc_extent.width < w + 4 * u || (int)s_sc_extent.height < 16 * u)
        return;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
    hud_rect(&bl[0], u, u, w + 2 * u, lines * 7 * u);
    vkCmdBlitImage(s_cb, s_hud_img[0], VK_IMAGE_LAYOUT_GENERAL, dst,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, bl, VK_FILTER_NEAREST);
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
    for (line = 0; line < lines; line++) {
        const char *c;
        int x = 2 * u, y = 2 * u + line * 7 * u;
        for (c = txt[line]; *c; c++) {
            if (*c == '.') {
                if (n < 128)
                    hud_rect(&bl[n++], x, y + 4 * u, u, u);
                x += 2 * u;
                continue;
            }
            if (*c >= '0' && *c <= '9') {
                int r;
                for (r = 0; r < 5; r++) {
                    uint8_t bits = s_hud_font[*c - '0'][r];
                    int col = 0;
                    while (col < 3) {           /* one rect per run of set pixels */
                        int start;
                        if (!(bits & (4 >> col))) { col++; continue; }
                        start = col;
                        while (col < 3 && (bits & (4 >> col)))
                            col++;
                        if (n < 128)
                            hud_rect(&bl[n++], x + start * u, y + r * u, (col - start) * u, u);
                    }
                }
            }
            x += 4 * u;
        }
    }
    if (n)
        vkCmdBlitImage(s_cb, s_hud_img[1], VK_IMAGE_LAYOUT_GENERAL, dst,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)n, bl, VK_FILTER_NEAREST);
}


uint64_t nv2a_gl_texture_bytes(void) { return s_tex_bytes; }
uint32_t nv2a_gl_frame_count(void) { return s_frame; }

static uint32_t tex_bucket(uint32_t va, uint32_t color, uint32_t w, uint32_t h)
{
    uint32_t k = va ^ (color << 24) ^ (w << 12) ^ (h << 4);
    k ^= k >> 15; k *= 0x2C1B3C6Du; k ^= k >> 12;
    return k & (TEX_BUCKETS - 1);
}
static void tex_index_add(VkTex *t)
{
    uint32_t b = tex_bucket(t->va, t->color, t->w, t->h);
    uint16_t i = (uint16_t)(t - s_tex);
    s_tex_next[i] = s_tex_head[b];
    s_tex_head[b] = (uint16_t)(i + 1);
}
static void tex_index_del(VkTex *t)
{
    uint16_t *at = &s_tex_head[tex_bucket(t->va, t->color, t->w, t->h)];
    uint16_t i = (uint16_t)(t - s_tex);
    while (*at) {
        if (*at == i + 1) { *at = s_tex_next[i]; return; }
        at = &s_tex_next[*at - 1];
    }
}

static int tex_size_from_format(uint32_t color)
{
    if (color >= 0x0C && color <= 0x0F)
        return 1;
    switch (color) {
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
    case 0x16: case 0x17: case 0x18: case 0x1B: case 0x1C: case 0x1D:
    case 0x1E: case 0x1F: case 0x20: case 0x24: case 0x25: case 0x26:
    case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x34: case 0x35:
    case 0x36: case 0x37: case 0x40: case 0x41: case 0x43: case 0x44:
    case 0x45: case 0x46:
        return 0;
    default:
        return 1;
    }
}

static uint32_t bytes_hash(uint32_t va, uint32_t bytes)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset() + va;
    uint32_t h = 2166136261u ^ bytes, step, i;
    if (!bytes)
        return h;
    step = bytes / 256;
    if (step < 4) step = 4;
    step &= ~3u;
    for (i = 0; i + 4 <= bytes; i += step) {
        uint32_t w;
        memcpy(&w, mem + i, 4);
        h = (h ^ w) * 16777619u;
    }
    memcpy(&i, mem + bytes - 4, 4);
    return (h ^ i) * 16777619u;
}

/* Every byte, for linear images. A sampled hash steps bytes/256, which for a
 * power-of-two pitch is a whole number of rows: every sample lands in the same
 * column. Carbon's EA logo movie (512x256 A8R8G8B8, pitch 2048, a ring of
 * three) is black down that column, so its frames were never re-uploaded and
 * the logo cycled through the first three. Linear images are few (movies,
 * CPU-written pictures), so reading all of them once a frame is cheap. */
static uint32_t bytes_hash_full(uint32_t va, uint32_t bytes)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset() + va;
    uint64_t a = 14695981039346656037ull ^ bytes, b = 1099511628211ull, x, y;
    uint32_t i;

    for (i = 0; i + 16 <= bytes; i += 16) {
        memcpy(&x, mem + i, 8);
        memcpy(&y, mem + i + 8, 8);
        a = (a ^ x) * 1099511628211ull;
        b = (b ^ y) * 1099511628211ull;
    }
    for (; i < bytes; i++)
        a = (a ^ mem[i]) * 1099511628211ull;
    a ^= b * 31u;
    return (uint32_t)(a ^ (a >> 32));
}

static uint32_t tex_bytes(uint32_t color, uint32_t w, uint32_t h, uint32_t pitch)
{
    if (color == 0x0C) return ((w + 3) / 4) * ((h + 3) / 4) * 8;
    if (color == 0x0E || color == 0x0F) return ((w + 3) / 4) * ((h + 3) / 4) * 16;
    if (pitch) return pitch * h;
    return w * h * 4;
}

static uint32_t swizzle_index(uint32_t u, uint32_t v, uint32_t w, uint32_t h)
{
    uint32_t out = 0, bit = 0, mw = w - 1, mh = h - 1, m;
    for (m = 1; mw >= m || mh >= m; m <<= 1) {
        if (mw >= m) { if (u & m) out |= 1u << bit; bit++; }
        if (mh >= m) { if (v & m) out |= 1u << bit; bit++; }
    }
    return out;
}

/* The swizzle splits into an x part and a y part (their bits never share a
 * position), so a whole image needs two small tables instead of a bit loop
 * per texel: index(u, v) = s_swz_x[u] | s_swz_y[v]. */
static uint32_t s_swz_x[4096], s_swz_y[4096], s_swz_w, s_swz_h;

static void swizzle_tables(uint32_t w, uint32_t h)
{
    uint32_t i;
    if (w == s_swz_w && h == s_swz_h)
        return;
    for (i = 0; i < w; i++) s_swz_x[i] = swizzle_index(i, 0, w, h);
    for (i = 0; i < h; i++) s_swz_y[i] = swizzle_index(0, i, w, h);
    s_swz_w = w;
    s_swz_h = h;
}

static int decode_indexed(uint32_t va, uint32_t w, uint32_t h, uint32_t *out)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t entries = 256u >> ((s_palette_reg >> 2) & 3);
    const uint8_t *idx = mem + va;
    uint32_t x, y;
    uint32_t pal[256];
    if (!(s_palette_reg & ~0x3Fu))
        return 0;
    memcpy(pal, mem + s_palette_va, entries * 4);
    for (x = entries; x < 256; x++)
        pal[x] = pal[0];                     /* past the end reads entry 0 */
    swizzle_tables(w, h);
    for (y = 0; y < h; y++) {
        const uint8_t *row = idx + s_swz_y[y];
        uint32_t *o = out + (size_t)y * w;
        for (x = 0; x < w; x++)
            o[x] = pal[row[s_swz_x[x]]];
    }
    return 1;
}

static void tex_free(VkTex *t)
{
    garbage_add_tex(t->image, t->view, t->mem, &t->tm);
    s_tex_bytes -= t->bytes;
    tex_index_del(t);
    memset(t, 0, sizeof *t);
}

/* Freed contiguous blocks (drain_frees): a texture keyed on an address in a
 * freed block goes too. Page tracking only sees CPU writes; a block that comes
 * back as a render target is written by the GPU alone, and the old texture at
 * that address would be served unchanged. */
static unsigned tex_drop_freed(const FreeSpan *q, int n)
{
    unsigned dropped = 0;
    int i, j;
    for (i = 0; i < VK_MAX_TEX; i++) {
        VkTex *t = &s_tex[i];
        uint32_t lo, len;
        if (!t->image)
            continue;
        lo = t->va & 0x0FFFFFFFu;
        len = t->pitch ? tex_bytes(t->color, t->w, t->h, t->pitch)
                       : (t->face ? t->face * 6 : tex_bytes(t->color, t->w, t->h, 0));
        if (!len)
            len = 1;
        for (j = 0; j < n; j++)
            if (lo < q[j].hi && q[j].lo < lo + len)
                break;
        if (j == n)
            continue;
        if (debug_on() && dropped < 8)
            LOGE("texture 0x%08X fmt %02X %ux%u dropped: block 0x%08X..0x%08X freed\n", t->va, t->color,
                 t->w, t->h, q[j].lo | 0x80000000u, (q[j].hi - 1) | 0x80000000u);
        tex_free(t);
        dropped++;
    }
    if (debug_on() && dropped > 8)
        LOGE("... %u textures dropped for freed blocks\n", dropped);
    return dropped;
}

static void tex_account(VkTex *t, uint32_t bytes)
{
    uint64_t budget = tex_budget();
    s_hz_texs++;
    s_hz_tex_bytes += bytes;
    s_tex_bytes += (uint64_t)bytes - t->bytes;
    t->bytes = bytes;
    while (s_tex_bytes > budget) {
        VkTex *lru = NULL;
        uint32_t i;
        for (i = 0; i < VK_MAX_TEX; i++) {
            VkTex *c = &s_tex[i];
            if (c->image && c != t && c->used != s_frame && (!lru || c->used < lru->used))
                lru = c;
        }
        if (!lru)
            break;
        tex_free(lru);
    }
}

/* Copy the image's texels, already in the ring at `at` (row length
 * `row_texels`, 0 = tight), into mip level `level` of layer `layer`. */
static void tex_copy_from_ring(VkTex *t, VkDeviceSize at, uint32_t row_texels,
                               uint32_t layer, uint32_t level)
{
    VkBufferImageCopy rg;

    end_rendering();
    if (!level)
        barrier_all();
    memset(&rg, 0, sizeof rg);
    rg.bufferOffset = at;
    rg.bufferRowLength = row_texels;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.mipLevel = level;
    rg.imageSubresource.baseArrayLayer = layer;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = t->w >> level ? t->w >> level : 1;
    rg.imageExtent.height = t->h >> level ? t->h >> level : 1;
    rg.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cb, s_f->ring, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
}

/* Copy `bytes` at `src` (row length `row_texels`, 0 = tight) into the image. */
static void tex_upload(VkTex *t, const void *src, uint32_t bytes, uint32_t row_texels,
                       uint32_t layer, uint32_t level)
{
    void *dst;
    VkDeviceSize at = ring_alloc(bytes, 16, &dst);

    if (!dst)
        return;
    memcpy(dst, src, bytes);
    tex_copy_from_ring(t, at, row_texels, layer, level);
}

/* Mip levels 1.. of every layer from level 0, by GPU blits (decoded
 * textures: building them costs no CPU, unlike decoding the title's). */
static void gen_mips(VkImage img, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    uint32_t l;

    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    for (l = 1; l < levels; l++) {
        VkImageBlit bl;
        vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
        memset(&bl, 0, sizeof bl);
        bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.srcSubresource.mipLevel = l - 1;
        bl.srcSubresource.layerCount = layers;
        bl.srcOffsets[1].x = (int32_t)w;
        bl.srcOffsets[1].y = (int32_t)h;
        bl.srcOffsets[1].z = 1;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        bl.dstSubresource = bl.srcSubresource;
        bl.dstSubresource.mipLevel = l;
        bl.dstOffsets[1].x = (int32_t)w;
        bl.dstOffsets[1].y = (int32_t)h;
        bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(s_cb, img, VK_IMAGE_LAYOUT_GENERAL, img,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    }
}

static uint32_t full_levels(uint32_t w, uint32_t h)
{
    uint32_t n = 1;
    while ((w > h ? w : h) >> n) n++;
    return n;
}

/* Cube map faces follow each other, each with its mip chain, padded to 128
 * bytes (xemu's NV2A_CUBEMAP_FACE_ALIGNMENT); +X -X +Y -Y +Z -Z, the same
 * order and texel orientation as Vulkan's cube layers. */
static uint32_t cube_face_bytes(uint32_t color, uint32_t w, uint32_t h, uint32_t levels)
{
    uint32_t n = 0, l;

    if (!levels)
        levels = 1;
    for (l = 0; l < levels; l++) {
        n += tex_bytes(color, w, h, 0);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return (n + 127) & ~127u;
}

/* One 2D image (layer 0) or cube face from guest memory at va. */
static void tex_fill(VkTex *t, uint32_t va, uint32_t layer)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t color = t->color, w = t->w, h = t->h, pitch = t->pitch, *px;
    Nv2aTexture nt;
    VkDeviceSize at;
    void *map;

    if (t->fmt != VK_FORMAT_B8G8R8A8_UNORM) {
        /* DXT as it is, with the title's mip levels (they follow level 0). */
        uint32_t l, lw = w, lh = h;
        for (l = 0; l < t->levels; l++) {
            uint32_t n = tex_bytes(color, lw, lh, 0);
            tex_upload(t, mem + va, n, 0, layer, l);
            va += n;
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
        }
        return;
    }
    if (color == 0x12 && pitch && !(pitch & 3) && pitch / 4 >= w) {
        /* Linear A8R8G8B8 (movie frames): the bytes are B8G8R8A8 already. */
        tex_upload(t, mem + va, pitch * (h - 1) + w * 4, pitch / 4, layer, 0);
        return;
    }
    /* Everything else is decoded straight into the upload ring. */
    at = ring_alloc(w * h * 4, 16, &map);
    px = (uint32_t *)map;
    if (!px)
        return;
    if (color == 0x06 || color == 0x07) {
        const uint32_t *src = (const uint32_t *)(mem + va);
        uint32_t x, y, fill = color == 0x07 ? 0xFF000000u : 0;
        swizzle_tables(w, h);
        for (y = 0; y < h; y++) {
            const uint32_t *row = src + s_swz_y[y];
            uint32_t *out = px + (size_t)y * w;
            for (x = 0; x < w; x++)
                out[x] = row[s_swz_x[x]] | fill;
        }
    } else {
        int ok;
        memset(&nt, 0, sizeof nt);
        nt.offset = va; nt.width = w; nt.height = h; nt.pitch = pitch; nt.color = color;
        nt.addr_u = nt.addr_v = 1;
        ok = color == 0x0B ? decode_indexed(va, w, h, px)
                           : nv2a_backend_decode_texture(&nt, px);
        if (!ok) {
            uint32_t k;
            for (k = 0; k < w * h; k++)
                px[k] = 0xFFFF00FFu;
            if (s_trace)
                LOGE("texture 0x%08X format 0x%02X not decodable\n", va, color);
        }
    }
    tex_copy_from_ring(t, at, 0, layer, 0);
}

/* face: bytes per cube face (cube_face_bytes), 0 for a 2D texture;
 * levels: mip levels to sample (1 = none). */
static VkImageView tex_get(uint32_t va, uint32_t color, uint32_t w, uint32_t h, uint32_t pitch,
                           uint32_t face, uint32_t levels)
{
    VkTex *t = NULL, *victim = NULL;
    uint32_t i, hash;
    uint64_t pstamp;
    VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
    uint32_t layers = face ? 6 : 1, f, size;

    for (i = s_tex_head[tex_bucket(va, color, w, h)]; i; i = s_tex_next[i - 1]) {
        VkTex *c = &s_tex[i - 1];
        if (c->image && c->va == va && c->color == color && c->w == w && c->h == h
            && c->face == face) {
            t = c;
            break;
        }
    }
    if (t && t->checked == s_frame) {
        t->used = s_frame;
        return t->view;
    }
    {
        /* Page tracking (toolkit page_track.c): pages unwritten since the
         * texture was last read need no hash. Checked -- and so armed --
         * before the bytes are hashed or uploaded, so a write in between
         * shows up next time. Palettised textures also depend on the
         * palette: hashed as before. */
        uint32_t len = pitch ? tex_bytes(color, w, h, pitch)
                             : (face ? face * 6 : tex_bytes(color, w, h, pitch));
        pstamp = t ? t->pstamp : 0;
        if (color != 0x0B && xbox_PageTrackCheck(va, len, &pstamp) && t) {
            t->used = t->checked = s_frame;
            return t->view;
        }
    }
    hash = pitch ? bytes_hash_full(va, tex_bytes(color, w, h, pitch))
             : bytes_hash(va, face ? face * 6 : tex_bytes(color, w, h, pitch));
    if (color == 0x0B)
        hash ^= bytes_hash(s_palette_va, 1024) * 31u;
    if (t && t->hash == hash) {
        t->used = t->checked = s_frame;
        t->pstamp = pstamp;
        return t->view;
    }
    if (s_tex_changes < 0) {
        const char *e = getenv("RECOMP_TEX_CHANGES");
        s_tex_changes = e && atoi(e) > 0;
    }
    if (t && s_tex_changes)
        fprintf(stderr, "[tex] frame %u: %08X format %02X %ux%u changed\n",
                s_frame, va, color, w, h);
    if (s_have_bc && color == 0x0C) fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    if (s_have_bc && color == 0x0E) fmt = VK_FORMAT_BC2_UNORM_BLOCK;
    if (s_have_bc && color == 0x0F) fmt = VK_FORMAT_BC3_UNORM_BLOCK;
    if (t && (t->fmt != fmt || t->pitch != pitch || t->levels != levels)) {
        tex_free(t);
        t = NULL;
    }
    if (!t) {
        for (i = 0; i < VK_MAX_TEX; i++) {
            VkTex *c = &s_tex[i];
            if (!c->image) { if (!victim || victim->image) victim = c; }
            else if (!victim || (victim->image && c->used < victim->used)) victim = c;
        }
        t = victim;
        if (t->image)
            tex_free(t);
        t->va = va; t->color = color; t->w = w; t->h = h; t->pitch = pitch; t->fmt = fmt;
        t->face = face; t->levels = levels;
        VT("texture %08X fmt %02X %ux%u pitch %u%s\n", va, color, w, h, pitch,
           face ? " cube" : "");
        if (!make_image_ml(w, h, layers, levels, fmt,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, &t->image, &t->view, &t->mem, &t->tm)) {
            static unsigned failed;
            if (failed++ < 8 || !(failed & (failed - 1)))
                LOGE("texture 0x%08X %ux%u: no image or device memory -- drawn with the dummy texture "
                     "(%u so far; %llu KB of textures held)\n", va, w, h, failed,
                     (unsigned long long)(s_tex_bytes >> 10));
            memset(t, 0, sizeof *t);
            return face ? s_dummy_cube_view : s_dummy_view;
        }
        tex_index_add(t);
        if (s_trace)
            LOGE("texture 0x%08X format 0x%02X %ux%u pitch %u%s\n", va, color, w, h, pitch,
                 face ? " cube" : "");
    }
    t->hash = hash;
    t->pstamp = pstamp;
    t->used = t->checked = s_frame;

    for (f = 0; f < layers; f++)
        tex_fill(t, va + f * face, f);
    if (fmt == VK_FORMAT_B8G8R8A8_UNORM && levels > 1)
        gen_mips(t->image, w, h, levels, layers);
    size = fmt != VK_FORMAT_B8G8R8A8_UNORM ? tex_bytes(color, w, h, 0) : w * h * 4;
    if (levels > 1)
        size += size / 3;                   /* the mip chain */
    tex_account(t, size * layers);
    return t->view;
}

/* A cube map whose faces the title renders (race car reflections: six
 * 128x128 targets, one per face, every frame). The faces exist only as
 * surface images, so the cube is assembled from them on the GPU, again
 * whenever a face was drawn into since the last copy. */
typedef struct {
    uint32_t va, w, h, face, size, levels;
    VkImage image; VkImageView view; VkDeviceMemory mem;
    VkSurf  *src[6];
    uint32_t gen[6];
    uint32_t used;
} VkCube;
#define VK_MAX_CUBE 4
static VkCube s_cube[VK_MAX_CUBE];

static VkImageView cube_from_surfaces(uint32_t va, uint32_t w, uint32_t h, uint32_t face,
                                      VkSurf *faces[6])
{
    VkCube *c = NULL;
    uint32_t f, size = 0;
    int i, copied = 0;

    for (f = 0; f < 6; f++)
        if (faces[f] && to_stored(w, faces[f]->pw, faces[f]->w) > size)
            size = to_stored(w, faces[f]->pw, faces[f]->w);
    for (i = 0; i < VK_MAX_CUBE; i++)
        if (s_cube[i].image && s_cube[i].va == va && s_cube[i].w == w && s_cube[i].h == h
            && s_cube[i].face == face && s_cube[i].size == size) {
            c = &s_cube[i];
            break;
        }
    if (!c) {
        for (i = 0; i < VK_MAX_CUBE; i++)
            if (!c || !s_cube[i].image || (c->image && s_cube[i].used < c->used))
                c = &s_cube[i];
        if (c->image)
            garbage_add(c->image, c->view, c->mem);
        memset(c, 0, sizeof *c);
        /* Mipmapped (sampled trilinear, bind_stage): a reflection on a
         * curved or distant body no longer sparkles. */
        c->levels = s_mips ? full_levels(size, size) : 1;
        if (!make_image_ml(size, size, 6, c->levels, VK_FORMAT_B8G8R8A8_UNORM,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, &c->image, &c->view, &c->mem, NULL)) {
            memset(c, 0, sizeof *c);
            return s_dummy_cube_view;
        }
        clear_image_color(c->image, 0, 0, 0, 0);
        c->va = va; c->w = w; c->h = h; c->face = face; c->size = size;
        if (s_trace)
            LOGE("cube 0x%08X %ux%u from surfaces (stored %u)\n", va, w, h, size);
    }
    c->used = s_frame;
    for (f = 0; f < 6; f++) {
        VkSurf *sf = faces[f];
        VkImageBlit bl;
        if (!sf || (c->src[f] == sf && c->gen[f] == sf->gen))
            continue;
        if (!copied) {
            end_rendering();
            barrier_all();
            copied = 1;
        }
        memset(&bl, 0, sizeof bl);
        bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.srcSubresource.layerCount = 1;
        bl.srcOffsets[1].x = (int32_t)to_stored(w, sf->pw, sf->w);
        bl.srcOffsets[1].y = (int32_t)to_stored(h, sf->ph, sf->h);
        bl.srcOffsets[1].z = 1;
        bl.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.dstSubresource.baseArrayLayer = f;
        bl.dstSubresource.layerCount = 1;
        bl.dstOffsets[1].x = (int32_t)size;
        bl.dstOffsets[1].y = (int32_t)size;
        bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(s_cb, sf->image, VK_IMAGE_LAYOUT_GENERAL, c->image,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        c->src[f] = sf;
        c->gen[f] = sf->gen;
    }
    if (copied) {
        if (c->levels > 1)
            gen_mips(c->image, size, size, c->levels, 6);
        barrier_all();
    }
    return c->view;
}

/* Samplers: wrap u, wrap v, mag, min, mipmapped. */
typedef struct { uint32_t key; VkSampler s; } SamplerEnt;
static SamplerEnt s_samplers[512];
static int s_nsamplers;

static VkSamplerAddressMode wrap_mode(uint32_t m)
{
    switch (m) {
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 4:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

static VkSampler sampler_get(uint32_t key)
{
    VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    int i;
    for (i = 0; i < s_nsamplers; i++)
        if (s_samplers[i].key == key)
            return s_samplers[i].s;
    ci.addressModeU = wrap_mode(key & 0xF);
    ci.addressModeV = wrap_mode((key >> 8) & 0xF);
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.magFilter = (key & 0x10000) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = (key & 0x20000) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.maxLod = 0.0f;
    if (key & 0x40000) {
        /* The title's mipmapped filters, always trilinear (GPU time only). */
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        ci.maxLod = VK_LOD_CLAMP_NONE;
        ci.mipLodBias = s_lod_bias;
        if (key >> 20) {
            ci.anisotropyEnable = VK_TRUE;
            ci.maxAnisotropy = (float)(key >> 20);
        }
    }
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (s_nsamplers == 512)
        s_nsamplers = 0;                    /* never in practice: 6 x 6 x 8 keys x levels */
    if (vkCreateSampler(s_dev, &ci, NULL, &s_samplers[s_nsamplers].s) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    s_samplers[s_nsamplers].key = key;
    return s_samplers[s_nsamplers++].s;
}

/* A surface kept taller than its clip (surf_get) and sampled as a texture
 * of the clip's size: texcoords cover the w x h texels at the top left, not
 * the whole surface. A texture of the surface's logical or real size keeps
 * the old full-surface mapping (anti-aliased surfaces). */
static void rt_scale(uint32_t w, uint32_t h, uint32_t sw, uint32_t sh,
                     uint32_t aa_sx, uint32_t aa_sy, float scale[2])
{
    uint32_t lw = sw / (aa_sx ? aa_sx : 1), lh = sh / (aa_sy ? aa_sy : 1);

    if (w < lw) scale[0] *= (float)w / (float)lw;
    if (h < lh) scale[1] *= (float)h / (float)lh;
}

/* Car reflections on/off at run time, for a game option (NFSU2: Options ->
 * Video). Read on the executor thread at every cube stage bind. */
volatile int nv2a_vk_cube_maps = 1;

/* Stage i: its image view (a surface or a cached texture), sampler and
 * texcoord scale. Records uploads, so call it outside rendering or accept
 * that it ends the pass. */
static void bind_stage(const uint32_t *regs, int i, float scale[2], VkDescriptorImageInfo *out,
                       VkSurf **sampled)
{
    /* Texture mode CUBE_MAP: the shader declares a samplerCube (gl_psh.c),
     * so the descriptor must be a cube view whatever is bound. */
    int cube = ((regs[0x1E70 / 4] >> (5 * i)) & 0x1F) == 3;
    /* RECOMP_VK_CUBE=0, or the title's option (nv2a_vk_cube_maps): cube
     * stages sample the black dummy (no car reflections, the old look). */
    static int cube_env = -1;
    if (cube_env < 0) {
        const char *e = getenv("RECOMP_VK_CUBE");
        cube_env = !(e && *e == '0');
    }
    int cube_on = cube_env && nv2a_vk_cube_maps;
    uint32_t base = (0x1B00u + (uint32_t)i * 0x40u) / 4;
    uint32_t control0 = regs[base + 3];
    uint32_t format = regs[base + 1];
    uint32_t color = (format >> 8) & 0xFF;
    uint32_t va, w, h, pitch = 0, addr, filter, key, levels = 1, minf;
    int dyn_cube = 0;
    VkSurf *rt;

    scale[0] = scale[1] = 1.0f;
    out->imageView = cube ? s_dummy_cube_view : s_dummy_view;
    out->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    out->sampler = sampler_get(0);
    *sampled = NULL;
    if (!(control0 & 0x40000000u) || !regs[base])
        return;
    va = s_batch->tex_va[i];
    if (tex_size_from_format(color)) {
        uint32_t most;
        w = 1u << ((format >> 20) & 0xF);
        h = 1u << ((format >> 24) & 0xF);
        /* SET_TEXTURE_FORMAT [19:16]: mip levels, down to 1x1 at most. */
        most = full_levels(w, h);
        levels = (format >> 16) & 0xF;
        if (!levels || !s_mips) levels = 1;
        if (levels > most) levels = most;
    } else {
        uint32_t rect = regs[base + 7];
        w = rect >> 16;
        h = rect & 0xFFFF;
        pitch = regs[base + 4] >> 16;
        if (w) scale[0] = 1.0f / (float)w;
        if (h) scale[1] = 1.0f / (float)h;
    }
    if (!w || !h || w > 4096 || h > 4096)
        return;
    s_palette_reg = regs[base + 8];
    s_palette_va = s_batch->pal_va[i];
    rt = cube ? NULL : surf_find(va);
    if (cube) {
        /* SET_TEXTURE_FORMAT bit 2: CUBEMAP_ENABLE; mip levels in [19:16]. */
        if (cube_on && (format & 4)) {
            uint32_t face = cube_face_bytes(color, w, h, (format >> 16) & 0xF), f;
            VkSurf *faces[6];
            int any = 0;
            for (f = 0; f < 6; f++)
                any |= (faces[f] = surf_find(va + f * face)) != NULL;
            if (any) {
                out->imageView = cube_from_surfaces(va, w, h, face, faces);
                levels = s_mips ? 2 : 1;            /* has a chain: see below */
                dyn_cube = 1;
            } else {
                uint32_t texs = s_hz_texs;
                uint64_t t0 = hz_now();
                out->imageView = tex_get(va, color, w, h, 0, face, levels);
                if (s_hz_texs != texs)
                    s_hz_tex_ns += hz_now() - t0;
            }
        }
    } else if (rt) {
        out->imageView = rt->view;
        *sampled = rt;
        rt_scale(w, h, rt->w, rt->h, rt->aa_sx, rt->aa_sy, scale);
    } else {
        uint32_t texs = s_hz_texs;
        uint64_t t0 = hz_now();
        out->imageView = tex_get(va, color, w, h, pitch, 0, levels);
        if (s_hz_texs != texs)
            s_hz_tex_ns += hz_now() - t0;
    }
    addr = regs[base + 2];
    filter = regs[base + 5];
    key = (addr & 0xF0F);
    minf = (filter >> 16) & 0xFF;
    if (((filter >> 24) & 0xF) == 1) key |= 0x10000;
    if (minf == 1) key |= 0x20000;
    /* MIN 3..6: the *_MIPMAP_* filters. */
    if (levels > 1 && ((minf >= 3 && minf <= 6) || (dyn_cube && minf != 1))) {
        int an = nv2a_vk_aniso;
        key |= 0x40000;
        if (an > (int)s_aniso) an = (int)s_aniso;
        if (an > 1 && !(key & 0x30000))
            key |= (uint32_t)an << 20;          /* bits 20..24: anisotropy */
    }
    out->sampler = sampler_get(key);
}

/* ── Shaders and pipelines ────────────────────────────────────────── */

static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_layout;
static VkPipelineCache       s_pcache;

typedef struct {
    uint64_t   vkey;
    Nv2aPshKey pkey;
    VkShaderModule vs, fs;
    uint32_t   used;
    uint32_t   hash;
    int        next;
} VkProg;

#define VK_MAX_PROG 1024
#define PROG_BUCKETS 1024
static VkProg s_prog[VK_MAX_PROG];
static int s_prog_head[PROG_BUCKETS];
static uint32_t s_prog_made, s_prog_live;

uint32_t nv2a_gl_program_count(void) { return s_prog_live; }
/* The GL renderer's thread statistics; the Vulkan one records on the
 * executor's thread. */
void nv2a_gl_queue_stats(double secs, int *gl_idle_pct, int *exec_wait_pct)
{
    (void)secs;
    if (gl_idle_pct) *gl_idle_pct = 0;
    if (exec_wait_pct) *exec_wait_pct = 0;
}

typedef struct {
    glslang_stage_t stage;
    const char *src;
    uint32_t *words;
    size_t nwords;
    int ok;
} CompileJob;

static void *compile_thread(void *arg)
{
    CompileJob *j = (CompileJob *)arg;
    glslang_input_t in;
    glslang_shader_t *sh;
    glslang_program_t *pr;

    memset(&in, 0, sizeof in);
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = j->stage;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_1;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_3;
    in.code = j->src;
    in.default_version = 450;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = GLSLANG_MSG_DEFAULT_BIT;
    in.resource = glslang_default_resource();
    sh = glslang_shader_create(&in);
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in)) {
        LOGE("%s shader failed:\n%s\n", j->stage == GLSLANG_STAGE_VERTEX ? "vertex" : "fragment",
             glslang_shader_get_info_log(sh));
        if (s_trace)
            fprintf(stderr, "%s\n", j->src);
        glslang_shader_delete(sh);
        return NULL;
    }
    pr = glslang_program_create();
    glslang_program_add_shader(pr, sh);
    if (!glslang_program_link(pr, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        LOGE("link failed: %s\n", glslang_program_get_info_log(pr));
    } else {
        glslang_program_SPIRV_generate(pr, j->stage);
        j->nwords = glslang_program_SPIRV_get_size(pr);
        j->words = (uint32_t *)malloc(j->nwords * 4);
        if (j->words) {
            glslang_program_SPIRV_get(pr, j->words);
            j->ok = 1;
        }
    }
    glslang_program_delete(pr);
    glslang_shader_delete(sh);
    return NULL;
}

/* ── Caches across runs ───────────────────────────────────────────
 *
 * A program compiled at first use stalled a race start for ~1 s (glslang,
 * then NIR and NAK inside NVK: ~50 ms each on the console). Three files, next
 * to the NRO (the current directory on Linux); RECOMP_PROG_CACHE=0 disables
 * them all, =<path> moves the program list:
 *   progcache.bin    what each program was built from -- the GL renderer's
 *                    records, shared (the format does not depend on the back end)
 *   vkspirv.bin      glslang's output by hash of the GLSL source
 *   vkpipes.bin      the pipeline variants seen (program + fixed state)
 *   vkpipecache.bin  the driver's VkPipelineCache (NAK binaries), saved while
 *                    running whenever pipelines were added
 * At start-up every recorded program and pipeline is built before the title
 * draws. */
static int s_prewarming;
static int s_cache_off = -1;

static const char *cache_dir(void)
{
#if defined(__SWITCH__)
    return "sdmc:/switch/b3x/";
#else
    return "";
#endif
}

static int cache_path(char *out, size_t cap, const char *name)
{
    if (s_cache_off < 0) {
        const char *e = getenv("RECOMP_PROG_CACHE");
        s_cache_off = e && e[0] == '0' && !e[1];
    }
    if (s_cache_off)
        return 0;
    if (!strcmp(name, "progcache.bin")) {
        const char *e = getenv("RECOMP_PROG_CACHE");
        if (e && *e) {
            snprintf(out, cap, "%s", e);
            return 1;
        }
    }
    snprintf(out, cap, "%s%s", cache_dir(), name);
    return 1;
}

#define SPV_MAGIC 0x31565053u                   /* "SPV1" */
typedef struct { uint64_t hash; uint32_t stage, nwords; uint32_t *words; } SpvEnt;
static SpvEnt *s_spv;
static uint32_t s_nspv, s_spv_cap, s_spv_hits;

static uint64_t text_hash(const char *t, uint32_t stage)
{
    uint64_t h = 1469598103934665603ull ^ stage;
    while (*t)
        h = (h ^ (uint8_t)*t++) * 1099511628211ull;
    return h;
}

static void spv_add(uint64_t hash, uint32_t stage, const uint32_t *w, uint32_t n)
{
    if (s_nspv == s_spv_cap) {
        s_spv_cap = s_spv_cap ? s_spv_cap * 2 : 256;
        s_spv = (SpvEnt *)realloc(s_spv, s_spv_cap * sizeof *s_spv);
    }
    s_spv[s_nspv].hash = hash;
    s_spv[s_nspv].stage = stage;
    s_spv[s_nspv].nwords = n;
    s_spv[s_nspv].words = (uint32_t *)malloc((size_t)n * 4);
    memcpy(s_spv[s_nspv].words, w, (size_t)n * 4);
    s_nspv++;
}

static const SpvEnt *spv_find(uint64_t hash, uint32_t stage)
{
    uint32_t i;
    for (i = 0; i < s_nspv; i++)
        if (s_spv[i].hash == hash && s_spv[i].stage == stage)
            return &s_spv[i];
    return NULL;
}

static void spv_load(void)
{
    char path[256];
    FILE *f;
    uint32_t hdr[4];
    if (!cache_path(path, sizeof path, "vkspirv.bin") || !(f = fopen(path, "rb")))
        return;
    while (fread(hdr, sizeof hdr, 1, f) == 1) {
        uint64_t hash;
        uint32_t *w;
        if (hdr[0] != SPV_MAGIC || hdr[3] > (1u << 22))
            break;
        memcpy(&hash, &hdr[1], 8);
        w = (uint32_t *)malloc((size_t)hdr[3] * 8 + 8);
        if (!w || fread(w, 4, hdr[3], f) != hdr[3]) { free(w); break; }
        /* the stage is in the word after the header */
        spv_add(hash, w[0], w + 1, hdr[3] - 1);
        free(w);
    }
    fclose(f);
}

static void spv_append(uint64_t hash, uint32_t stage, const uint32_t *w, uint32_t n)
{
    char path[256];
    FILE *f;
    uint32_t hdr[4];
    if (!cache_path(path, sizeof path, "vkspirv.bin") || !(f = fopen(path, "ab")))
        return;
    hdr[0] = SPV_MAGIC;
    memcpy(&hdr[1], &hash, 8);
    hdr[3] = n + 1;
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(&stage, 4, 1, f);
    fwrite(w, 4, n, f);
    fclose(f);
}

#if !defined(__SWITCH__)
static int SDLCALL compile_thread_sdl(void *arg)
{
    compile_thread(arg);
    return 0;
}
#endif

/* GLSL to a shader module. glslang's parser recurses deeply: it runs on a
 * thread with a 4 MB stack (the executor's is smaller). */
static VkShaderModule compile(glslang_stage_t stage, const char *src)
{
    CompileJob j = { stage, src, NULL, 0, 0 };
#if defined(__SWITCH__)
    pthread_t t;
    pthread_attr_t at;
#else
    SDL_Thread *t;
    SDL_PropertiesID tp;
#endif
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;

    uint64_t hash = text_hash(src, (uint32_t)stage);
    const SpvEnt *hit = spv_find(hash, (uint32_t)stage);

    if (hit) {
        s_spv_hits++;
        ci.codeSize = (size_t)hit->nwords * 4;
        ci.pCode = hit->words;
        vkCreateShaderModule(s_dev, &ci, NULL, &m);
        return m;
    }
    VT("compile %s (%u bytes of GLSL)\n", stage == GLSLANG_STAGE_VERTEX ? "vs" : "fs",
       (unsigned)strlen(src));
#if defined(__SWITCH__)
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 4u << 20);
    if (pthread_create(&t, &at, compile_thread, &j) == 0)
        pthread_join(t, NULL);
    else
        compile_thread(&j);
    pthread_attr_destroy(&at);
#else
    tp = SDL_CreateProperties();
    SDL_SetPointerProperty(tp, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER,
                           (void *)compile_thread_sdl);
    SDL_SetStringProperty(tp, SDL_PROP_THREAD_CREATE_NAME_STRING, "glslang");
    SDL_SetPointerProperty(tp, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, &j);
    SDL_SetNumberProperty(tp, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 4u << 20);
    t = SDL_CreateThreadWithProperties(tp);
    SDL_DestroyProperties(tp);
    if (t)
        SDL_WaitThread(t, NULL);
    else
        compile_thread(&j);
#endif
    if (!j.ok)
        return VK_NULL_HANDLE;
    ci.codeSize = j.nwords * 4;
    ci.pCode = j.words;
    VT("  SPIR-V %u words -> vkCreateShaderModule\n", (unsigned)j.nwords);
    vkCreateShaderModule(s_dev, &ci, NULL, &m);
    spv_add(hash, (uint32_t)stage, j.words, (uint32_t)j.nwords);
    spv_append(hash, (uint32_t)stage, j.words, (uint32_t)j.nwords);
    free(j.words);
    return m;
}

/* glslang builds its built-in symbol tables (per stage) at the first compile
 * in a process: ~85 ms on an RTX 3050 Ti laptop, the program itself ~3 ms.
 * With a warm SPIR-V cache that first compile is the first new program
 * during play -- a ~90 ms frame (THPS 2X, M5 b E). So a tiny vertex and
 * fragment shader are compiled once at start-up on a thread of their own;
 * glslang guards the tables with its global lock, so a real compile in the
 * meantime only waits for them. RECOMP_GLSLANG_WARM=0: off. */
static void *glslang_warm(void *arg)
{
    static const char vs[] = "#version 450\nlayout(location = 0) in vec4 p;\n"
                             "void main() { gl_Position = p; }\n";
    static const char fs[] = "#version 450\nlayout(location = 0) out vec4 c;\n"
                             "void main() { c = vec4(1.0); }\n";
    CompileJob v = { GLSLANG_STAGE_VERTEX, vs, NULL, 0, 0 };
    CompileJob f = { GLSLANG_STAGE_FRAGMENT, fs, NULL, 0, 0 };
    (void)arg;
    compile_thread(&v);
    compile_thread(&f);
    free(v.words);
    free(f.words);
    return NULL;
}

#if !defined(__SWITCH__)
static int SDLCALL glslang_warm_sdl(void *arg)
{
    glslang_warm(arg);
    return 0;
}
#endif

static void glslang_warm_start(void)
{
    const char *e = getenv("RECOMP_GLSLANG_WARM");
#if defined(__SWITCH__)
    pthread_t t;
    pthread_attr_t at;
#else
    SDL_Thread *t;
    SDL_PropertiesID tp;
#endif
    if (e && e[0] == '0' && !e[1])
        return;
#if defined(__SWITCH__)
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 4u << 20);
    if (pthread_create(&t, &at, glslang_warm, NULL) == 0)
        pthread_detach(t);
    pthread_attr_destroy(&at);
#else
    tp = SDL_CreateProperties();
    SDL_SetPointerProperty(tp, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER,
                           (void *)glslang_warm_sdl);
    SDL_SetStringProperty(tp, SDL_PROP_THREAD_CREATE_NAME_STRING, "glslang-warm");
    SDL_SetNumberProperty(tp, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 4u << 20);
    t = SDL_CreateThreadWithProperties(tp);
    SDL_DestroyProperties(tp);
    if (t)
        SDL_DetachThread(t);
#endif
}

static uint64_t prog_hash(const Nv2aRawBatch *b)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t pc, k;
    for (pc = b->vp_start; pc < b->vp_slots; pc++) {
        for (k = 0; k < 4; k++)
            h = (h ^ b->vp_program[pc][k]) * 1099511628211ull;
        if (b->vp_program[pc][3] & 1)
            break;
    }
    return h ? h : 1;
}

static uint32_t prog_bucket(uint64_t vkey, const Nv2aPshKey *pk)
{
    const uint32_t *w = (const uint32_t *)pk;
    uint64_t h = vkey ^ 1469598103934665603ull;
    size_t i;
    for (i = 0; i < sizeof *pk / 4; i++)
        h = (h ^ w[i]) * 1099511628211ull;
    return (uint32_t)(h ^ (h >> 32)) & (PROG_BUCKETS - 1);
}

static void pipes_forget_prog(int prog);

/* progcache.bin records: nv2a_gl.c's ProgRec, byte for byte. */
#define PROG_REC_MAGIC 0x4350564Eu              /* "NVPC" */
typedef struct {
    uint32_t   magic, xform, vp_start, slots;
    Nv2aPshKey pk;
    uint32_t   prog[136][4];
} ProgRec;

static void prog_rec_fill(ProgRec *rec, const Nv2aRawBatch *b, const Nv2aPshKey *pk)
{
    memset(rec, 0, sizeof *rec);
    rec->magic = PROG_REC_MAGIC;
    rec->xform = b->xform == 2 ? 2 : 1;
    rec->pk = *pk;
    if (rec->xform == 2 && b->vp_program && b->vp_slots <= 136) {
        rec->vp_start = b->vp_start;
        rec->slots = b->vp_slots;
        memcpy(rec->prog, b->vp_program, (size_t)b->vp_slots * 16);
    }
}

static void prog_rec_append(const Nv2aRawBatch *b, const Nv2aPshKey *pk)
{
    char path[256];
    ProgRec rec;
    FILE *f;
    if (s_prewarming || !cache_path(path, sizeof path, "progcache.bin"))
        return;
    prog_rec_fill(&rec, b, pk);
    if ((f = fopen(path, "ab"))) {
        fwrite(&rec, sizeof rec, 1, f);
        fclose(f);
    }
}

/* The batch a record describes (vertex program slots in rec). */
static void prog_rec_batch(const ProgRec *rec, Nv2aRawBatch *bb, uint32_t n)
{
    memset(bb, 0, sizeof *bb);
    bb->xform = rec->xform;
    bb->vp_program = (const uint32_t (*)[4])rec->prog;
    bb->vp_slots = rec->slots ? rec->slots : 136;
    bb->vp_start = rec->vp_start;
    bb->vp_prog_gen = 0x80000000u + n;          /* hash it: not an executor version */
}

static VkProg *prog_get(const Nv2aRawBatch *b, const Nv2aPshKey *pk)
{
    static uint32_t last_gen;
    static uint64_t last_vkey;
    static VkProg *last;
    static char vsrc[320 * 1024], fsrc[64 * 1024];
    uint64_t vkey = 0;
    uint32_t bucket;
    VkProg *p, *victim = NULL;
    int i;
    size_t n;

    if (b->xform == 2) {
        if (b->vp_prog_gen != last_gen || !last_vkey) {
            last_vkey = prog_hash(b);
            last_gen = b->vp_prog_gen;
        }
        vkey = last_vkey;
    }
    if (last && last->vs && last->vkey == vkey && !memcmp(&last->pkey, pk, sizeof *pk)) {
        last->used = s_frame;
        return last;
    }
    bucket = prog_bucket(vkey, pk);
    for (i = s_prog_head[bucket]; i; i = s_prog[i - 1].next) {
        VkProg *c = &s_prog[i - 1];
        if (c->vs && c->vkey == vkey && !memcmp(&c->pkey, pk, sizeof *pk)) {
            c->used = s_frame;
            last = c;
            return c;
        }
    }
    for (i = 0; i < VK_MAX_PROG; i++) {
        VkProg *c = &s_prog[i];
        if (!c->vs) { if (!victim || victim->vs) victim = c; }
        else if (!victim || (victim->vs && c->used < victim->used)) victim = c;
    }
    p = victim;
    last = NULL;
    if (p->vs) {
        int *at = &s_prog_head[p->hash], me = (int)(p - s_prog) + 1;
        while (*at) {
            if (*at == me) { *at = p->next; break; }
            at = &s_prog[*at - 1].next;
        }
        pipes_forget_prog((int)(p - s_prog));
        vkDestroyShaderModule(s_dev, p->vs, NULL);
        vkDestroyShaderModule(s_dev, p->fs, NULL);
        s_prog_live--;
    }
    memset(p, 0, sizeof *p);

    n = (size_t)snprintf(vsrc, sizeof vsrc, "%s", nv2a_gl_vsh_prelude());
    if (vkey) {
        int k = nv2a_gl_vsh_program(b->vp_program, b->vp_slots, b->vp_start, vsrc + n, sizeof vsrc - n);
        if (k < 0)
            return NULL;
        n += (size_t)k;
        snprintf(vsrc + n, sizeof vsrc - n, "%s", nv2a_gl_vsh_main_program());
    } else {
        snprintf(vsrc + n, sizeof vsrc - n, "%s", nv2a_gl_vsh_fixed());
    }
    if (nv2a_gl_psh(pk, fsrc, sizeof fsrc) < 0)
        return NULL;
    p->vs = compile(GLSLANG_STAGE_VERTEX, vsrc);
    p->fs = compile(GLSLANG_STAGE_FRAGMENT, fsrc);
    if (!p->vs || !p->fs) {
        if (p->vs) vkDestroyShaderModule(s_dev, p->vs, NULL);
        if (p->fs) vkDestroyShaderModule(s_dev, p->fs, NULL);
        memset(p, 0, sizeof *p);
        return NULL;
    }
    s_prog_live++;
    if ((++s_prog_made % 100) == 0)
        LOGE("%u shader programs compiled (%u live)\n", s_prog_made, s_prog_live);
    p->vkey = vkey;
    p->pkey = *pk;
    p->used = s_frame;
    p->hash = bucket;
    p->next = s_prog_head[bucket];
    s_prog_head[bucket] = (int)(p - s_prog) + 1;
    last = p;
    prog_rec_append(b, pk);
    return p;
}

typedef struct {
    int      prog;
    uint32_t blend, bsrc, bdst, beq, cmask, depth, topo;
    uint32_t samples;                       /* MSAA sample count of the target, 0 = 1 */
    uint8_t  vfmt[NV2A_RAW_ATTRS];          /* s_baked_vi only: VkFormat per attribute */
} PipeKey;

/* The part of PipeKey that is hashed and compared: without s_baked_vi the
 * formats are dynamic state and stay out of the key. */
#define PIPE_KEY_BYTES (s_baked_vi ? sizeof(PipeKey) : offsetof(PipeKey, vfmt))

typedef struct {
    PipeKey key;
    VkPipeline pipe;
    uint32_t hash;
    int next;
} PipeEnt;

#define VK_MAX_PIPE 4096
#define PIPE_BUCKETS 4096
static PipeEnt s_pipes[VK_MAX_PIPE];
static int s_pipe_head[PIPE_BUCKETS];
static int s_npipes;

static void pipes_forget_prog(int prog)
{
    int i;
    /* Rare (1024 live programs): drop every pipeline using it. */
    for (i = 0; i < s_npipes; i++) {
        PipeEnt *e = &s_pipes[i];
        if (e->pipe && e->key.prog == prog) {
            int *at = &s_pipe_head[e->hash], me = i + 1;
            while (*at) {
                if (*at == me) { *at = e->next; break; }
                at = &s_pipes[*at - 1].next;
            }
            garbage_add_pipe(e->pipe);               /* may still be in flight */
            e->pipe = VK_NULL_HANDLE;
        }
    }
}

static VkBlendFactor blend_factor(uint32_t v)
{
    switch (v) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 0x300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(uint32_t v)
{
    switch (v) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

/* vkpipes.bin: the program by what generated it, and the pipeline's own
 * state. */
#define PIPE_REC_MAGIC 0x4C504B56u              /* "VKPL" */
typedef struct {
    uint32_t   magic, pad;
    uint64_t   vkey;
    Nv2aPshKey pk;
    uint32_t   blend, bsrc, bdst, beq, cmask, depth, topo;
} PipeRec;
/* s_baked_vi: the same plus the vertex formats, in vkpipes_vi.bin. */
typedef struct {
    PipeRec r;
    uint8_t vfmt[NV2A_RAW_ATTRS];
} PipeRecVi;
#define PIPE_REC_FILE  (s_baked_vi ? "vkpipes_vi.bin" : "vkpipes.bin")
#define PIPE_REC_BYTES (s_baked_vi ? sizeof(PipeRecVi) : sizeof(PipeRec))
static uint32_t s_pipes_unsaved;

static void pipe_rec_append(const PipeKey *k)
{
    char path[256];
    PipeRecVi rec;
    FILE *f;
    if (s_prewarming || !cache_path(path, sizeof path, PIPE_REC_FILE))
        return;
    memset(&rec, 0, sizeof rec);
    rec.r.magic = PIPE_REC_MAGIC;
    rec.r.vkey = s_prog[k->prog].vkey;
    rec.r.pk = s_prog[k->prog].pkey;
    rec.r.blend = k->blend; rec.r.bsrc = k->bsrc; rec.r.bdst = k->bdst; rec.r.beq = k->beq;
    rec.r.cmask = k->cmask; rec.r.depth = k->depth; rec.r.topo = k->topo;
    memcpy(rec.vfmt, k->vfmt, sizeof rec.vfmt);
    if ((f = fopen(path, "ab"))) {
        fwrite(&rec, PIPE_REC_BYTES, 1, f);
        fclose(f);
    }
}

static VkPipeline pipe_get(const PipeKey *k)
{
    const uint32_t *w = (const uint32_t *)k;
    uint32_t h = 2166136261u, i;
    int e;
    PipeEnt *pe;
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    VkFormat cfmt = VK_FORMAT_B8G8R8A8_UNORM;
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT,
        VK_DYNAMIC_STATE_DEPTH_BIAS,
    };
    /* s_baked_vi: the same, with the strides dynamic instead of the whole
     * vertex input. */
    static const VkDynamicState dyn_baked[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE,
        VK_DYNAMIC_STATE_DEPTH_BIAS,
    };
    VkVertexInputBindingDescription vb[NV2A_RAW_ATTRS];
    VkVertexInputAttributeDescription va[NV2A_RAW_ATTRS];
    VkPipeline pipe = VK_NULL_HANDLE;
    VkProg *p = &s_prog[k->prog];
    size_t kb = PIPE_KEY_BYTES;

    for (i = 0; i < kb / 4; i++)
        h = (h ^ w[i]) * 16777619u;
    h &= PIPE_BUCKETS - 1;
    for (e = s_pipe_head[h]; e; e = s_pipes[e - 1].next)
        if (s_pipes[e - 1].pipe && !memcmp(&s_pipes[e - 1].key, k, kb))
            return s_pipes[e - 1].pipe;

    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = p->vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = p->fs;
    st[1].pName = "main";
    ia.topology = k->topo == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                : k->topo == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                               : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    rs.depthClampEnable = s_have_depth_clamp ? VK_TRUE : VK_FALSE;
    /* Polygon offset: on in every pipeline, the values set per draw
     * (vkCmdSetDepthBias, apply_dynamic) and 0/0 when the title has it off --
     * no pipeline variants, Vulkan 1.0 core, nothing in the shader. */
    rs.depthBiasEnable = VK_TRUE;
    ms.rasterizationSamples = k->samples ? (VkSampleCountFlagBits)k->samples : VK_SAMPLE_COUNT_1_BIT;
    memset(&ba, 0, sizeof ba);
    ba.blendEnable = k->blend ? VK_TRUE : VK_FALSE;
    ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = blend_factor(k->bsrc);
    ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = blend_factor(k->bdst);
    ba.colorBlendOp = ba.alphaBlendOp = blend_op(k->beq);
    ba.colorWriteMask = k->cmask;
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    dy.dynamicStateCount = (uint32_t)(sizeof dyn / sizeof dyn[0]);
    dy.pDynamicStates = dyn;
    if (s_baked_vi) {
        for (i = 0; i < NV2A_RAW_ATTRS; i++) {
            vb[i].binding = i;
            vb[i].stride = 0;                     /* dynamic: upload_vertices */
            vb[i].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            va[i].location = i;
            va[i].binding = i;
            va[i].format = (VkFormat)k->vfmt[i];
            va[i].offset = 0;
        }
        vi.vertexBindingDescriptionCount = NV2A_RAW_ATTRS;
        vi.pVertexBindingDescriptions = vb;
        vi.vertexAttributeDescriptionCount = NV2A_RAW_ATTRS;
        vi.pVertexAttributeDescriptions = va;
        dy.dynamicStateCount = (uint32_t)(sizeof dyn_baked / sizeof dyn_baked[0]);
        dy.pDynamicStates = dyn_baked;
    }
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &cfmt;
    if (k->depth) {
        rci.depthAttachmentFormat = s_depth_fmt;
        rci.stencilAttachmentFormat = s_depth_fmt;
    }
    ci.pNext = &rci;
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = s_layout;
    VT("vkCreateGraphicsPipelines prog %d blend %u cmask %X depth %u topo %u\n", k->prog, k->blend,
       k->cmask, k->depth, k->topo);
    if (vkCreateGraphicsPipelines(s_dev, s_pcache, 1, &ci, NULL, &pipe) != VK_SUCCESS) {
        LOGE("pipeline creation failed\n");
        return VK_NULL_HANDLE;
    }
    if (s_npipes == VK_MAX_PIPE) {
        LOGE("pipeline table full\n");
        return pipe;                          /* leaks; never seen in practice */
    }
    pe = &s_pipes[s_npipes];
    pe->key = *k;
    pe->pipe = pipe;
    pe->hash = h;
    pe->next = s_pipe_head[h];
    s_pipe_head[h] = ++s_npipes;
    s_pipes_unsaved++;
    pipe_rec_append(k);
    return pipe;
}

static VkProg *prog_find(uint64_t vkey, const Nv2aPshKey *pk)
{
    int i;
    for (i = s_prog_head[prog_bucket(vkey, pk)]; i; i = s_prog[i - 1].next) {
        VkProg *c = &s_prog[i - 1];
        if (c->vs && c->vkey == vkey && !memcmp(&c->pkey, pk, sizeof *pk))
            return c;
    }
    return NULL;
}

static void *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    void *d = NULL;
    long n;
    *size = 0;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        d = malloc((size_t)n);
        if (d && fread(d, 1, (size_t)n, f) == (size_t)n)
            *size = (size_t)n;
        else { free(d); d = NULL; }
    }
    fclose(f);
    return d;
}

static void pcache_save(void)
{
    char path[256], tmp[272];
    size_t n = 0;
    void *d;
    FILE *f;
    if (!s_pcache || !cache_path(path, sizeof path, "vkpipecache.bin"))
        return;
    if (vkGetPipelineCacheData(s_dev, s_pcache, &n, NULL) != VK_SUCCESS || !n)
        return;
    d = malloc(n);
    if (!d || vkGetPipelineCacheData(s_dev, s_pcache, &n, d) != VK_SUCCESS) { free(d); return; }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if ((f = fopen(tmp, "wb"))) {
        int ok = fwrite(d, 1, n, f) == n;
        fclose(f);
        if (ok) {
            remove(path);                  /* FAT: rename does not replace */
            rename(tmp, path);
        }
    }
    free(d);
    s_pipes_unsaved = 0;
}

static void prewarm(void)
{
    char path[256];
    FILE *f;
    uint64_t t0 = hz_now();
    uint32_t nprog = 0, npipe = 0, bad = 0;
    ProgRec rec;
    PipeRecVi pr;

    spv_load();
    s_prewarming = 1;
    if (cache_path(path, sizeof path, "progcache.bin") && (f = fopen(path, "rb"))) {
        while (fread(&rec, sizeof rec, 1, f) == 1) {
            Nv2aRawBatch bb;
            if (rec.magic != PROG_REC_MAGIC || rec.slots > 136) { bad++; break; }
            prog_rec_batch(&rec, &bb, nprog);
            if (prog_get(&bb, &rec.pk))
                nprog++;
        }
        fclose(f);
    }
    if (cache_path(path, sizeof path, PIPE_REC_FILE) && (f = fopen(path, "rb"))) {
        memset(&pr, 0, sizeof pr);
        while (fread(&pr, PIPE_REC_BYTES, 1, f) == 1) {
            VkProg *p;
            PipeKey k;
            if (pr.r.magic != PIPE_REC_MAGIC) { bad++; break; }
            if (!(p = prog_find(pr.r.vkey, &pr.r.pk)))
                continue;
            memset(&k, 0, sizeof k);
            k.prog = (int)(p - s_prog);
            k.blend = pr.r.blend; k.bsrc = pr.r.bsrc; k.bdst = pr.r.bdst; k.beq = pr.r.beq;
            k.cmask = pr.r.cmask; k.depth = pr.r.depth; k.topo = pr.r.topo;
            memcpy(k.vfmt, pr.vfmt, sizeof k.vfmt);
            if (pipe_get(&k))
                npipe++;
        }
        fclose(f);
    }
    s_prewarming = 0;
    fprintf(stderr, "  [VK] prewarm: %u programs (%u SPIR-V from cache), %u pipelines in %.0f ms%s\n",
            nprog, s_spv_hits, npipe, (hz_now() - t0) / 1e6, bad ? " (a file ends in a bad record)" : "");
    if (s_pipes_unsaved)
        pcache_save();
}

/* ── Context ──────────────────────────────────────────────────────── */

/* 1 while the swapchain presents with FIFO (or none exists yet). */
static volatile int s_present_fifo = 1;

int nv2a_vk_present_fifo(void)
{
    return s_present_fifo;
}

static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR fmts[16];
    VkPresentModeKHR modes[8], mode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t nf = 16, nm = 8, i;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };

    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_pd, s_surface, &caps) != VK_SUCCESS)
        return 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s_pd, s_surface, &nf, fmts);
    vkGetPhysicalDeviceSurfacePresentModesKHR(s_pd, s_surface, &nm, modes);
    s_sc_format = fmts[0].format;
    for (i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            s_sc_format = fmts[i].format;
            ci.imageColorSpace = fmts[i].colorSpace;
            break;
        }
    /* V-Sync on by default (design principle A10): FIFO, which every device
     * has. burnout3-sw chose immediate/mailbox because Burnout 3 paces itself
     * (vblank emulation); THPS 2X's menus do not and presented ~460 frames a
     * second (frame-time counter, M3.5), burning CPU and GPU for nothing.
     * RECOMP_VSYNC=0 brings back the unblocked modes (benchmarks; Horizon's
     * immediate mode does not tear, nvnflinger composes): immediate, else
     * mailbox. RECOMP_PRESENT=fifo|mailbox|immediate picks one mode; a mode
     * the device lacks falls back to FIFO.
     * Default MAILBOX when the device offers it, FIFO otherwise (THPS 2X, M5:
     * FIFO adds a second 60 Hz pacer -- the display -- next to the guest
     * vblank; their slow phase drift gave 2000-5000 frames > 25 ms per
     * 10 min, mailbox 0). Mailbox does not tear and the guest vblank still
     * paces the title, so V-Sync stays on (A10). */
    {
        const char *e = getenv("RECOMP_VSYNC");
        const char *pm = getenv("RECOMP_PRESENT");
        const char *why = "default";
        VkPresentModeKHR want = VK_PRESENT_MODE_MAILBOX_KHR;
        if (pm && !strcmp(pm, "fifo"))
            want = VK_PRESENT_MODE_FIFO_KHR, why = "RECOMP_PRESENT";
        else if (pm && !strcmp(pm, "mailbox"))
            why = "RECOMP_PRESENT";
        else if (pm && !strcmp(pm, "immediate"))
            want = VK_PRESENT_MODE_IMMEDIATE_KHR, why = "RECOMP_PRESENT";
        if (!pm && e && *e == '0') {
            why = "RECOMP_VSYNC=0";
            for (i = 0; i < nm; i++)
                if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = modes[i];
            for (i = 0; i < nm && mode == VK_PRESENT_MODE_FIFO_KHR; i++)
                if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
        } else if (want != VK_PRESENT_MODE_FIFO_KHR) {
            for (i = 0; i < nm; i++)
                if (modes[i] == want) mode = want;
            if (mode == VK_PRESENT_MODE_FIFO_KHR)
                why = "fallback: the device has no such mode";
        }
        s_present_fifo = mode == VK_PRESENT_MODE_FIFO_KHR;
        LOGE("present mode: %s (%s)\n", mode == VK_PRESENT_MODE_FIFO_KHR ? "fifo" :
             mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" :
             mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "other", why);
    }
    s_sc_extent = caps.currentExtent;
    if (s_sc_extent.width == 0xFFFFFFFFu) {
        s_sc_extent.width = xbox_video_widescreen() ? 1280 : 960;   /* the window's aspect (SDL_CreateWindow) */
        s_sc_extent.height = 720;
    }
    ci.surface = s_surface;
    ci.minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount)
        ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = s_sc_format;
    ci.imageExtent = s_sc_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = s_swapchain;
    {
        VkSwapchainKHR sc;
        if (vkCreateSwapchainKHR(s_dev, &ci, NULL, &sc) != VK_SUCCESS)
            return 0;
        if (s_swapchain)
            vkDestroySwapchainKHR(s_dev, s_swapchain, NULL);
        s_swapchain = sc;
    }
    for (i = 0; i < 8; i++)
        if (s_sc_views[i]) {
            vkDestroyImageView(s_dev, s_sc_views[i], NULL);
            s_sc_views[i] = VK_NULL_HANDLE;
        }
    s_sc_count = 8;
    vkGetSwapchainImagesKHR(s_dev, s_swapchain, &s_sc_count, s_sc_images);
    for (i = 0; i < s_sc_count; i++) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = s_sc_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = s_sc_format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(s_dev, &vi, NULL, &s_sc_views[i]) != VK_SUCCESS)
            s_sc_views[i] = VK_NULL_HANDLE;     /* that image falls back to the blit */
    }
    fprintf(stderr, "  [VK] swapchain %ux%u, %u images, format %d, present mode %d\n",
            s_sc_extent.width, s_sc_extent.height, s_sc_count, s_sc_format, mode);
    return 1;
}

/* The renderer cannot start: never go on headless (THPS 2X player report,
 * 2026-10-29: an RX 580 whose Windows driver lacks dynamic vertex input ran the
 * game on without a picture and without a word). The reason goes to the log,
 * line by line, and into a message box (not with RECOMP_NO_DIALOGS=1, the
 * automated tests); then the port's fatal handler ends the process, else
 * exit(1). Only a headless start asked for (RECOMP_VK_HEADLESS=1,
 * SDL_VIDEODRIVER=offscreen) keeps running without a window. */
static void vk_fatal(const char *why)
{
    char msg[2048];
    const char *p = why, *nl;
    const char *nd = getenv("RECOMP_NO_DIALOGS");

#if !defined(__SWITCH__)
    {
        /* The device check thread and ready() may both fail: one message. */
        static SDL_AtomicInt once;
        if (!SDL_CompareAndSwapAtomicInt(&once, 0, 1))
            for (;;)
                SDL_Delay(1000);        /* the first caller ends the process */
    }
#endif
    LOGE("FATAL: the renderer cannot start\n");
    while (*p) {
        nl = strchr(p, '\n');
        if (!nl)
            nl = p + strlen(p);
        if (nl > p)
            fprintf(stderr, "  [VK]   %.*s\n", (int)(nl - p), p);
        p = *nl ? nl + 1 : nl;
    }
    fflush(stderr);
#if !defined(__SWITCH__)
    if (s_win && s_win_thread == SDL_GetCurrentThreadID()) {   /* not a black window behind the box */
        SDL_DestroyWindow(s_win);
        s_win = NULL;
    }
    if (!(nd && *nd == '1')) {
        int wn = (int)strlen(why);
        while (wn > 0 && why[wn - 1] == '\n')
            wn--;
        snprintf(msg, sizeof msg, "The game cannot start: the graphics card or its driver does not offer what "
                 "the Vulkan renderer needs.\n\n%.*s%s%s", wn, why, s_fatal_help ? "\n\n" : "",
                 s_fatal_help ? s_fatal_help : "");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, s_fatal_title ? s_fatal_title : "Vulkan renderer",
                                 msg, NULL);
    }
#else
    (void)msg; (void)nd;
#endif
    if (s_fatal_handler)
        s_fatal_handler();
    exit(1);
}

/* What the renderer needs from a device: Vulkan 1.3 with dynamic rendering,
 * push descriptors, a graphics queue, and the swapchain when it has a window.
 * Dynamic vertex input is optional (vk_dyn_vertex_input; without it the
 * formats are baked into the pipelines). RECOMP_VK_TEST_MISSING=<extension
 * name> (tests only) treats that extension as absent, to check this path on
 * any GPU.
 * Appends "<device> (driver: Vulkan x.y.z)" and its missing items to why
 * (when given); returns 1 when nothing is missing. */
static int vk_device_ok(VkPhysicalDevice pd, int need_swapchain, char *why, size_t nwhy)
{
    static const struct { const char *ext, *what; } need[] = {
        { VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, "push descriptors" },
        { VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, "dynamic vertex input" },
        { VK_KHR_SWAPCHAIN_EXTENSION_NAME, "presenting to a window" },
    };
    static VkExtensionProperties ep[512];
    const char *fake = getenv("RECOMP_VK_TEST_MISSING");
    VkPhysicalDeviceProperties pp;
    VkQueueFamilyProperties qf[16];
    char miss[512];
    size_t nm = 0;
    uint32_t ne = 512, nq = 16, i, k;
    int has[3] = { 0, 0, 0 }, gfx = 0;

    miss[0] = 0;
#define MISS(...) (nm += (size_t)snprintf(miss + nm, nm < sizeof miss ? sizeof miss - nm : 0, __VA_ARGS__), \
                   nm = nm < sizeof miss ? nm : sizeof miss - 1)
    vkGetPhysicalDeviceProperties(pd, &pp);
    if (pp.apiVersion < VK_API_VERSION_1_3)
        MISS("  - Vulkan 1.3 (the driver offers %u.%u)\n", VK_API_VERSION_MAJOR(pp.apiVersion),
             VK_API_VERSION_MINOR(pp.apiVersion));
    if (vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep) < 0)
        ne = 0;
    for (i = 0; i < ne; i++)
        for (k = 0; k < 3; k++)
            if (!strcmp(ep[i].extensionName, need[k].ext) && !(fake && !strcmp(fake, need[k].ext)))
                has[k] = 1;
    for (k = 0; k < 3; k++)
        if (!has[k] && (k == 0 || (k == 2 && need_swapchain)))
            MISS("  - %s (%s)\n", need[k].ext, need[k].what);
    if (pp.apiVersion >= VK_API_VERSION_1_3) {
        VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        f2.pNext = &f13;
        vkGetPhysicalDeviceFeatures2(pd, &f2);
        if (!f13.dynamicRendering)
            MISS("  - dynamic rendering (Vulkan 1.3 feature)\n");
    }
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    for (i = 0; i < nq; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            gfx = 1;
    if (!gfx)
        MISS("  - a graphics queue\n");
#undef MISS
    if (why) {
        size_t n = strlen(why);
        snprintf(why + n, n < nwhy ? nwhy - n : 0, "%s%s (driver: Vulkan %u.%u.%u)\n%s%s", n ? "\n" : "",
                 pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion),
                 VK_API_VERSION_PATCH(pp.apiVersion), nm ? "missing:\n" : "", miss);
    }
    return nm == 0;
}

/* 1 when the device offers VK_EXT_vertex_input_dynamic_state with its
 * feature (RECOMP_VK_TEST_MISSING may name it). */
static int vk_dyn_vertex_input(VkPhysicalDevice pd)
{
    static VkExtensionProperties ep[512];
    const char *fake = getenv("RECOMP_VK_TEST_MISSING");
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fvi =
        { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT };
    uint32_t ne = 512, i;
    int has = 0;

    if (fake && !strcmp(fake, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME))
        return 0;
    if (vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep) < 0)
        ne = 0;
    for (i = 0; i < ne && !has; i++)
        has = !strcmp(ep[i].extensionName, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    if (!has)
        return 0;
    f2.pNext = &fvi;
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    return fvi.vertexInputDynamicState == VK_TRUE;
}

/* The first device (in the driver's order, as before) with everything the
 * renderer needs; 0 when there is none, why then names every device and what
 * it lacks. */
static int vk_pick_device(VkInstance inst, int need_swapchain, VkPhysicalDevice *out, char *why, size_t nwhy)
{
    VkPhysicalDevice pds[8];
    uint32_t n = 8, i;

    why[0] = 0;
    if (vkEnumeratePhysicalDevices(inst, &n, pds) < 0 || !n) {
        snprintf(why, nwhy, "No Vulkan device was found - is the graphics driver installed?");
        return 0;
    }
    for (i = 0; i < n; i++)
        if (vk_device_ok(pds[i], need_swapchain, NULL, 0)) {
            *out = pds[i];
            return 1;
        }
    for (i = 0; i < n; i++)
        vk_device_ok(pds[i], need_swapchain, why, nwhy);
    return 0;
}

/* nv2a_vk_install, before the title starts: the device check on a throw-away
 * instance, so a GPU without what the renderer needs stops the start at once
 * -- not later on the NV2A thread, with the title's movies already playing
 * sound behind the message. ready() checks again on its own instance. It
 * runs on its own thread (vk_precheck_thread): ~110 ms on the RTX 3050 Ti,
 * mostly loading the driver, that the start does not wait for. */
static void vk_precheck(void)
{
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkInstance inst;
    VkPhysicalDevice pd;
    static char why[1536];
    const char *e = getenv("RECOMP_VK_HEADLESS");
    const char *drv = getenv("SDL_VIDEODRIVER");

#if !defined(__SWITCH__)
    uint64_t t0 = SDL_GetTicksNS();
#endif

    if ((e && *e == '1') || (drv && !strcmp(drv, "offscreen")))
        return;                         /* a headless test run: ready() decides */
    ai.pApplicationName = "nfsu2x";
    ai.pEngineName = "xboxrecomp";
    ai.apiVersion = VK_API_VERSION_1_3;
    ici.pApplicationInfo = &ai;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS)
        vk_fatal("Vulkan could not be started (vkCreateInstance failed) - is a Vulkan driver installed?");
    if (!vk_pick_device(inst, 1, &pd, why, sizeof why)) {
        vkDestroyInstance(inst, NULL);
        vk_fatal(why);
    }
    vkDestroyInstance(inst, NULL);
#if !defined(__SWITCH__)
    fprintf(stderr, "  [VK] device check passed in %.0f ms\n", (double)(SDL_GetTicksNS() - t0) / 1e6);
#endif
}

#if !defined(__SWITCH__)
static int SDLCALL vk_precheck_thread(void *arg)
{
    (void)arg;
    vk_precheck();
    return 0;
}
#endif

static int ready(void)
{
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    const char *iext[8];
    uint32_t niext = 0, n, i;
    VkPhysicalDeviceProperties pp;
    VkQueueFamilyProperties qf[8];
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fvi =
        { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT };
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT feds =
        { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT };
    VkPhysicalDeviceFeatures have;
    const char *dext[5];
    uint32_t ndext = 0;
    const char *layers[1];
    uint32_t nlayers = 0;

    if (s_state)
        return s_state > 0;
    s_state = -1;
    s_trace = getenv("RECOMP_GL_TRACE") != NULL;
    {
        const char *e = getenv("RECOMP_VK_TRACE");
        s_vtrace = e ? atoi(e) : 0;
    }
    nv2a_shader_vk = 1;
    glslang_initialize_process();
    glslang_warm_start();

    iext[niext++] = VK_KHR_SURFACE_EXTENSION_NAME;
#if defined(__SWITCH__)
    iext[niext++] = VK_NN_VI_SURFACE_EXTENSION_NAME;
#else
    {
        const char *e = getenv("RECOMP_VK_HEADLESS");
        const char *drv = getenv("SDL_VIDEODRIVER");
        s_headless = (e && *e == '1') || (drv && !strcmp(drv, "offscreen"));
        if (!s_headless) {
            if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
                char w[512];
                snprintf(w, sizeof w, "The window system could not be started (SDL: %s).", SDL_GetError());
                vk_fatal(w);
            } else {
                /* Host pads (src/input): SDL wants them initialised and
                 * pumped on the thread that owns the window -- vk_flip polls
                 * the events. No pads is not an error. */
                if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
                    LOGE("SDL gamepads unavailable: %s\n", SDL_GetError());
                /* The window has the title's aspect ratio (THPS 2X
                 * maintainer, 2026-10-26; every port): 4:3 -> 960x720,
                 * 16:9 (xbox_video_widescreen) -> 1280x720, and keeps it
                 * when resized, so no bars are added around the picture. A
                 * rectangle the port saved (nv2a_vk_set_window_rect) keeps
                 * its height and position, the width follows the aspect. */
                float aspect = xbox_video_widescreen() ? 16.0f / 9.0f : 4.0f / 3.0f;
                int wh = s_wr_have ? s_wr_want[3] : 720;
                int ww = (int)(wh * aspect + 0.5f);
                s_win = SDL_CreateWindow("NV2A (Vulkan)", ww, wh,
                                         SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
                s_win_thread = SDL_GetCurrentThreadID();
                if (!s_win) {
                    char w[512];
                    snprintf(w, sizeof w, "The Vulkan window could not be opened (SDL: %s).", SDL_GetError());
                    vk_fatal(w);
                } else {
                    int r[4] = { s_wr_want[0], s_wr_want[1], ww, wh };
                    if (!SDL_SetWindowAspectRatio(s_win, aspect, aspect))
                        LOGE("SDL_SetWindowAspectRatio: %s\n", SDL_GetError());
                    {
                        /* the port's icon for the window / taskbar (kernel.h xbox_set_window_icon) */
                        extern const void *xbox_window_icon(size_t *n);
                        size_t in = 0;
                        const void *ib = xbox_window_icon(&in);
                        SDL_Surface *is = ib ? SDL_LoadBMP_IO(SDL_IOFromConstMem(ib, in), true) : NULL;
                        if (is) {
                            if (!SDL_SetWindowIcon(s_win, is))
                                LOGE("SDL_SetWindowIcon: %s\n", SDL_GetError());
                            SDL_DestroySurface(is);
                        } else if (ib) {
                            LOGE("window icon: not a BMP SDL reads (%s)\n", SDL_GetError());
                        }
                    }
                    if (s_wr_have && wr_on_screen(r))
                        SDL_SetWindowPosition(s_win, r[0], r[1]);
                    else if (s_wr_have)
                        LOGE("saved window position %d,%d is on no display -- centred\n", r[0], r[1]);
                    dm_apply();
                    wr_record();
                    Uint32 ne = 0;
                    const char *const *names = SDL_Vulkan_GetInstanceExtensions(&ne);
                    display_hz_update();
                    if (!names) ne = 0;
                    if (ne > 6) ne = 6;
                    niext = 0;
                    for (i = 0; i < ne; i++)
                        iext[niext++] = names[i];
                }
            }
        }
        if (s_headless)
            niext = 0;
    }
    {
        const char *v = getenv("RECOMP_VK_VALIDATION");
        if (v && *v == '1')
            layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
    }
#endif
    ai.pApplicationName = "nfsu2x";
    ai.pEngineName = "xboxrecomp";
    ai.apiVersion = VK_API_VERSION_1_3;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = niext;
    ici.ppEnabledExtensionNames = iext;
    ici.enabledLayerCount = nlayers;
    ici.ppEnabledLayerNames = layers;
    if (vkCreateInstance(&ici, NULL, &s_inst) != VK_SUCCESS) {
        LOGE("vkCreateInstance failed\n");
        if (s_headless)
            return 0;
        vk_fatal("Vulkan could not be started (vkCreateInstance failed) - is a Vulkan driver installed?");
    }
    {
        static char why[1536];
        if (!vk_pick_device(s_inst, !s_headless, &s_pd, why, sizeof why)) {
            if (s_headless) {
                LOGE("no Vulkan device the renderer can use\n");
                return 0;
            }
            vk_fatal(why);
        }
    }
    vkGetPhysicalDeviceProperties(s_pd, &pp);
    fprintf(stderr, "  [VK] %s, Vulkan %u.%u.%u\n", pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion),
            VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion));
    {
        const char *e = getenv("RECOMP_VK_NO_DYN_VERTEX_INPUT");
        int dyn = vk_dyn_vertex_input(s_pd);
        s_baked_vi = !dyn || (e && *e == '1');
        if (s_baked_vi)
            fprintf(stderr, "  [VK] vertex input baked into the pipelines (%s)\n",
                    dyn ? "RECOMP_VK_NO_DYN_VERTEX_INPUT=1" : "no " VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    }
    s_ubo_align = pp.limits.minUniformBufferOffsetAlignment;
    s_ubo_range = pp.limits.maxUniformBufferRange;
    {
        const char *e = getenv("RECOMP_VK_FFP_LIGHT");
        s_ffp_light = !(e && *e == '0');
        e = getenv("RECOMP_VK_FFP_MARK");
        s_ffp_mark = e && *e == '1';
        e = getenv("RECOMP_VK_TEXGEN");
        s_ffp_texgen = !e ? 1 : *e == 't' ? 2 : *e != '0';
        if (s_ffp_texgen == 2 && atoi(e + 1) > 0)
            s_ffp_texgen_period = (uint32_t)atoi(e + 1);
        e = getenv("RECOMP_GL_SCALE");
        double k = e ? strtod(e, NULL) : 1.0;
        uint32_t m = pp.limits.maxImageDimension2D;
        if (pp.limits.maxFramebufferWidth < m) m = pp.limits.maxFramebufferWidth;
        if (pp.limits.maxFramebufferHeight < m) m = pp.limits.maxFramebufferHeight;
        s_max_size = m ? m : 4096;
        s_ms_max = pp.limits.framebufferColorSampleCounts & pp.limits.framebufferDepthSampleCounts
                 & (VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT);
        s_scale_env = e && k > 0.0;                 /* unset: the port's option decides */
        if (!(k > 0.0)) k = 1.0;                    /* unset, 0 or garbage */
        s_scale = k < 0.5 ? 0.5 : k > VK_SCALE_MAX ? VK_SCALE_MAX : k;
        e = getenv("RECOMP_VK_SQUARE");
        s_square_env = xbox_video_widescreen() && !(e && *e == '0');
        e = getenv("RECOMP_GL_SCALE_X");
        k = e ? strtod(e, NULL) : 0.0;
        if (k > 0.0)
            s_square_env = 0, s_scale_env = 1;
        s_square = s_square_env && nv2a_vk_square;
        if (k > 0.0)
            s_xratio = (k < 0.5 ? 0.5 : k > VK_SCALE_MAX * 4.0 / 3.0 ? VK_SCALE_MAX * 4.0 / 3.0 : k) / s_scale;
        s_xratio_env = s_xratio;
        s_scale_x = s_scale * s_xratio;
        if (s_scale != 1.0 || s_scale_x != 1.0 || s_square)
            fprintf(stderr, "  [VK] rendering at %g x %g (RECOMP_GL_SCALE[_X])%s, surfaces up to %u\n",
                    s_scale_x, s_scale, s_square ? ", square pixels" : "", s_max_size);
    }
    if (s_ubo_align < 16) s_ubo_align = 16;
    vkGetPhysicalDeviceMemoryProperties(s_pd, &s_memprops);
    vkGetPhysicalDeviceFeatures(s_pd, &have);
    s_have_bc = have.textureCompressionBC;
    s_have_depth_clamp = have.depthClamp;
    {
        const char *e = getenv("RECOMP_VK_MIPS");
        s_mips = !(e && *e == '0');
        e = getenv("RECOMP_VK_ANISO");
        if (e)
            s_aniso = (float)atof(e);
        e = getenv("RECOMP_VK_LOD_BIAS");
        if (e)
            s_lod_bias = (float)atof(e);
        if (s_lod_bias < -pp.limits.maxSamplerLodBias) s_lod_bias = -pp.limits.maxSamplerLodBias;
        if (s_lod_bias > pp.limits.maxSamplerLodBias) s_lod_bias = pp.limits.maxSamplerLodBias;
        if (!have.samplerAnisotropy || !s_mips || !(s_aniso > 1.0f))
            s_aniso = 1.0f;
        if (s_aniso > pp.limits.maxSamplerAnisotropy)
            s_aniso = pp.limits.maxSamplerAnisotropy;
    }
    nv2a_shader_depth_clamp = s_have_depth_clamp;
    {
        VkFormatProperties fp;
        const char *df = getenv("RECOMP_VK_DEPTH_FMT");
        vkGetPhysicalDeviceFormatProperties(s_pd, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
        /* D24S8 is missing on AMD and many Android GPUs; D32_SFLOAT_S8 then.
         * RECOMP_VK_DEPTH_FMT=d32 forces it, to test that path anywhere. */
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                || (df && (df[0] == 'd' || df[0] == 'D') && df[1] == '3'))
            s_depth_fmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
        {
            const char *e = getenv("RECOMP_GL_DXT");
            if (e && *e == '0')
                s_have_bc = 0;
        }
    }

    n = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(s_pd, &n, qf);
    for (s_qfam = 0; s_qfam < n; s_qfam++)
        if (qf[s_qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            break;
    if (s_qfam == n) {
        LOGE("no graphics queue\n");
        return 0;
    }
    if (qf[s_qfam].timestampValidBits)
        s_ts_mask = qf[s_qfam].timestampValidBits >= 64 ? ~0ull
                    : (1ull << qf[s_qfam].timestampValidBits) - 1;

#if defined(__SWITCH__)
    {
        NWindow *win = nwindowGetDefault();
        VkViSurfaceCreateInfoNN sci = { VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN };
        nwindowSetDimensions(win, 1280, 720);
        sci.window = win;
        if (vkCreateViSurfaceNN(s_inst, &sci, NULL, &s_surface) != VK_SUCCESS) {
            LOGE("vkCreateViSurfaceNN failed\n");
            return 0;
        }
    }
#else
    if (!s_headless && !SDL_Vulkan_CreateSurface(s_win, s_inst, NULL, &s_surface)) {
        char w[512];
        snprintf(w, sizeof w, "The window's Vulkan surface could not be created (SDL: %s).", SDL_GetError());
        vk_fatal(w);
    }
#endif

    qci.queueFamilyIndex = s_qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    f13.dynamicRendering = VK_TRUE;
    fvi.vertexInputDynamicState = VK_TRUE;
    feds.extendedDynamicState = VK_TRUE;
    if (!s_baked_vi)
        f13.pNext = &fvi;
    f2.pNext = &f13;
    f2.features.textureCompressionBC = s_have_bc ? VK_TRUE : VK_FALSE;
    f2.features.depthClamp = s_have_depth_clamp ? VK_TRUE : VK_FALSE;
    f2.features.samplerAnisotropy = s_aniso > 1.0f ? VK_TRUE : VK_FALSE;
    if (!s_headless)
        dext[ndext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    dext[ndext++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    if (!s_baked_vi)
        dext[ndext++] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME;
    {
        /* Optional: the driver's VRAM budget/usage (render-scale fit, the
         * RECOMP_DEBUG cache line); without it 80 % of the heap is assumed. */
        static VkExtensionProperties ep[256];
        uint32_t ne = 256, i;
        if (vkEnumerateDeviceExtensionProperties(s_pd, NULL, &ne, ep) >= 0)
            for (i = 0; i < ne; i++)
                if (!strcmp(ep[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
                    dext[ndext++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
                    s_have_budget = 1;
                    break;
                }
    }
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = ndext;
    dci.ppEnabledExtensionNames = dext;
    (void)feds;
    {
        VkResult vr = vkCreateDevice(s_pd, &dci, NULL, &s_dev);
        if (vr != VK_SUCCESS) {
            char w[512];
            LOGE("vkCreateDevice failed (VkResult %d)\n", (int)vr);
            if (s_headless)
                return 0;
            snprintf(w, sizeof w, "%s (driver: Vulkan %u.%u.%u)\nhas everything the renderer needs, but the "
                     "driver refused to create the device (VkResult %d). Updating the graphics driver may help.",
                     pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion),
                     VK_API_VERSION_PATCH(pp.apiVersion), (int)vr);
            vk_fatal(w);
        }
    }
    vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);
#define GET(p, name) p = (void *)vkGetDeviceProcAddr(s_dev, name); \
    if (!p) { LOGE("missing %s\n", name); if (s_headless) return 0; vk_fatal("The driver lacks the function " name "."); }
    GET(p_push_desc, "vkCmdPushDescriptorSetKHR");
    if (s_baked_vi) {
        GET(p_bind_vb2, "vkCmdBindVertexBuffers2");
    } else {
        GET(p_vertex_input, "vkCmdSetVertexInputEXT");
    }
    GET(p_begin_rendering, "vkCmdBeginRendering");
    GET(p_end_rendering, "vkCmdEndRendering");
    GET(p_cull_mode, "vkCmdSetCullMode");
    GET(p_front_face, "vkCmdSetFrontFace");
    GET(p_topology, "vkCmdSetPrimitiveTopology");
    GET(p_depth_test, "vkCmdSetDepthTestEnable");
    GET(p_depth_write, "vkCmdSetDepthWriteEnable");
    GET(p_depth_op, "vkCmdSetDepthCompareOp");
    GET(p_stencil_test, "vkCmdSetStencilTestEnable");
    GET(p_stencil_op, "vkCmdSetStencilOp");
#undef GET

    if (!s_headless && !create_swapchain())
        vk_fatal("The window's swapchain could not be created (the details are in the lines above in the log).");

    {
        VkDescriptorSetLayoutBinding b[7];
        VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        memset(b, 0, sizeof b);
        for (i = 0; i < 7; i++) {
            b[i].binding = i;
            b[i].descriptorCount = 1;
            b[i].descriptorType = i < 2 || i == 6 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                  : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].stageFlags = i == 0 || i == 6 ? VK_SHADER_STAGE_VERTEX_BIT
                                               : VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        lci.bindingCount = 7;
        lci.pBindings = b;
        if (vkCreateDescriptorSetLayout(s_dev, &lci, NULL, &s_dsl) != VK_SUCCESS)
            return 0;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &s_dsl;
        if (vkCreatePipelineLayout(s_dev, &pli, NULL, &s_layout) != VK_SUCCESS)
            return 0;
        {
            char path[256];
            size_t n = 0;
            void *d = cache_path(path, sizeof path, "vkpipecache.bin") ? read_file(path, &n) : NULL;
            pci.initialDataSize = n;
            pci.pInitialData = d;
            if (vkCreatePipelineCache(s_dev, &pci, NULL, &s_pcache) != VK_SUCCESS) {
                pci.initialDataSize = 0;       /* another driver's or a damaged file */
                pci.pInitialData = NULL;
                vkCreatePipelineCache(s_dev, &pci, NULL, &s_pcache);
            }
            if (n)
                fprintf(stderr, "  [VK] pipeline cache: %u KB from %s\n", (unsigned)(n >> 10), path);
            free(d);
        }
    }

    /* always made (VK_FRAMES * 2 queries): the FPS counter can turn the split on
     * in the middle of a run; the stamps are written only while it is counted */
    if (s_ts_mask) {
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        VkPhysicalDeviceProperties tp;
        vkGetPhysicalDeviceProperties(s_pd, &tp);
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = VK_FRAMES * 2;
        if (vkCreateQueryPool(s_dev, &qi, NULL, &s_ts_pool) != VK_SUCCESS)
            s_ts_pool = VK_NULL_HANDLE;
        s_ts_ms = tp.limits.timestampPeriod / 1e6;
    }
    {
        const char *e = getenv("RECOMP_VK_FRAMES");
        if (e && *e == '1')
            s_nframes = 1;
        LOGE("frames in flight: %u\n", s_nframes);
    }
    for (i = 0; i < s_nframes; i++) {
        Frame *f = &s_fr[i];
        VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        VkMemoryRequirements rq;
        cpi.queueFamilyIndex = s_qfam;
        vkCreateCommandPool(s_dev, &cpi, NULL, &f->pool);
        cai.commandPool = f->pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(s_dev, &cai, &f->cb);
        vkCreateFence(s_dev, &fci, NULL, &f->fence);
        vkCreateSemaphore(s_dev, &sci, NULL, &f->acquired);
        vkCreateSemaphore(s_dev, &sci, NULL, &f->rendered);
        bci.size = RING_BYTES;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(s_dev, &bci, NULL, &f->ring) != VK_SUCCESS)
            return 0;
        vkGetBufferMemoryRequirements(s_dev, f->ring, &rq);
        f->ring_mem = alloc_mem(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!f->ring_mem || vkBindBufferMemory(s_dev, f->ring, f->ring_mem, 0) != VK_SUCCESS ||
            vkMapMemory(s_dev, f->ring_mem, 0, RING_BYTES, 0, (void **)&f->ring_ptr) != VK_SUCCESS) {
            LOGE("frame ring allocation failed\n");
            return 0;
        }
    }
    s_fi = 0;
    frame_begin();
    if (!make_image(1, 1, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &s_dummy_img, &s_dummy_view, &s_dummy_mem))
        return 0;
    clear_image_color(s_dummy_img, 0, 0, 0, 0);
    if (!make_image_n(1, 1, 6, VK_FORMAT_B8G8R8A8_UNORM,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, &s_dummy_cube_img, &s_dummy_cube_view,
                      &s_dummy_cube_mem))
        return 0;
    clear_image_color(s_dummy_cube_img, 0, 0, 0, 0);
    if (!s_headless) {                              /* the FPS counter's two pixels, also while off */
        for (i = 0; i < 2; i++) {
            /* SAMPLED only so make_image's view is valid (VUID-VkImageViewCreateInfo-image-04441) */
            if (!make_image(1, 1, VK_FORMAT_B8G8R8A8_UNORM,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, &s_hud_img[i], &s_hud_view[i], &s_hud_mem[i])) {
                s_hud_img[0] = VK_NULL_HANDLE;
                break;
            }
            clear_image_color(s_hud_img[i], (float)i, (float)i, (float)i, 1);
        }
    }
    s_state = 1;
    fprintf(stderr, "  [VK] ready%s: BC %s, depth %s, depth clamp %s, UBO align %u,"
            " mips %s (bias %g), aniso %gx, FXAA %s\n",
            s_headless ? " (headless)" : "", s_have_bc ? "yes" : "no",
            s_depth_fmt == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
            s_have_depth_clamp ? "yes" : "no", (unsigned)s_ubo_align,
            s_mips ? "yes" : "no", s_lod_bias, s_aniso, fxaa_on() ? "on" : "off");
    prewarm();
    return 1;
}

/* Submit what is recorded and wait for it, then keep recording in the same
 * frame (ring full). */
static void flush_frame_wait(void)
{
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    end_rendering();
    s_rt = NULL;
    vkEndCommandBuffer(s_cb);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_cb;
    vkQueueSubmit(s_queue, 1, &si, s_f->fence);
    xbox_ft_push(FT_GPU_WAIT);
    vkWaitForFences(s_dev, 1, &s_f->fence, VK_TRUE, UINT64_MAX);
    xbox_ft_pop();
    vkResetFences(s_dev, 1, &s_f->fence);
    garbage_free(s_f);
    s_f->ring_off = 0;
    vkResetCommandPool(s_dev, s_f->pool, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s_cb, &bi);
    s_cb_gen++;
    s_dyn_valid = 0;
}

/* ── Render state ─────────────────────────────────────────────────── */

static float byte_f(uint32_t v, int shift) { return (float)((v >> shift) & 0xFF) / 255.0f; }
static void argb_vec4(uint32_t v, float out[4])
{
    out[0] = byte_f(v, 16); out[1] = byte_f(v, 8);
    out[2] = byte_f(v, 0);  out[3] = byte_f(v, 24);
}

static VkCompareOp cmp_op(uint32_t v, uint32_t dflt)
{
    if (!v) v = dflt;
    if (v < 0x200 || v > 0x207) v = dflt;
    return (VkCompareOp)(v - 0x200);
}

static VkStencilOp stencil_op(uint32_t v)
{
    switch (v) {
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;         /* KEEP, and 0 (never set) */
    }
}

/* Dynamic state for a draw, set only when it changed. */
typedef struct {
    uint32_t cull, cullmode, front, topo;
    uint32_t dtest, dwrite, dop;
    uint32_t stest, sfail, szfail, spass, sop, smask, swrite, sref;
    uint32_t bcolor;
    float    bias_c, bias_s;              /* vkCmdSetDepthBias constant / slope */
} DynState;
static DynState s_dyn;

/* RECOMP_VK_ZSTATS=1 (debug, M4 decal flicker): per 300 flips, how the title
 * sets depth state -- polygon offset (enables 0x0330/0x0334/0x0338, factor
 * 0x0384 and bias 0x0388 as floats), depth compare ops, the zeta format
 * (SET_SURFACE_FORMAT 0x0208 bits 4..7) and CONTROL0 0x0290 (bit 12 float Z,
 * bit 16 w-buffer). Counted per draw that has a depth buffer. */
static int s_zst = -1;
static uint32_t s_zst_draws, s_zst_off[3], s_zst_func[8], s_zst_zfmt[16], s_zst_float, s_zst_w,
                s_zst_test, s_zst_write;
static struct { uint32_t f, b, n, fill; } s_zst_pair[16];
static int s_zst_npair;

static void zstats_draw(const uint32_t *r)
{
    int k;
    if (s_zst < 0) {
        const char *e = getenv("RECOMP_VK_ZSTATS");
        s_zst = e && *e == '1';
    }
    if (!s_zst)
        return;
    s_zst_draws++;
    if (r[0x330 / 4]) s_zst_off[0]++;
    if (r[0x334 / 4]) s_zst_off[1]++;
    if (r[0x338 / 4]) s_zst_off[2]++;
    s_zst_func[r[0x354 / 4] & 7]++;
    s_zst_zfmt[(r[0x208 / 4] >> 4) & 15]++;
    if (r[0x290 / 4] & 0x1000) s_zst_float++;
    if (r[0x290 / 4] & 0x10000) s_zst_w++;
    if (r[0x30C / 4]) s_zst_test++;
    if (r[0x35C / 4]) s_zst_write++;
    if (r[0x330 / 4] || r[0x334 / 4] || r[0x338 / 4] || r[0x384 / 4] || r[0x388 / 4]) {
        for (k = 0; k < s_zst_npair; k++)
            if (s_zst_pair[k].f == r[0x384 / 4] && s_zst_pair[k].b == r[0x388 / 4])
                break;
        if (k == s_zst_npair && s_zst_npair < 16) {
            s_zst_pair[k].f = r[0x384 / 4];
            s_zst_pair[k].b = r[0x388 / 4];
            s_zst_pair[k].n = s_zst_pair[k].fill = 0;
            s_zst_npair++;
        }
        if (k < 16) {
            s_zst_pair[k].n++;
            if (r[0x338 / 4])
                s_zst_pair[k].fill++;
        }
    }
}

static void zstats_report(uint32_t frame)
{
    int k;
    if (s_zst <= 0 || frame % 300 || !s_zst_draws)
        return;
    fprintf(stderr, "[ZSTATS] frame %u: %u depth draws; test %u write %u; poly offset point %u line %u fill %u;"
            " float-Z %u, w-buffer %u; zeta fmt", frame, s_zst_draws, s_zst_test, s_zst_write,
            s_zst_off[0], s_zst_off[1], s_zst_off[2], s_zst_float, s_zst_w);
    for (k = 0; k < 16; k++)
        if (s_zst_zfmt[k]) fprintf(stderr, " %d:%u", k, s_zst_zfmt[k]);
    fprintf(stderr, "; func");
    for (k = 0; k < 8; k++)
        if (s_zst_func[k]) fprintf(stderr, " 0x20%d:%u", k, s_zst_func[k]);
    for (k = 0; k < s_zst_npair; k++) {
        float f, b;
        memcpy(&f, &s_zst_pair[k].f, 4);
        memcpy(&b, &s_zst_pair[k].b, 4);
        fprintf(stderr, "; offset factor %g bias %g: %u draws (%u fill-enabled)", f, b, s_zst_pair[k].n,
                s_zst_pair[k].fill);
    }
    fprintf(stderr, "\n");
    s_zst_draws = s_zst_test = s_zst_write = s_zst_float = s_zst_w = 0;
    memset(s_zst_off, 0, sizeof s_zst_off);
    memset(s_zst_func, 0, sizeof s_zst_func);
    memset(s_zst_zfmt, 0, sizeof s_zst_zfmt);
    s_zst_npair = 0;
}

/* NV2A polygon offset (SET_POLY_OFFSET_{POINT,LINE,FILL}_ENABLE 0x0330/0x0334/
 * 0x0338, SCALE_FACTOR 0x0384, BIAS 0x0388, floats) as Vulkan depth bias.
 * Slope: the factor as is. Constant: the bias is in units of the guest depth
 * buffer, 1/zmax of the 0..1 range the shader maps guest z to (Z24: 1/0xFFFFFF,
 * Z16: 1/0xFFFF); Vulkan's is r of the host format:
 *   D24S8:     r = 2^-24                 -> scale 2^24 / zmax (Z24 ~1, Z16 256)
 *   D32F (S8): r = 2^(e - 23), e the exponent of the primitive's largest depth;
 *              taken as e = -1 (depth 0.5..1, where a perspective scene lies
 *              almost entirely), which gives the same scale. Nearer than 0.5
 *              the offset halves per octave -- still pulls the decal forward,
 *              and float depth has the precision to spare there.
 * Filled triangles only (FRONT_POLYGON_MODE 0x038C FILL or unset); an offset on
 * points or lines is logged once. THPS 2X Hangar: floor and wall decals with
 * factor -1.25/bias -5 and -1.5/-6 z-fought without this (M4). Off for
 * comparisons: RECOMP_VK_DEPTH_BIAS=0. */
static int s_bias_on = -1;
static uint32_t zmax_of(const uint32_t *r);

static void depth_bias(const uint32_t *r, VkPrimitiveTopology topo, float *c, float *sl)
{
    uint32_t mode = r[0x38C / 4];
    int tri = topo != VK_PRIMITIVE_TOPOLOGY_POINT_LIST && topo != VK_PRIMITIVE_TOPOLOGY_LINE_LIST
              && topo != VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    *c = *sl = 0.0f;
    if (s_bias_on < 0) {
        const char *e = getenv("RECOMP_VK_DEPTH_BIAS");
        s_bias_on = !(e && *e == '0');
    }
    if (!tri || (mode && mode != 0x1B02)) {
        static int told;
        if (!told && (r[0x330 / 4] || r[0x334 / 4]) && (!tri || mode == 0x1B00 || mode == 0x1B01)) {
            told = 1;
            fprintf(stderr, "  [VK] polygon offset on points/lines (topology %d, polygon mode 0x%X):"
                    " not applied\n", (int)topo, mode);
        }
        return;
    }
    if (!s_bias_on || !r[0x338 / 4])
        return;
    {
        float f, b;
        double unit = 16777216.0 / (double)zmax_of(r);   /* guest units -> host r (see above) */
        memcpy(&f, &r[0x384 / 4], 4);
        memcpy(&b, &r[0x388 / 4], 4);
        *c = (float)(b * unit);
        *sl = f;
    }
}

static void apply_dynamic(const uint32_t *r, int has_depth, VkPrimitiveTopology topo)
{
    DynState d;
    if (has_depth)
        zstats_draw(r);
    memset(&d, 0, sizeof d);
    d.cull = r[0x308 / 4] != 0;
    d.cullmode = r[0x39C / 4];
    d.front = r[0x3A0 / 4];
    d.topo = (uint32_t)topo;
    d.dtest = has_depth && r[0x30C / 4];
    d.dwrite = has_depth && r[0x35C / 4];
    d.dop = r[0x354 / 4];
    d.stest = has_depth && r[0x32C / 4];
    if (d.stest) {
        d.sfail = r[0x370 / 4]; d.szfail = r[0x374 / 4]; d.spass = r[0x378 / 4];
        d.sop = r[0x364 / 4]; d.smask = r[0x36C / 4] & 0xFF; d.swrite = r[0x360 / 4] & 0xFF;
        d.sref = r[0x368 / 4] & 0xFF;
    }
    d.bcolor = r[0x34C / 4];
    if (has_depth)
        depth_bias(r, topo, &d.bias_c, &d.bias_s);
    if (s_dyn_valid && !memcmp(&d, &s_dyn, sizeof d))
        return;
    if (!s_dyn_valid || d.bias_c != s_dyn.bias_c || d.bias_s != s_dyn.bias_s)
        vkCmdSetDepthBias(s_cb, d.bias_c, 0.0f, d.bias_s);   /* clamp 0: no feature needed */
    if (!s_dyn_valid || d.cull != s_dyn.cull || d.cullmode != s_dyn.cullmode) {
        VkCullModeFlags cm = VK_CULL_MODE_NONE;
        if (d.cull)
            cm = d.cullmode == 0x404 ? VK_CULL_MODE_FRONT_BIT
               : d.cullmode == 0x408 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT;
        p_cull_mode(s_cb, cm);
    }
    if (!s_dyn_valid || d.front != s_dyn.front)
        /* NV2A winding is as seen on screen, rows top-first -- Vulkan's
         * framebuffer orientation, so CW is CW. nv2a_gl swaps because its
         * window y runs the other way. Swapped here too, every culled draw
         * (29 a frame in the main menu: headlight glass, grille) lost its
         * front faces and kept its back ones. */
        p_front_face(s_cb, d.front == 0x901 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    if (!s_dyn_valid || d.topo != s_dyn.topo)
        p_topology(s_cb, topo);
    if (!s_dyn_valid || d.dtest != s_dyn.dtest) p_depth_test(s_cb, d.dtest);
    if (!s_dyn_valid || d.dwrite != s_dyn.dwrite) p_depth_write(s_cb, d.dwrite);
    if (!s_dyn_valid || d.dop != s_dyn.dop) p_depth_op(s_cb, cmp_op(d.dop, 0x203));
    if (!s_dyn_valid || d.stest != s_dyn.stest) p_stencil_test(s_cb, d.stest);
    if (!s_dyn_valid || memcmp(&d.sfail, &s_dyn.sfail, 7 * sizeof(uint32_t))) {
        p_stencil_op(s_cb, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_op(d.sfail), stencil_op(d.spass),
                     stencil_op(d.szfail), cmp_op(d.sop, 0x207));
        vkCmdSetStencilCompareMask(s_cb, VK_STENCIL_FACE_FRONT_AND_BACK, d.smask);
        vkCmdSetStencilWriteMask(s_cb, VK_STENCIL_FACE_FRONT_AND_BACK, d.swrite);
        vkCmdSetStencilReference(s_cb, VK_STENCIL_FACE_FRONT_AND_BACK, d.sref);
    }
    if (!s_dyn_valid || d.bcolor != s_dyn.bcolor) {
        float bc[4];
        argb_vec4(d.bcolor, bc);
        vkCmdSetBlendConstants(s_cb, bc);
    }
    s_dyn = d;
    s_dyn_valid = 1;
}

static void psh_key(const uint32_t *r, Nv2aPshKey *k)
{
    int i;
    memset(k, 0, sizeof *k);
    for (i = 0; i < 8; i++) {
        k->color_icw[i] = r[0xAC0 / 4 + i];
        k->color_ocw[i] = r[0x1E40 / 4 + i];
        k->alpha_icw[i] = r[0x260 / 4 + i];
        k->alpha_ocw[i] = r[0xAA0 / 4 + i];
    }
    k->control = r[0x1E60 / 4];
    k->final0 = r[0x288 / 4];
    k->final1 = r[0x28C / 4];
    k->shader_program = r[0x1E70 / 4];
    k->other_input = r[0x1E78 / 4];
}

static uint32_t zmax_of(const uint32_t *r)
{
    return (((r[0x208 / 4] >> 4) & 0xF) == 1) ? 0xFFFFu : 0xFFFFFFu;
}

/* NV097 primitive -> Vulkan topology and (possibly rewritten) indices. */
static uint32_t *s_prim_idx;
static size_t    s_prim_cap;

static int prim_indices(const Nv2aRawBatch *b, const uint32_t **idx, uint32_t *n,
                        VkPrimitiveTopology *topo, uint32_t *cls)
{
    uint32_t i, m = 0;
    *idx = b->indices;
    *n = b->index_count;
    switch (b->prim) {
    case 1: *topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; *cls = 0; return 1;
    case 2: *topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; *cls = 1; return 1;
    case 4: *topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1; return 1;
    case 5: *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return 1;
    case 6: case 9: *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; *cls = 2; return 1;
    case 7: case 10: *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; *cls = 2; return 1;
    case 3:                                      /* LINE_LOOP: strip + first */
    case 8: {                                    /* QUADS -> triangles */
        size_t need = b->prim == 3 ? (size_t)b->index_count + 1 : (size_t)b->index_count / 4 * 6;
        if (need > s_prim_cap) {
            free(s_prim_idx);
            s_prim_cap = need + 1024;
            s_prim_idx = (uint32_t *)malloc(s_prim_cap * 4);
        }
        if (!s_prim_idx) { *n = 0; return 0; }
        if (b->prim == 3) {
            if (!b->index_count) { *n = 0; return 0; }
            memcpy(s_prim_idx, b->indices, (size_t)b->index_count * 4);
            s_prim_idx[b->index_count] = b->indices[0];
            m = b->index_count + 1;
            *topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1;
        } else {
            for (i = 0; i + 3 < b->index_count; i += 4) {
                const uint32_t *q = b->indices + i;
                s_prim_idx[m++] = q[0]; s_prim_idx[m++] = q[1]; s_prim_idx[m++] = q[2];
                s_prim_idx[m++] = q[0]; s_prim_idx[m++] = q[2]; s_prim_idx[m++] = q[3];
            }
            *topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2;
        }
        *idx = s_prim_idx;
        *n = m;
        return 1;
    }
    default:
        *n = 0;
        return 0;
    }
}

/* ── Vertices ─────────────────────────────────────────────────────── */

typedef struct { const uint8_t *lo, *hi; VkDeviceSize at; VkBuffer buf; VkDeviceSize boff; } VRun;

/* Vertex mirror (M4 option A stage 2): vertex data the title keeps in the
 * contiguous window stays on the GPU between frames. A device-local copy of
 * the window, in 4 MB chunks made on first use; a page is copied in (staged
 * through the frame ring, like a texture upload) only when page tracking
 * (kernel page_track.c) says the title wrote it since the last copy. Draws
 * then bind the mirror instead of copying their vertex runs into the ring.
 *
 * Ring path instead, as before, when: the run is outside the window or crosses
 * a chunk; a page is streaming (written every frame -- dynamic buffers); or a
 * page that a draw of this frame already read from the mirror was written
 * again (the recorded draw must keep the old bytes). The copy is ordered after
 * every earlier read of the mirror on the queue (barrier), so a frame still in
 * flight is not disturbed.
 *
 * Off by default (RECOMP_VK_VMIRROR=1 turns it on): in THPS 2X it serves 98 %
 * of the vertex runs with a handful of page copies, but the runs were small and
 * its bookkeeping costs what it saves (executor 9.2 % of the frame without it,
 * 9.7 % with it; GPU time the same). Worth it for titles with large static
 * vertex buffers -- measure first. */
#define VM_BASE    0x80000000u
#define VM_WINDOW  (64u << 20)
#define VM_CHUNK   (4u << 20)
#define VM_PAGES   (VM_WINDOW / 4096u)
static struct { VkBuffer buf; VkDeviceMemory mem; int failed; } s_vm[VM_WINDOW / VM_CHUNK];
static uint32_t s_vm_gen[VM_PAGES];     /* generation the mirror page holds */
static uint8_t  s_vm_valid[VM_PAGES];
static uint32_t s_vm_used[VM_PAGES];    /* s_frame + 1 of the last draw that read it */
static int      s_vm_on = -1;
static uint32_t s_vm_st_hit, s_vm_st_ring, s_vm_st_pages, s_vm_st_copies;

static VkBuffer vm_chunk(uint32_t c)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements rq;
    if (s_vm[c].buf || s_vm[c].failed)
        return s_vm[c].buf;
    bci.size = VM_CHUNK;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(s_dev, &bci, NULL, &s_vm[c].buf) != VK_SUCCESS) {
        s_vm[c].buf = VK_NULL_HANDLE;
        s_vm[c].failed = 1;
        return VK_NULL_HANDLE;
    }
    vkGetBufferMemoryRequirements(s_dev, s_vm[c].buf, &rq);
    s_vm[c].mem = alloc_mem(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!s_vm[c].mem || vkBindBufferMemory(s_dev, s_vm[c].buf, s_vm[c].mem, 0) != VK_SUCCESS) {
        vkDestroyBuffer(s_dev, s_vm[c].buf, NULL);
        if (s_vm[c].mem)
            vkFreeMemory(s_dev, s_vm[c].mem, NULL);
        s_vm[c].buf = VK_NULL_HANDLE;
        s_vm[c].mem = VK_NULL_HANDLE;
        s_vm[c].failed = 1;
        LOGE("vertex mirror: no device memory for a 4 MB chunk -- ring path\n");
        return VK_NULL_HANDLE;
    }
    return s_vm[c].buf;
}

/* Serve guest bytes va .. va+len-1 from the mirror: 1 with *buf / *off set. */
static int vm_bind(uint32_t va, uint32_t len, VkBuffer *buf, VkDeviceSize *off, int may_copy)
{
    uint32_t rel, c, p0, p1, p, cur = s_frame + 1, gen;
    uint32_t need_lo = 0, need_n = 0;
    static uint32_t newgen[1024];
    int copied = 0;

    if (s_vm_on < 0) {
        const char *e = getenv("RECOMP_VK_VMIRROR");
        s_vm_on = e && *e == '1';
    }
    if (!s_vm_on || !len || va < VM_BASE || va - VM_BASE >= VM_WINDOW || va - VM_BASE + len > VM_WINDOW)
        return 0;
    rel = va - VM_BASE;
    c = rel / VM_CHUNK;
    if ((rel + len - 1) / VM_CHUNK != c)
        return 0;
    p0 = rel / 4096u;
    p1 = (rel + len - 1) / 4096u;
    if (p1 - p0 >= 1024)
        return 0;
    /* Pass 1: which pages are current; bail out before changing anything. */
    for (p = p0; p <= p1; p++) {
        if (s_vm_valid[p] && xbox_PageTrackPeek(VM_BASE + p * 4096u, &gen) && gen == s_vm_gen[p]) {
            newgen[p - p0] = gen;
            continue;
        }
        if (s_vm_used[p] == cur)
            return 0;                       /* rewritten after a draw of this frame read it */
        {
            uint64_t st = 0;
            xbox_PageTrackCheck(VM_BASE + p * 4096u, 4096u, &st);      /* arms it */
            if (!xbox_PageTrackPeek(VM_BASE + p * 4096u, &gen))
                return 0;                   /* streaming / not trackable */
            newgen[p - p0] = gen | 0x80000000u;   /* marked: copy */
        }
    }
    if (!vm_chunk(c))
        return 0;
    if (!may_copy)
        for (p = p0; p <= p1; p++)
            if (newgen[p - p0] & 0x80000000u)
                return 0;                   /* inside a render pass: no copies, ring path */
    /* Pass 2: copy the stale pages (armed before they are read). */
    for (p = p0; p <= p1 + 1; p++) {
        int stale = p <= p1 && (newgen[p - p0] & 0x80000000u);
        if (stale) {
            if (!need_n)
                need_lo = p;
            need_n++;
            continue;
        }
        if (need_n) {
            void *dst;
            VkDeviceSize at = ring_alloc((VkDeviceSize)need_n * 4096u, 16, &dst);
            VkBufferCopy rg;
            if (!dst)
                return 0;
            if (!copied) {
                end_rendering();
                barrier_all();              /* after every earlier read of the mirror */
                copied = 1;
            }
            memcpy(dst, (const uint8_t *)xbox_GetMemoryOffset() + VM_BASE + need_lo * 4096u,
                   (size_t)need_n * 4096u);
            rg.srcOffset = at;
            rg.dstOffset = (VkDeviceSize)need_lo * 4096u - (VkDeviceSize)c * VM_CHUNK;
            rg.size = (VkDeviceSize)need_n * 4096u;
            vkCmdCopyBuffer(s_cb, s_f->ring, s_vm[c].buf, 1, &rg);
            s_vm_st_pages += need_n;
            s_vm_st_copies++;
            need_n = 0;
        }
    }
    if (copied)
        barrier_all();                      /* copies before the draws that read them */
    for (p = p0; p <= p1; p++) {
        s_vm_gen[p] = newgen[p - p0] & 0x7FFFFFFFu;
        s_vm_valid[p] = 1;
        s_vm_used[p] = cur;
    }
    *buf = s_vm[c].buf;
    *off = (VkDeviceSize)rel - (VkDeviceSize)c * VM_CHUNK;
    return 1;
}

static uint32_t elem_bytes(uint32_t type, uint32_t size)
{
    switch (type) {
    case 0: return 4;
    case 2: return 4 * size;
    case 4: return size;
    default: return 2 * size;
    }
}

static uint32_t vertex_runs(const Nv2aRawBatch *b, uint16_t direct, VRun *run, int *of_run)
{
    uint32_t nv = b->vertex_count, nrun = 0, a, i;
    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        const uint8_t *lo, *hi;
        if (!(direct & (1u << a)))
            continue;
        lo = (const uint8_t *)((uintptr_t)b->direct[a].ptr & ~(uintptr_t)3);
        hi = b->direct[a].ptr + (size_t)(nv - 1) * b->direct[a].stride
           + elem_bytes(b->direct[a].type, b->direct[a].size);
        for (i = 0; i < nrun; i++)
            if (lo <= run[i].hi && run[i].lo <= hi)
                break;
        if (i == nrun) {
            run[nrun].lo = lo;
            run[nrun].hi = hi;
            nrun++;
        } else {
            if (lo < run[i].lo) run[i].lo = lo;
            if (hi > run[i].hi) run[i].hi = hi;
        }
        of_run[a] = (int)i;
    }
    for (i = 0; i + 1 < nrun; ) {
        uint32_t j;
        for (j = i + 1; j < nrun; j++)
            if (run[j].lo <= run[i].hi && run[i].lo <= run[j].hi)
                break;
        if (j == nrun) { i++; continue; }
        if (run[j].lo < run[i].lo) run[i].lo = run[j].lo;
        if (run[j].hi > run[i].hi) run[i].hi = run[j].hi;
        for (a = 0; a < NV2A_RAW_ATTRS; a++) {
            if (!(direct & (1u << a)))
                continue;
            if (of_run[a] == (int)j) of_run[a] = (int)i;
            else if (of_run[a] == (int)(nrun - 1)) of_run[a] = (int)j;
        }
        run[j] = run[nrun - 1];
        nrun--;
    }
    return nrun;
}

/* The Vulkan vertex format of a direct attribute, or UNDEFINED when the
 * device cannot fetch it (then it is converted to float4 here). */
static VkFormat s_vfmt_cache[8][5];
static int      s_vfmt_known[8][5];

static VkFormat vertex_format(uint32_t type, uint32_t size)
{
    static const VkFormat f_float[5] = { 0, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
                                         VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
    static const VkFormat f_snorm16[5] = { 0, VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM,
                                           VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM };
    static const VkFormat f_unorm8[5] = { 0, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM,
                                          VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
    static const VkFormat f_sscaled16[5] = { 0, VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED,
                                             VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED };
    VkFormat f;
    if (size < 1 || size > 4 || type > 7)
        return VK_FORMAT_UNDEFINED;
    if (s_vfmt_known[type][size])
        return s_vfmt_cache[type][size];
    switch (type) {
    case 0: f = VK_FORMAT_B8G8R8A8_UNORM; break;
    case 1: f = f_snorm16[size]; break;
    case 2: f = f_float[size]; break;
    case 4: f = f_unorm8[size]; break;
    case 5: f = f_sscaled16[size]; break;
    default: f = VK_FORMAT_UNDEFINED; break;
    }
    if (f != VK_FORMAT_UNDEFINED) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(s_pd, f, &fp);
        if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
            if (s_trace)
                LOGE("vertex format %d (type %u size %u) not supported: converted\n", f, type, size);
            f = VK_FORMAT_UNDEFINED;
        }
    }
    s_vfmt_known[type][size] = 1;
    s_vfmt_cache[type][size] = f;
    return f;
}

/* s_baked_vi: the vertex formats upload_vertices will bind, for the pipeline
 * key -- the device format of a direct attribute, float4 for the converted
 * and the absent ones. */
static void vertex_formats(const Nv2aRawBatch *b, uint8_t out[NV2A_RAW_ATTRS])
{
    uint32_t a;
    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        VkFormat f = VK_FORMAT_UNDEFINED;
        if (b->attr_direct & (1u << a))
            f = vertex_format(b->direct[a].type, b->direct[a].size);
        out[a] = (uint8_t)(f != VK_FORMAT_UNDEFINED ? f : VK_FORMAT_R32G32B32A32_SFLOAT);
    }
}

/* One attribute of vertex v, as the NV2A reads it, into float4. */
static void attr_to_float4(const Nv2aRawBatch *b, uint32_t a, uint32_t v, float out[4])
{
    const uint8_t *p = b->direct[a].ptr + (size_t)v * b->direct[a].stride;
    uint32_t size = b->direct[a].size, c;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (b->direct[a].type) {
    case 0:                                          /* D3DCOLOR: B,G,R,A */
        out[0] = p[2] / 255.0f; out[1] = p[1] / 255.0f; out[2] = p[0] / 255.0f; out[3] = p[3] / 255.0f;
        break;
    case 1:
        for (c = 0; c < size && c < 4; c++) {
            int16_t s; memcpy(&s, p + 2 * c, 2);
            out[c] = s / 32767.0f < -1.0f ? -1.0f : s / 32767.0f;
        }
        break;
    case 2:
        for (c = 0; c < size && c < 4; c++) memcpy(&out[c], p + 4 * c, 4);
        break;
    case 4:
        for (c = 0; c < size && c < 4; c++) out[c] = p[c] / 255.0f;
        break;
    default:
        for (c = 0; c < size && c < 4; c++) { int16_t s; memcpy(&s, p + 2 * c, 2); out[c] = (float)s; }
        break;
    }
}

/* Before the render pass of a draw begins: copy what its vertex runs need
 * into the mirror (a copy cannot go inside a pass); upload_vertices then
 * binds them without copying. */
static void vm_prepare(const Nv2aRawBatch *b)
{
    VRun run[NV2A_RAW_ATTRS];
    int of_run[NV2A_RAW_ATTRS];
    uint32_t nrun, a, i;
    uint16_t direct = 0;
    if (s_vm_on < 0) {
        const char *e = getenv("RECOMP_VK_VMIRROR");
        s_vm_on = e && *e == '1';
    }
    if (!b->vertex_count || !s_vm_on)
        return;
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if ((b->attr_direct & (1u << a))
            && vertex_format(b->direct[a].type, b->direct[a].size) != VK_FORMAT_UNDEFINED)
            direct |= (uint16_t)(1u << a);
    if (!direct)
        return;
    nrun = vertex_runs(b, direct, run, of_run);
    for (i = 0; i < nrun; i++) {
        uintptr_t gva = (uintptr_t)run[i].lo - (uintptr_t)xbox_GetMemoryOffset();
        VkBuffer buf;
        VkDeviceSize off;
        if (gva >= VM_BASE && gva < VM_BASE + VM_WINDOW)
            vm_bind((uint32_t)gva, (uint32_t)(run[i].hi - run[i].lo), &buf, &off, 1);
    }
}

static int upload_vertices(const Nv2aRawBatch *b)
{
    VRun run[NV2A_RAW_ATTRS];
    int of_run[NV2A_RAW_ATTRS];
    uint32_t slot[NV2A_RAW_ATTRS], np = 0, nrun, a, i, v;
    uint32_t nv = b->vertex_count;
    uint16_t direct = 0, conv, cdirect = 0;
    VkDeviceSize bytes = 0, pack_at, const_at, at;
    VkFormat fmt[NV2A_RAW_ATTRS];
    uint8_t *dst;
    VkVertexInputBindingDescription2EXT bind[NV2A_RAW_ATTRS];
    VkVertexInputAttributeDescription2EXT attr[NV2A_RAW_ATTRS];
    VkBuffer bufs[NV2A_RAW_ATTRS];
    VkDeviceSize offs[NV2A_RAW_ATTRS];
    void *map;

    if (!nv)
        return 0;
    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        fmt[a] = VK_FORMAT_UNDEFINED;
        if (!(b->attr_direct & (1u << a)))
            continue;
        fmt[a] = vertex_format(b->direct[a].type, b->direct[a].size);
        if (fmt[a] != VK_FORMAT_UNDEFINED)
            direct |= (uint16_t)(1u << a);
        else
            cdirect |= (uint16_t)(1u << a);          /* direct data, converted here */
    }
    conv = b->attr_present & (uint16_t)~direct;
    nrun = vertex_runs(b, direct, run, of_run);
    for (i = 0; i < nrun; i++) {
        uintptr_t gva = (uintptr_t)run[i].lo - (uintptr_t)xbox_GetMemoryOffset();
        run[i].buf = VK_NULL_HANDLE;
        if (gva >= VM_BASE && gva < VM_BASE + VM_WINDOW
            && vm_bind((uint32_t)gva, (uint32_t)(run[i].hi - run[i].lo), &run[i].buf, &run[i].boff, 0)) {
            s_vm_st_hit++;
            continue;                       /* served from the mirror, no ring space */
        }
        s_vm_st_ring++;
        run[i].at = bytes;
        bytes += ((VkDeviceSize)(run[i].hi - run[i].lo) + 15) & ~(VkDeviceSize)15;
    }
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if (conv & (1u << a))
            slot[np++] = a;
    pack_at = bytes;
    bytes += (VkDeviceSize)nv * np * 16;
    const_at = bytes;
    bytes += NV2A_RAW_ATTRS * 16;
    at = ring_alloc(bytes, 16, &map);
    dst = (uint8_t *)map;
    if (!dst)
        return 0;
    for (i = 0; i < nrun; i++)
        if (!run[i].buf)
            memcpy(dst + run[i].at, run[i].lo, (size_t)(run[i].hi - run[i].lo));
    if (np) {
        float *out = (float *)(dst + pack_at);
        for (v = 0; v < nv; v++)
            for (i = 0; i < np; i++) {
                float *o = out + ((size_t)v * np + i) * 4;
                if (cdirect & (1u << slot[i]))
                    attr_to_float4(b, slot[i], v, o);
                else
                    memcpy(o, b->attrs + ((size_t)v * NV2A_RAW_ATTRS + slot[i]) * 4, 16);
            }
    }
    memcpy(dst + const_at, b->attr_const, NV2A_RAW_ATTRS * 16);

    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        memset(&bind[a], 0, sizeof bind[a]);
        memset(&attr[a], 0, sizeof attr[a]);
        bind[a].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT;
        bind[a].binding = a;
        bind[a].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bind[a].divisor = 1;
        attr[a].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT;
        attr[a].location = a;
        attr[a].binding = a;
        bufs[a] = s_f->ring;
        if (direct & (1u << a)) {
            const VRun *r = &run[of_run[a]];
            bind[a].stride = b->direct[a].stride;
            attr[a].format = fmt[a];
            if (r->buf) {
                bufs[a] = r->buf;
                offs[a] = r->boff + (VkDeviceSize)(b->direct[a].ptr - r->lo);
            } else {
                offs[a] = at + r->at + (VkDeviceSize)(b->direct[a].ptr - r->lo);
            }
        } else if (conv & (1u << a)) {
            for (i = 0; i < np && slot[i] != a; i++) { }
            bind[a].stride = np * 16;
            attr[a].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            offs[a] = at + pack_at + i * 16;
        } else {
            /* Absent: the title's SET_VERTEX_DATA* value, stride 0. */
            bind[a].stride = 0;
            attr[a].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            offs[a] = at + const_at + a * 16;
        }
    }
    if (s_baked_vi) {
        /* The formats are in the pipeline (vertex_formats, same choice as
         * above); only the strides go with the buffers. */
        VkDeviceSize strides[NV2A_RAW_ATTRS];
        for (a = 0; a < NV2A_RAW_ATTRS; a++)
            strides[a] = bind[a].stride;
        p_bind_vb2(s_cb, 0, NV2A_RAW_ATTRS / 2, bufs, offs, NULL, strides);
        p_bind_vb2(s_cb, NV2A_RAW_ATTRS / 2, NV2A_RAW_ATTRS - NV2A_RAW_ATTRS / 2,
                   bufs + NV2A_RAW_ATTRS / 2, offs + NV2A_RAW_ATTRS / 2, NULL, strides + NV2A_RAW_ATTRS / 2);
        return 1;
    }
    {
        /* Vertex input is dynamic state: re-send it only when it changed
         * (or the command buffer is new) -- consecutive draws of a mesh type
         * repeat it, and NVK re-validates all 16 bindings each time. */
        static VkVertexInputBindingDescription2EXT last_bind[NV2A_RAW_ATTRS];
        static VkVertexInputAttributeDescription2EXT last_attr[NV2A_RAW_ATTRS];
        static uint32_t last_gen;
        if (last_gen != s_cb_gen || memcmp(bind, last_bind, sizeof bind)
            || memcmp(attr, last_attr, sizeof attr)) {
            p_vertex_input(s_cb, NV2A_RAW_ATTRS, bind, NV2A_RAW_ATTRS, attr);
            memcpy(last_bind, bind, sizeof bind);
            memcpy(last_attr, attr, sizeof attr);
            last_gen = s_cb_gen;
        }
    }
    /* Eight at a time: Mesa's common entry point copies the arrays through
     * STACK_ARRAY, which mallocs above 8 elements (~3.6% of the executor
     * on the console in malloc/free). */
    vkCmdBindVertexBuffers(s_cb, 0, NV2A_RAW_ATTRS / 2, bufs, offs);
    vkCmdBindVertexBuffers(s_cb, NV2A_RAW_ATTRS / 2, NV2A_RAW_ATTRS - NV2A_RAW_ATTRS / 2,
                           bufs + NV2A_RAW_ATTRS / 2, offs + NV2A_RAW_ATTRS / 2);
    return 1;
}

/* ── Back end entry points ────────────────────────────────────────── */

/* std140 blocks (gl_vsh.c / gl_psh.c, Vulkan dialect). */
typedef struct {
    float surf[4];
    float m[16];
    float vpoff[4];
    float aa[2];
    int32_t xform;
    float snap_y;              /* nv2a_snap's offset for y; surf[3] is x's */
    float fogv[4];             /* fixed-function fog plane in object space */
    int32_t foggen;            /* 0 none, 1 spec alpha, 2 planar, 3 abs planar, 4 fog coord */
    int32_t d1mode;            /* vertex programs: 1 specular off, 2 on (alpha 1), 3 on (alpha kept) */
    int32_t lit;               /* fixed-function lighting (gl_vsh_ff.c) */
    int32_t colmat;
    int32_t lmask;
    int32_t lflags;            /* 1 normalize, 2 local eye, 4 separate specular, 8 specular, 16 spec alpha */
    float specpow;
    int32_t pad_;
    float mv[16], imv[16];
    float samb[4], memis[4], eye[4];
    float light[64][4];        /* 8 lights x amb, dif, spc, half, dir, pos+range, atten, spot dir */
    int32_t tgen;              /* bit 4*stage+c REFLECTION_MAP texgen, bit 16+stage texture matrix */
    int32_t pad2_[3];
    float tmat[16][4];         /* SET_TEXTURE_MATRIX, 4 stages x 4 rows */
} VsBlock;

/* Specular exponent from SET_SPECULAR_PARAMS (gl_vsh_ff.c, after xemu). */
float nv2a_gl_specular_power(const float *params);

typedef struct {
    float tscale[4][4];          /* vec2 array: 16-byte elements */
    float c0[8][4], c1[8][4];
    float fc0[4], fc1[4];
    float fog[4];
    int32_t afunc;
    float aref;
    float pad[2];
    float fogp[4];               /* nv2a_fog_params */
} FsBlock;

/* ── Instancing ───────────────────────────────────────────────────────
 *
 * A quarter of a race frame's draws repeat the previous draw with only the
 * transform constants changed (NFSU2's drag start line: ~480 of ~1870) --
 * the same mesh at another matrix. Such a run becomes one instanced draw:
 * the first draw is recorded up to its vkCmdDrawIndexed, which is held
 * back (s_pend); every repeat only appends its 192 constants right behind
 * the previous set in the ring (binding 6, which the vertex shader indexes
 * by gl_InstanceIndex * 192); anything else -- another draw, a clear, a
 * flip -- first issues the held draw with the instance count. Instances
 * are drawn in order, so blending and depth come out as separate draws.
 *
 * "Repeat" means: no register changed since the held draw except the
 * constant and program upload windows and the per-draw methods (executor
 * dirty blocks, compared to a copy), the same program, vertex pointers,
 * formats and count, the same indices, absent-attribute values and
 * texture addresses, and no semaphore release or trap in between
 * (vtx_epoch: vertex bytes the title could have rewritten). Only vertex
 * programs (xform 2) whose attributes are all uploaded as stored.
 * RECOMP_VK_INSTANCE=0 turns it off. */
#define VK_INST_MAX   16                    /* gl_vsh.c: c[192 * 16] */
#define VK_CONST_SIZE (192 * 16)

extern uint8_t *nv2a_pb_reg_dirty(void);
static uint32_t s_regs_seen[0x2000 / 4];    /* registers as of the last draw */

/* Did a register a draw depends on change since the last draw? */
static int regs_changed(const uint32_t *r)
{
    uint8_t *dirty = nv2a_pb_reg_dirty();
    uint32_t blk, w;
    int changed = 0;

    for (blk = 0; blk < 0x2000 / 64; blk++) {
        if (!dirty[blk])
            continue;
        dirty[blk] = 0;
        if (blk >= 0xB00 / 64 && blk < 0xC00 / 64)
            continue;                           /* program / constant uploads */
        for (w = blk * 16; w < blk * 16 + 16; w++) {
            uint32_t m = w * 4;
            if (r[w] == s_regs_seen[w])
                continue;
            s_regs_seen[w] = r[w];
            if (m == 0x0100 || m == 0x17FC || (m >= 0x1800 && m < 0x1820)
             || m == 0x1D6C || m == 0x1D70 || m == 0x1E9C || m == 0x1EA4)
                continue;                       /* NOP, BEGIN_END, draw data, semaphore, load slots */
            changed = 1;
        }
    }
    return changed;
}

static struct {
    int          on;                        /* a draw is held */
    uint32_t     n, count, max;             /* index count, instances so far, room */
    VkDeviceSize cat;                       /* first instance's constants */
    uint32_t     cb_gen;
    uint32_t     prim, vertex_count, index_count;
    uint16_t     attr_present, attr_direct;
    uint32_t     vp_prog_gen, attr_const_gen, vtx_epoch, color_va, zeta_va;
    uint32_t     tex_va[4], pal_va[4];
    float        aa_sx, aa_sy;
    Nv2aSurface  surface;
    struct { const uint8_t *ptr; uint32_t type, size, stride; } direct[NV2A_RAW_ATTRS];
    uint32_t    *idx;
    size_t       idx_cap;
} s_pend;
static int s_inst_on = -1, s_inst_alt;
static uint32_t s_st_draws, s_st_calls, s_st_inst;

static void pend_flush(void)
{
    if (!s_pend.on)
        return;
    s_pend.on = 0;
    if (s_pend.cb_gen != s_cb_gen)
        return;                                 /* cannot happen: flushes go first */
    vkCmdDrawIndexed(s_cb, s_pend.n, s_pend.count, 0, 0, 0);
    s_st_calls++;
}

static int pend_matches(const Nv2aRawBatch *b)
{
    uint32_t a;
    if (b->xform != 2 || b->prim != s_pend.prim || b->vertex_count != s_pend.vertex_count
     || b->index_count != s_pend.index_count || b->attr_present != s_pend.attr_present
     || b->attr_direct != s_pend.attr_direct || b->vp_prog_gen != s_pend.vp_prog_gen
     || b->attr_const_gen != s_pend.attr_const_gen || b->vtx_epoch != s_pend.vtx_epoch
     || b->color_va != s_pend.color_va || b->zeta_va != s_pend.zeta_va
     || b->aa_sx != s_pend.aa_sx || b->aa_sy != s_pend.aa_sy
     || memcmp(b->tex_va, s_pend.tex_va, sizeof s_pend.tex_va)
     || memcmp(b->pal_va, s_pend.pal_va, sizeof s_pend.pal_va)
     || memcmp(&b->surface, &s_pend.surface, sizeof s_pend.surface))
        return 0;
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if ((b->attr_direct & (1u << a))
         && (b->direct[a].ptr != s_pend.direct[a].ptr || b->direct[a].type != s_pend.direct[a].type
          || b->direct[a].size != s_pend.direct[a].size || b->direct[a].stride != s_pend.direct[a].stride))
            return 0;
    return !memcmp(b->indices, s_pend.idx, (size_t)b->index_count * 4);
}

/* Can b be held as the first instance of a run? */
static int pend_eligible(const Nv2aRawBatch *b)
{
    uint32_t a;
    if (!s_inst_on || b->xform != 2 || (b->attr_present & ~b->attr_direct))
        return 0;
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if ((b->attr_direct & (1u << a))
         && vertex_format(b->direct[a].type, b->direct[a].size) == VK_FORMAT_UNDEFINED)
            return 0;
    return 1;
}

static void pend_hold(const Nv2aRawBatch *b, uint32_t n, VkDeviceSize cat, uint32_t max)
{
    uint32_t a;
    s_pend.on = 1;
    s_pend.n = n;
    s_pend.count = 1;
    s_pend.max = max;
    s_pend.cat = cat;
    s_pend.cb_gen = s_cb_gen;
    s_pend.prim = b->prim;
    s_pend.vertex_count = b->vertex_count;
    s_pend.index_count = b->index_count;
    s_pend.attr_present = b->attr_present;
    s_pend.attr_direct = b->attr_direct;
    s_pend.vp_prog_gen = b->vp_prog_gen;
    s_pend.attr_const_gen = b->attr_const_gen;
    s_pend.vtx_epoch = b->vtx_epoch;
    s_pend.color_va = b->color_va;
    s_pend.zeta_va = b->zeta_va;
    s_pend.aa_sx = b->aa_sx;
    s_pend.aa_sy = b->aa_sy;
    s_pend.surface = b->surface;
    memcpy(s_pend.tex_va, b->tex_va, sizeof s_pend.tex_va);
    memcpy(s_pend.pal_va, b->pal_va, sizeof s_pend.pal_va);
    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        s_pend.direct[a].ptr = b->direct[a].ptr;
        s_pend.direct[a].type = b->direct[a].type;
        s_pend.direct[a].size = b->direct[a].size;
        s_pend.direct[a].stride = b->direct[a].stride;
    }
    if (b->index_count > s_pend.idx_cap) {
        free(s_pend.idx);
        s_pend.idx_cap = b->index_count + 1024;
        s_pend.idx = (uint32_t *)malloc(s_pend.idx_cap * 4);
        if (!s_pend.idx) {
            s_pend.idx_cap = 0;
            s_pend.on = 0;
            vkCmdDrawIndexed(s_cb, n, 1, 0, 0, 0);
            s_st_calls++;
            return;
        }
    }
    memcpy(s_pend.idx, b->indices, (size_t)b->index_count * 4);
}

/* One more instance of the held draw, when b is one; 0: draw it normally. */
static int pend_append(const Nv2aRawBatch *b, int changed)
{
    VkDeviceSize at;
    void *map;

    if (!s_pend.on)
        return 0;
    if (changed || s_pend.count >= s_pend.max || s_pend.cb_gen != s_cb_gen || !pend_matches(b))
        return 0;
    at = (s_f->ring_off + s_ubo_align - 1) & ~(s_ubo_align - 1);
    if (at != s_pend.cat + (VkDeviceSize)s_pend.count * VK_CONST_SIZE
     || at + VK_CONST_SIZE > RING_BYTES)
        return 0;
    at = ring_alloc(VK_CONST_SIZE, s_ubo_align, &map);
    if (!map)
        return 0;
    memcpy(map, b->vp_consts, VK_CONST_SIZE);
    s_pend.count++;
    s_st_inst++;
    return 1;
}

static void vk_draw_raw(const Nv2aRawBatch *b)
{
    VkSurf *s, *sampled[4];
    VkDepthBuf *d;
    VkProg *p;
    Nv2aPshKey pk;
    const uint32_t *r = b->regs;
    const uint32_t *idx;
    uint32_t n, i, cls;
    VkPrimitiveTopology topo;
    VkDescriptorImageInfo img[4];
    float tscale[4][2];
    PipeKey key;
    VkPipeline pipe;
    VkDescriptorBufferInfo ub[3];
    VkWriteDescriptorSet wr[7];
    uint32_t inst_room = 0;
    void *map;
    static VkPipeline cur_pipe;
    static uint32_t cur_pipe_gen;

    if (!ready())
        return;
    s_regs = r;
    s_batch = b;
    if (s_inst_on < 0) {
        const char *e = getenv("RECOMP_VK_INSTANCE");
        s_inst_on = !(e && *e == '0') && s_ubo_range >= VK_CONST_SIZE
                 && VK_CONST_SIZE % s_ubo_align == 0;
        if (s_inst_on && e && *e == '2')
            s_inst_alt = 1;                     /* debug: every other frame */
        fprintf(stderr, "  [VK] instancing %s (uniform range %u, alignment %u)\n",
                s_inst_on ? "on" : "off", s_ubo_range, (unsigned)s_ubo_align);
    }
    s_st_draws++;
    {
        int changed = regs_changed(r);
        if (pend_append(b, changed))
            return;
    }
    pend_flush();
    if (!prim_indices(b, &idx, &n, &topo, &cls) || !n)
        return;
    VT("draw: prim %u idx %u verts %u attrs %04X direct %04X xform %u surf %08X zeta %08X\n", b->prim, n,
       b->vertex_count, b->attr_present, b->attr_direct, b->xform, b->surface.color_va, b->zeta_va);
    s = target(&b->surface, b->zeta_va, &d);
    if (!s)
        return;
    s_drew_any = 1;
    psh_key(r, &pk);
    {
        uint32_t made = s_prog_made;
        uint64_t t0 = hz_now();
        p = prog_get(b, &pk);
        if (s_prog_made != made) {
            s_hz_progs += s_prog_made - made;
            s_hz_prog_ns += hz_now() - t0;
        }
    }
    if (!p)
        return;
    s_hz_draws++;

    /* Textures first: uploads end the rendering pass. */
    for (i = 0; i < 4; i++)
        bind_stage(r, (int)i, tscale[i], &img[i], &sampled[i]);
    for (i = 0; i < 4; i++)
        if (sampled[i] && sampled[i]->dirty && sampled[i] != s) {
            end_rendering();                 /* next begin_rendering barriers */
            break;
        }

    memset(&key, 0, sizeof key);
    key.prog = (int)(p - s_prog);
    if (r[0x304 / 4]) {
        key.blend = 1; key.bsrc = r[0x344 / 4]; key.bdst = r[0x348 / 4]; key.beq = r[0x350 / 4];
    }
    {
        uint32_t cm = r[0x358 / 4], m = 0;
        if (cm & 0x00010000u) m |= VK_COLOR_COMPONENT_R_BIT;
        if (cm & 0x00000100u) m |= VK_COLOR_COMPONENT_G_BIT;
        if (cm & 0x00000001u) m |= VK_COLOR_COMPONENT_B_BIT;
        if (cm & 0x01000000u) m |= VK_COLOR_COMPONENT_A_BIT;
        key.cmask = m;
    }
    if (d && d->ms != s->ms)
        d = NULL;                            /* as begin_rendering attaches it */
    key.depth = d != NULL;
    key.topo = cls;
    key.samples = s->ms;                     /* MSAA: the presented surface's sample count */
    if (s_baked_vi)
        vertex_formats(b, key.vfmt);
    pipe = pipe_get(&key);
    if (!pipe)
        return;

    {
        /* Everything this draw puts in the ring, reserved now: a flush
         * later in the draw would reuse the ring under its own uniforms. */
        VkDeviceSize need = 2 * 4096 + sizeof(VsBlock) + sizeof(FsBlock) + VK_CONST_SIZE + (VkDeviceSize)n * 4
                          + NV2A_RAW_ATTRS * 16 + 16 * 16;
        uint32_t a;
        for (a = 0; a < NV2A_RAW_ATTRS; a++)
            if (b->attr_present & (1u << a)) {
                uint32_t st = (b->attr_direct & (1u << a)) ? b->direct[a].stride : 0;
                need += (VkDeviceSize)b->vertex_count * (st > 16 ? st : 16) + 16;
            }
        if (s_f->ring_off + need > RING_BYTES) {
            void *tmp;
            if (need > RING_BYTES) return;
            ring_alloc(RING_BYTES, 16, &tmp);       /* forces the flush */
            s_f->ring_off = 0;
        }
    }
    vm_prepare(b);                          /* mirror copies, outside the pass */
    begin_rendering(s, d);
    s->dirty = 1;
    s->gen++;
    if (pipe != cur_pipe || cur_pipe_gen != s_cb_gen || s_pipe_foreign) {
        vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        cur_pipe = pipe;
        cur_pipe_gen = s_cb_gen;
        s_pipe_foreign = 0;
    }
    apply_dynamic(r, d != NULL, topo);
    set_surface_scissor(r, s);

    /* Uniforms. */
    {
        VsBlock *vb;
        FsBlock *fb;
        VkDeviceSize vat = ring_alloc(sizeof(VsBlock), s_ubo_align, &map);
        vb = (VsBlock *)map;
        if (!vb) return;
        vb->surf[0] = 2.0f / (float)s->w;
        vb->surf[1] = 2.0f / (float)s->h;
        vb->surf[2] = 1.0f / (float)zmax_of(r);
        vb->surf[3] = 0.5f - 0.5f * (float)s->w / (float)s->pw; /* see nv2a_snap */
        vb->snap_y = 0.5f - 0.5f * (float)s->h / (float)s->ph;
        memcpy(vb->m, b->composite, sizeof vb->m);
        memcpy(vb->vpoff, b->vp_offset, sizeof vb->vpoff);
        vb->aa[0] = b->aa_sx > 0 ? b->aa_sx : 1.0f;
        vb->aa[1] = b->aa_sy > 0 ? b->aa_sy : 1.0f;
        vb->xform = b->xform == 1 ? 1 : 0;
        /* The fog unit's input for fixed-function vertices (SET_FOG_GEN_MODE,
         * as xemu's vsh-ff.c reads it). Planar: distance = plane . eye + w,
         * eye = MV * v0 (MODEL_VIEW_MATRIX0, rows as delivered, as lit_color
         * in the executor uses it), folded into one object-space vector.
         * Radial is approximated as planar. Without this a fixed-function
         * draw with fog on had distance 0 -- full fog, THPS 2X's grey menus. */
        if (b->xform == 1 && r[0x02A4 / 4]) {
            const float *mv = (const float *)&r[0x0480 / 4];
            const float *pl = (const float *)&r[0x09D0 / 4];
            uint32_t gen = r[0x02A0 / 4], j;
            vb->foggen = gen == 0 ? 1 : gen == 3 ? 3 : gen == 6 ? 4 : 2;
            for (j = 0; j < 4; j++)
                vb->fogv[j] = pl[0] * mv[j] + pl[1] * mv[4 + j] + pl[2] * mv[8 + j]
                            + (j == 3 ? pl[3] : 0.0f);
        }
        /* Specular enable for vertex programs (D1 out), fixed-function
         * lighting with specular and normalization (gl_vsh.c). */
        {
            int spec = r[0x03B8 / 4] != 0;
            uint32_t lc = r[0x0294 / 4];
            vb->d1mode = !spec ? 1 : (lc & (1u << 17)) ? 3 : 2;
            vb->lflags = (r[0x03A4 / 4] ? 1 : 0) | ((lc & (1u << 16)) ? 2 : 0) | ((lc & 1u) ? 4 : 0)
                       | (spec ? 8 : 0) | ((lc & (1u << 17)) ? 16 : 0) | (s_ffp_mark ? 32 : 0);
            vb->lit = b->xform == 1 && r[0x0314 / 4] && s_ffp_light;
            if (vb->lit) {
                int li;
                vb->colmat = (int32_t)r[0x0298 / 4];
                vb->lmask = (int32_t)r[0x03BC / 4];
                vb->specpow = nv2a_gl_specular_power((const float *)&r[0x09E0 / 4]);
                memcpy(vb->mv, &r[0x0480 / 4], sizeof vb->mv);
                memcpy(vb->imv, &r[0x0580 / 4], sizeof vb->imv);
                memcpy(vb->samb, &r[0x0A10 / 4], 3 * sizeof(float));
                memcpy(vb->memis, &r[0x03A8 / 4], 3 * sizeof(float));
                memcpy(&vb->memis[3], &r[0x03B4 / 4], sizeof(float));
                memcpy(vb->eye, &r[0x0A50 / 4], sizeof vb->eye);
                for (li = 0; li < 8; li++) {
                    const uint32_t *L = &r[(0x1000 + li * 0x80) / 4];
                    float *o = vb->light[li * 8];
                    memcpy(o + 0, L + 0x00 / 4, 12);         /* ambient */
                    memcpy(o + 4, L + 0x0C / 4, 12);         /* diffuse */
                    memcpy(o + 8, L + 0x18 / 4, 12);         /* specular */
                    memcpy(o + 12, L + 0x28 / 4, 12);        /* infinite half vector */
                    memcpy(o + 16, L + 0x34 / 4, 12);        /* infinite direction */
                    memcpy(o + 20, L + 0x5C / 4, 12);        /* local position */
                    memcpy(o + 23, L + 0x24 / 4, 4);         /* local range */
                    memcpy(o + 24, L + 0x68 / 4, 12);        /* local attenuation */
                    memcpy(o + 28, L + 0x4C / 4, 16);        /* spot direction */
                }
            }
        }
        /* Fixed-function texgen (gl_vsh_ff.c, after xemu): REFLECTION_MAP on S/T/R
         * (SET_TEXGEN_S/T/R 0x03C0 + stage * 0x10) and the texture matrix after it
         * (SET_TEXTURE_MATRIX_ENABLE). THPS 2X's skaters sample a cube map with
         * it. Other texgen modes stay unimplemented: the vertex's own coordinates.
         * The ring is not cleared, so tgen is always written. */
        vb->tgen = 0;
        if (b->xform == 1 && (s_ffp_texgen == 1 || (s_ffp_texgen == 2 && !((s_frame / s_ffp_texgen_period) & 1u)))) {
            int st, c;
            for (st = 0; st < 4; st++) {
                for (c = 0; c < 3; c++)
                    if (r[(0x03C0 + st * 0x10 + c * 4) / 4] == 0x8512)
                        vb->tgen |= 1 << (st * 4 + c);
                if (r[(0x0420 + st * 4) / 4]) {
                    vb->tgen |= 1 << (16 + st);
                    memcpy(vb->tmat[st * 4], &r[(0x06C0 + st * 0x40) / 4], 64);
                }
            }
        }
        ub[0].buffer = s_f->ring;
        ub[0].offset = vat;
        ub[0].range = sizeof(VsBlock);

        {
            VkDeviceSize fat = ring_alloc(sizeof(FsBlock), s_ubo_align, &map);
            fb = (FsBlock *)map;
            if (!fb) return;
            memset(fb, 0, sizeof *fb);
            for (i = 0; i < 4; i++) {
                fb->tscale[i][0] = tscale[i][0];
                fb->tscale[i][1] = tscale[i][1];
            }
            for (i = 0; i < 8; i++) {
                argb_vec4(r[0xA60 / 4 + i], fb->c0[i]);
                argb_vec4(r[0xA80 / 4 + i], fb->c1[i]);
            }
            argb_vec4(r[0x1E20 / 4], fb->fc0);
            argb_vec4(r[0x1E24 / 4], fb->fc1);
            {
                uint32_t fc = r[0x2A8 / 4];
                fb->fog[0] = byte_f(fc, 0); fb->fog[1] = byte_f(fc, 8);
                fb->fog[2] = byte_f(fc, 16); fb->fog[3] = byte_f(fc, 24);
            }
            fb->afunc = r[0x300 / 4] ? (int32_t)r[0x33C / 4] : 0x207;
            fb->aref = (float)(r[0x340 / 4] & 0xFF);
            nv2a_fog_params(r, b->xform, fb->fogp);
            ub[1].buffer = s_f->ring;
            ub[1].offset = fat;
            ub[1].range = sizeof(FsBlock);
        }
    }
    if (!upload_vertices(b))
        return;
    {
        VkDeviceSize iat;
        if (b->vertex_count <= 65536) {
            uint16_t *o;
            iat = ring_alloc((VkDeviceSize)n * 2, 4, &map);
            o = (uint16_t *)map;
            if (!o) return;
            for (i = 0; i < n; i++)
                o[i] = (uint16_t)idx[i];
            vkCmdBindIndexBuffer(s_cb, s_f->ring, iat, VK_INDEX_TYPE_UINT16);
        } else {
            iat = ring_alloc((VkDeviceSize)n * 4, 4, &map);
            if (!map) return;
            memcpy(map, idx, (size_t)n * 4);
            vkCmdBindIndexBuffer(s_cb, s_f->ring, iat, VK_INDEX_TYPE_UINT32);
        }
    }
    {
        /* Constants last: repeats append theirs right behind (pend_append). */
        VkDeviceSize cat = ring_alloc(VK_CONST_SIZE, s_ubo_align, &map), range;
        if (!map) return;
        memcpy(map, b->vp_consts, VK_CONST_SIZE);
        range = (VkDeviceSize)VK_CONST_SIZE * VK_INST_MAX;
        if (range > s_ubo_range) range = s_ubo_range;
        if (cat + range > RING_BYTES) range = RING_BYTES - cat;
        range = range / VK_CONST_SIZE * VK_CONST_SIZE;
        inst_room = (uint32_t)(range / VK_CONST_SIZE);
        ub[2].buffer = s_f->ring;
        ub[2].offset = cat;
        ub[2].range = range;
    }
    memset(wr, 0, sizeof wr);
    for (i = 0; i < 7; i++) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        if (i < 2 || i == 6) {
            wr[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            wr[i].pBufferInfo = &ub[i == 6 ? 2 : i];
        } else {
            wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[i].pImageInfo = &img[i - 2];
        }
    }
    p_push_desc(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 7, wr);

    VT("  vkCmdDrawIndexed %u\n", n);
    /* The constants were the ring's last allocation, so a repeat's set
     * lands right behind them (pend_append). */
    if (pend_eligible(b) && inst_room > 1) {
        pend_hold(b, n, ub[2].offset, inst_room);
    } else {
        vkCmdDrawIndexed(s_cb, n, 1, 0, 0, 0);
        s_st_calls++;
    }
    if (s_vtrace > 0)
        s_vtrace--;
}

/* A clear of some colour channels only. vkCmdClearAttachments has no write
 * mask, so this draws one triangle over the rect with the channels as the
 * pipeline's colour write mask. Burnout 3 clears its reflection cube faces
 * with A alone (0x83): the alpha is a highlight mask that only the light
 * fittings write, and without the clear it stayed 1 over every wall. */
static VkPipelineLayout s_clr_layout;
static VkShaderModule s_clr_vs, s_clr_fs;
static VkPipeline s_clr_pipe[16][2][3];         /* [write mask][depth attached][1 / 2 / 4 samples] */

static VkPipeline clear_pipe(uint32_t vkmask, int depth, uint32_t samples)
{
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    VkFormat cfmt = VK_FORMAT_B8G8R8A8_UNORM;
    static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipeline *pp = &s_clr_pipe[vkmask & 15][depth != 0][samples >= 4 ? 2 : samples >= 2 ? 1 : 0];

    if (*pp)
        return *pp;
    if (!s_clr_layout) {
        VkPushConstantRange pc = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
        VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pc;
        if (vkCreatePipelineLayout(s_dev, &pli, NULL, &s_clr_layout) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        s_clr_vs = compile(GLSLANG_STAGE_VERTEX,
            "#version 450\n"
            "void main() {\n"
            "    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
            "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n");
        s_clr_fs = compile(GLSLANG_STAGE_FRAGMENT,
            "#version 450\n"
            "layout(push_constant) uniform P { vec4 c; };\n"
            "layout(location = 0) out vec4 o;\n"
            "void main() { o = c; }\n");
    }
    if (!s_clr_vs || !s_clr_fs)
        return VK_NULL_HANDLE;
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = s_clr_vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = s_clr_fs;
    st[1].pName = "main";
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = samples ? (VkSampleCountFlagBits)samples : VK_SAMPLE_COUNT_1_BIT;
    memset(&ba, 0, sizeof ba);
    ba.colorWriteMask = vkmask & 15;
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    dy.dynamicStateCount = (uint32_t)(sizeof dyn / sizeof dyn[0]);
    dy.pDynamicStates = dyn;
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &cfmt;
    if (depth) {
        rci.depthAttachmentFormat = s_depth_fmt;
        rci.stencilAttachmentFormat = s_depth_fmt;
    }
    ci.pNext = &rci;
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = s_clr_layout;
    if (vkCreateGraphicsPipelines(s_dev, s_pcache, 1, &ci, NULL, pp) != VK_SUCCESS) {
        LOGE("clear pipeline creation failed\n");
        *pp = VK_NULL_HANDLE;
    }
    return *pp;
}

static void vk_clear(const Nv2aSurface *sf, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    VkSurf *s;
    VkDepthBuf *d;
    VkClearAttachment ca[2];
    VkClearRect rect;
    uint32_t nca = 0;

    if (!ready())
        return;
    pend_flush();
    s = target(sf, rs ? rs->zeta_va : 0, &d);
    if (!s)
        return;
    if (d && d->ms != s->ms)
        d = NULL;                            /* as begin_rendering attaches it */
    memset(&rect, 0, sizeof rect);
    rect.layerCount = 1;
    rect.rect.extent.width = s->pw;
    rect.rect.extent.height = s->ph;
    if (s_regs) {
        uint32_t hz = s_regs[0x1D98 / 4], vt = s_regs[0x1D9C / 4];
        uint32_t x0 = (hz & 0xFFFF) * s->aa_sx, x1 = ((hz >> 16) + 1) * s->aa_sx;
        uint32_t y0 = (vt & 0xFFFF) * s->aa_sy, y1 = ((vt >> 16) + 1) * s->aa_sy;
        if (x1 > s->w) x1 = s->w;
        if (y1 > s->h) y1 = s->h;
        if (x1 > x0 && y1 > y0) {
            uint32_t px0 = to_stored(x0, s->pw, s->w), py0 = to_stored(y0, s->ph, s->h);
            rect.rect.offset.x = (int32_t)px0;
            rect.rect.offset.y = (int32_t)py0;
            rect.rect.extent.width = to_stored(x1, s->pw, s->w) - px0;
            rect.rect.extent.height = to_stored(y1, s->ph, s->h) - py0;
        }
    }
    if (d && rect.rect.extent.width > d->pw - (uint32_t)rect.rect.offset.x)
        rect.rect.extent.width = d->pw - (uint32_t)rect.rect.offset.x;
    if (d && rect.rect.extent.height > d->ph - (uint32_t)rect.rect.offset.y)
        rect.rect.extent.height = d->ph - (uint32_t)rect.rect.offset.y;
    memset(ca, 0, sizeof ca);
    if ((flags & 0xF0) == 0xF0) {
        ca[nca].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca[nca].colorAttachment = 0;
        argb_vec4(argb, ca[nca].clearValue.color.float32);
        nca++;
    }
    if (d && (flags & 0x3)) {
        uint32_t zmax = s_regs ? zmax_of(s_regs) : 0xFFFFFFu;
        uint32_t z = zmax == 0xFFFFu ? (zstencil & 0xFFFF) : (zstencil >> 8);
        ca[nca].aspectMask = ((flags & 1) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                             ((flags & 2) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        ca[nca].clearValue.depthStencil.depth = (float)((double)z / (double)zmax);
        ca[nca].clearValue.depthStencil.stencil = zstencil & 0xFF;
        nca++;
    }
    if ((flags & 0xF0) && (flags & 0xF0) != 0xF0) {
        /* CLEAR_SURFACE R 0x10, G 0x20, B 0x40, A 0x80: the same order as
         * VkColorComponentFlags. */
        VkPipeline cp = clear_pipe((flags >> 4) & 0xF, d != NULL, s->ms);
        if (cp) {
            float c[4];
            argb_vec4(argb, c);
            begin_rendering(s, d);
            vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, cp);
            vkCmdSetScissor(s_cb, 0, 1, &rect.rect);
            s_sc_cur = rect.rect;
            vkCmdPushConstants(s_cb, s_clr_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, c);
            vkCmdDraw(s_cb, 3, 1, 0, 0);
            s_pipe_foreign = 1;                 /* the next draw rebinds its own */
            s_dyn_valid = 0;                    /* and resets its dynamic state */
            s_cb_gen++;                         /* incl. vertex input (cached per gen) */
            s->dirty = 1;
            s->gen++;
        }
    }
    if (!nca)
        return;
    VT("clear %08X %ux%u flags %X rect %d,%d %ux%u depth %p\n", s->va, s->pw, s->ph, flags,
       rect.rect.offset.x, rect.rect.offset.y, rect.rect.extent.width, rect.rect.extent.height, (void *)d);
    begin_rendering(s, d);
    vkCmdClearAttachments(s_cb, nca, ca, 1, &rect);
    VT("  cleared\n");
    s->dirty = 1;
    s->gen++;
}

static void dump_bmp(const char *path, const uint8_t *bgra, uint32_t w, uint32_t h)
{
    extern FILE *xbox_fopen_utf8(const char *path, const char *mode);
    FILE *f = xbox_fopen_utf8(path, "wb");   /* UTF-8 path (RECOMP_SHOT_PREFIX) */
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t row = w * 3, pad = (4 - (row & 3)) & 3, size = 54 + (row + pad) * h, y, x;
    if (!f)
        return;
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    for (y = h; y-- > 0; ) {
        const uint8_t *p = bgra + (size_t)y * w * 4;
        for (x = 0; x < w; x++)
            fwrite(p + x * 4, 1, 3, f);
        fwrite("\0\0\0", 1, pad, f);
    }
    fclose(f);
}

/* RECOMP_GL_DUMP: read the surface back (waits for the GPU). */
static void dump_surface(VkSurf *s, const char *path)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkBuffer buf;
    VkDeviceMemory mem;
    VkMemoryRequirements rq;
    VkBufferImageCopy rg;
    void *ptr;

    bci.size = (VkDeviceSize)s->pw * s->ph * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(s_dev, &bci, NULL, &buf) != VK_SUCCESS)
        return;
    vkGetBufferMemoryRequirements(s_dev, buf, &rq);
    mem = alloc_mem(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mem) { vkDestroyBuffer(s_dev, buf, NULL); return; }
    vkBindBufferMemory(s_dev, buf, mem, 0);
    end_rendering();
    barrier_all();
    memset(&rg, 0, sizeof rg);
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = s->pw;
    rg.imageExtent.height = s->ph;
    rg.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s_cb, s->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &rg);
    flush_frame_wait();
    if (vkMapMemory(s_dev, mem, 0, bci.size, 0, &ptr) == VK_SUCCESS) {
        dump_bmp(path, (const uint8_t *)ptr, s->pw, s->ph);
        /* RECOMP_VK_DUMP_RAW=1: also <path>.raw, the stored BGRA bytes with
         * alpha (the BMP drops it). */
        if (getenv("RECOMP_VK_DUMP_RAW")) {
            char rp[300];
            FILE *f;
            snprintf(rp, sizeof rp, "%s.raw", path);
            if ((f = fopen(rp, "wb")) != NULL) {
                fwrite(ptr, 4, (size_t)s->pw * s->ph, f);
                fclose(f);
            }
        }
        vkUnmapMemory(s_dev, mem);
    }
    vkDestroyBuffer(s_dev, buf, NULL);
    vkFreeMemory(s_dev, mem, NULL);
}

/* ── Present pass (FXAA) ──────────────────────────────────────────────
 * RECOMP_VK_FXAA=0: the presented surface is blitted to the swapchain as
 * before. Otherwise one full-screen draw scales it with FXAA 3.11 (quality
 * preset, 8 search steps) worked out in the surface's texels: a fixed GPU
 * cost per frame, no draws or CPU work added. */
static int s_fxaa = -1;
static VkDescriptorSetLayout s_pp_dsl;
static VkPipelineLayout      s_pp_layout;
static VkSampler             s_pp_sampler;
static struct { VkFormat fmt; VkPipeline pipe; } s_pp[2];

static const char s_pp_vs[] =
    "#version 450\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char s_pp_fs[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D src;\n"
    "layout(push_constant) uniform P { vec4 rect; vec4 rcp; } pc;\n"
    "layout(location = 0) out vec4 o;\n"
    "float L(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
    "float Lat(vec2 uv) { return L(textureLod(src, uv, 0.0).rgb); }\n"
    "#define LO(x, y) L(textureLodOffset(src, uv, 0.0, ivec2(x, y)).rgb)\n"
    "const float Q[8] = float[](1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 4.0, 8.0);\n"
    "void main() {\n"
    "    vec2 uv = (gl_FragCoord.xy - pc.rect.xy) / pc.rect.zw;\n"
    "    vec2 r = pc.rcp.xy;\n"
    "    vec4 cM = textureLod(src, uv, 0.0);\n"
    "    float lM = L(cM.rgb), lD = LO(0, 1), lU = LO(0, -1), lL = LO(-1, 0), lR = LO(1, 0);\n"
    "    float mx = max(lM, max(max(lD, lU), max(lL, lR)));\n"
    "    float mn = min(lM, min(min(lD, lU), min(lL, lR)));\n"
    "    float range = mx - mn;\n"
    "    if (range < max(0.0312, mx * 0.125)) { o = vec4(cM.rgb, 1.0); return; }\n"
    "    float lDL = LO(-1, 1), lUR = LO(1, -1), lUL = LO(-1, -1), lDR = LO(1, 1);\n"
    "    float lDU = lD + lU, lLR = lL + lR;\n"
    "    float lLc = lDL + lUL, lDc = lDL + lDR, lRc = lDR + lUR, lUc = lUR + lUL;\n"
    "    float eH = abs(-2.0 * lL + lLc) + abs(-2.0 * lM + lDU) * 2.0 + abs(-2.0 * lR + lRc);\n"
    "    float eV = abs(-2.0 * lU + lUc) + abs(-2.0 * lM + lLR) * 2.0 + abs(-2.0 * lD + lDc);\n"
    "    bool hor = eH >= eV;\n"
    "    float l1 = hor ? lU : lL, l2 = hor ? lD : lR;\n"
    "    float g1 = l1 - lM, g2 = l2 - lM;\n"
    "    bool steep1 = abs(g1) >= abs(g2);\n"
    "    float gs = 0.25 * max(abs(g1), abs(g2));\n"
    "    float step = hor ? r.y : r.x, avg;\n"
    "    if (steep1) { step = -step; avg = 0.5 * (l1 + lM); } else avg = 0.5 * (l2 + lM);\n"
    "    vec2 cur = uv;\n"
    "    if (hor) cur.y += step * 0.5; else cur.x += step * 0.5;\n"
    "    vec2 off = hor ? vec2(r.x, 0.0) : vec2(0.0, r.y);\n"
    "    vec2 uv1 = cur - off, uv2 = cur + off;\n"
    "    float e1 = Lat(uv1) - avg, e2 = Lat(uv2) - avg;\n"
    "    bool r1 = abs(e1) >= gs, r2 = abs(e2) >= gs;\n"
    "    if (!r1) uv1 -= off;\n"
    "    if (!r2) uv2 += off;\n"
    "    for (int i = 2; i < 8 && !(r1 && r2); i++) {\n"
    "        if (!r1) e1 = Lat(uv1) - avg;\n"
    "        if (!r2) e2 = Lat(uv2) - avg;\n"
    "        r1 = abs(e1) >= gs; r2 = abs(e2) >= gs;\n"
    "        if (!r1) uv1 -= off * Q[i];\n"
    "        if (!r2) uv2 += off * Q[i];\n"
    "    }\n"
    "    float d1 = hor ? uv.x - uv1.x : uv.y - uv1.y;\n"
    "    float d2 = hor ? uv2.x - uv.x : uv2.y - uv.y;\n"
    "    bool dir1 = d1 < d2;\n"
    "    float px = -min(d1, d2) / (d1 + d2) + 0.5;\n"
    "    bool ok = ((dir1 ? e1 : e2) < 0.0) != (lM < avg);\n"
    "    float fin = ok ? px : 0.0;\n"
    "    float la = (1.0 / 12.0) * (2.0 * (lDU + lLR) + lLc + lRc);\n"
    "    float s1 = clamp(abs(la - lM) / range, 0.0, 1.0);\n"
    "    float s2 = (-2.0 * s1 + 3.0) * s1 * s1;\n"
    "    fin = max(fin, s2 * s2 * 0.75);\n"
    "    if (hor) uv.y += fin * step; else uv.x += fin * step;\n"
    "    o = vec4(textureLod(src, uv, 0.0).rgb, 1.0);\n"
    "}\n";

/* ANTIALIASING (nv2a_vk_set_antialiasing): the port's mode, RECOMP_VK_AA wins */
static volatile int s_aa_opt = NV2A_VK_AA_FXAA;
static int s_aa_env = -2;                       /* -2 not read, -1 not set, else forced */

void nv2a_vk_set_antialiasing(int mode)
{
    s_aa_opt = mode >= NV2A_VK_AA_OFF && mode <= NV2A_VK_AA_MSAA4 ? mode : NV2A_VK_AA_FXAA;
}

static int aa_mode(void)
{
    if (s_aa_env == -2) {
        static const char *const n[5] = { "off", "fxaa", "smaa", "msaa2", "msaa4" };
        const char *e = getenv("RECOMP_VK_AA");
        int i;
        s_aa_env = -1;
        for (i = 0; e && i < 5; i++)
            if (!strcmp(e, n[i]))
                s_aa_env = i;
    }
    return s_aa_env >= 0 ? s_aa_env : s_aa_opt;
}

/* SHADERS (nv2a_vk_set_shader_preset, shader_pass.c): RECOMP_SHADER_PRESET
 * read at nv2a_vk_install wins over the port's calls. */
static int s_shader_env;

void nv2a_vk_set_shader_preset(const char *path)
{
#if !defined(__SWITCH__)
    if (!s_shader_env)
        sp_set_preset(path);
#else
    (void)path;
#endif
}

void nv2a_vk_set_shader_param(const char *name, float value)
{
#if !defined(__SWITCH__)
    sp_set_param(name, value);
#else
    (void)name; (void)value;
#endif
}

int nv2a_vk_shader_status(void)
{
#if !defined(__SWITCH__)
    return sp_status();
#else
    return NV2A_VK_SHADER_NO_LIBRARY;
#endif
}

/* Records the shader chain from pres into the swapchain image dst, the game
 * rectangle as its viewport (the chain clears the rest to black), dst left
 * in PRESENT_SRC. 0 = nothing usable recorded (present without it). */
static int present_shader(VkSurf *pres, VkImage dst, int dx, int dy, int dw, int dh)
{
#if !defined(__SWITCH__)
    static int last_dx = -1, last_dy, last_dw, last_dh;
    static uint32_t last_pw, last_ph;
    static uint64_t last_ns;
    VkImageMemoryBarrier b[2];
    uint64_t now = hz_now();
    uint32_t dt_ms = last_ns ? (uint32_t)((now - last_ns) / 1000000u) : 0;
    int same = dx == last_dx && dy == last_dy && dw == last_dw && dh == last_dh
               && pres->pw == last_pw && pres->ph == last_ph;
    int ok;

    last_ns = now;
    last_dx = dx; last_dy = dy; last_dw = dw; last_dh = dh;
    last_pw = pres->pw; last_ph = pres->ph;
    end_rendering();
    memset(b, 0, sizeof b);
    b[0].sType = b[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b[0].srcQueueFamilyIndex = b[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b[0].subresourceRange.levelCount = 1;
    b[0].subresourceRange.layerCount = 1;
    b[1] = b[0];
    /* the surface: GENERAL -> SHADER_READ_ONLY for the chain (its history
     * copy reads it as a transfer source too) */
    b[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    b[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[0].image = pres->image;
    b[1].srcAccessMask = 0;
    b[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b[1].image = dst;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 2, b);
    ok = sp_frame(s_cb, s_frame, pres->image, VK_FORMAT_B8G8R8A8_UNORM, pres->pw, pres->ph,
                  dst, s_sc_format, s_sc_extent.width, s_sc_extent.height, dx, dy, dw, dh, same,
                  dt_ms > 0 && dt_ms < 1000 ? 1000.0f / (float)dt_ms : 60.0f, dt_ms);
    /* back: the surface to GENERAL, dst to PRESENT_SRC (from whatever the
     * chain left it in: COLOR_ATTACHMENT) */
    b[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    b[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b[1].dstAccessMask = 0;
    b[1].oldLayout = ok ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, ok ? 2 : 1, b);
    s_dyn_valid = 0;
    return ok;
#else
    (void)pres; (void)dst; (void)dx; (void)dy; (void)dw; (void)dh;
    return 0;
#endif
}

static int fxaa_on(void)
{
    if (s_fxaa < 0) {
        const char *e = getenv("RECOMP_VK_FXAA");
        s_fxaa = !(e && *e == '0');
    }
    return s_fxaa && nv2a_vk_fxaa && aa_mode() == NV2A_VK_AA_FXAA;
}

static VkPipeline pp_pipe(VkFormat fmt)
{
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkShaderModule vs, fs;
    VkPipeline pipe = VK_NULL_HANDLE;
    int i;

    for (i = 0; i < 2; i++)
        if (s_pp[i].pipe && s_pp[i].fmt == fmt)
            return s_pp[i].pipe;
    if (!s_pp_layout) {
        VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                           VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
        VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        lci.bindingCount = 1;
        lci.pBindings = &b;
        if (vkCreateDescriptorSetLayout(s_dev, &lci, NULL, &s_pp_dsl) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &s_pp_dsl;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(s_dev, &pli, NULL, &s_pp_layout) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(s_dev, &sci, NULL, &s_pp_sampler) != VK_SUCCESS)
            return VK_NULL_HANDLE;
    }
    vs = compile(GLSLANG_STAGE_VERTEX, s_pp_vs);
    fs = compile(GLSLANG_STAGE_FRAGMENT, s_pp_fs);
    if (vs && fs) {
        memset(st, 0, sizeof st);
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vs;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = fs;
        st[1].pName = "main";
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        vp.viewportCount = vp.scissorCount = 1;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1.0f;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        memset(&ba, 0, sizeof ba);
        ba.colorWriteMask = 0xF;
        cb.attachmentCount = 1;
        cb.pAttachments = &ba;
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        rci.colorAttachmentCount = 1;
        rci.pColorAttachmentFormats = &fmt;
        ci.pNext = &rci;
        ci.stageCount = 2;
        ci.pStages = st;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = s_pp_layout;
        if (vkCreateGraphicsPipelines(s_dev, s_pcache, 1, &ci, NULL, &pipe) != VK_SUCCESS)
            pipe = VK_NULL_HANDLE;
    }
    if (vs) vkDestroyShaderModule(s_dev, vs, NULL);
    if (fs) vkDestroyShaderModule(s_dev, fs, NULL);
    if (!pipe) {
        LOGE("FXAA pipeline failed; presenting with a blit\n");
        s_fxaa = 0;
        return VK_NULL_HANDLE;
    }
    i = s_pp[0].pipe ? 1 : 0;
    if (s_pp[i].pipe)
        vkDestroyPipeline(s_dev, s_pp[i].pipe, NULL);   /* a third format: never in practice */
    s_pp[i].fmt = fmt;
    s_pp[i].pipe = pipe;
    return pipe;
}

/* Surface s, FXAA'd and scaled into the rectangle (dx, dy, dw, dh) of dst
 * (cleared to black around it), left in layout `final`. Recorded outside
 * rendering; 0 = not recorded (use the blit). */
static int present_fxaa(VkSurf *s, VkImage dst, VkImageView dview, VkFormat fmt, VkExtent2D ext,
                        int dx, int dy, int dw, int dh, VkImageLayout final,
                        VkPipelineStageFlags final_stage)
{
    VkPipeline pipe = pp_pipe(fmt);
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkViewport vp = { (float)dx, (float)dy, (float)dw, (float)dh, 0.0f, 1.0f };
    VkRect2D sc = { { dx, dy }, { (uint32_t)dw, (uint32_t)dh } };
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    float pc[8];

    if (!pipe || !dview)
        return 0;
    end_rendering();
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = dst;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    ca.imageView = dview;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color.float32[3] = 1.0f;
    ri.renderArea.extent = ext;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    p_begin_rendering(s_cb, &ri);
    vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdSetViewport(s_cb, 0, 1, &vp);
    vkCmdSetScissor(s_cb, 0, 1, &sc);
    ii.sampler = s_pp_sampler;
    ii.imageView = s->view;
    ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    p_push_desc(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pp_layout, 0, 1, &wr);
    pc[0] = (float)dx; pc[1] = (float)dy; pc[2] = (float)dw; pc[3] = (float)dh;
    pc[4] = 1.0f / (float)s->pw; pc[5] = 1.0f / (float)s->ph; pc[6] = pc[7] = 0.0f;
    vkCmdPushConstants(s_cb, s_pp_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(s_cb, 3, 1, 0, 0);
    p_end_rendering(s_cb);
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = final == VK_IMAGE_LAYOUT_GENERAL ? VK_ACCESS_MEMORY_READ_BIT : 0;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = final;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, final_stage,
                         0, 0, NULL, 0, NULL, 1, &b);
    s_dyn_valid = 0;
    return 1;
}

/* RECOMP_GL_DUMP while headless: the FXAA'd frame too, at the surface's
 * size (<prefix>NNNNN_fxaa.bmp), to check the pass without a window. */
static void dump_fxaa(VkSurf *s, const char *path)
{
    static VkSurf t;
    if (t.image && (t.pw != s->pw || t.ph != s->ph)) {
        garbage_add(t.image, t.view, t.mem);
        memset(&t, 0, sizeof t);
    }
    if (!t.image) {
        t.pw = s->pw; t.ph = s->ph;
        if (!make_image(t.pw, t.ph, VK_FORMAT_B8G8R8A8_UNORM,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, &t.image, &t.view, &t.mem)) {
            memset(&t, 0, sizeof t);
            return;
        }
    }
    {
        VkExtent2D ext = { t.pw, t.ph };
        if (present_fxaa(s, t.image, t.view, VK_FORMAT_B8G8R8A8_UNORM, ext, 0, 0,
                         (int)t.pw, (int)t.ph, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT))
            dump_surface(&t, path);
    }
}

/* ── SMAA (ANTIALIASING = SMAA) ───────────────────────────────────────
 * The reference implementation (third_party/smaa: iryoku/smaa 71c806a, MIT,
 * Jimenez et al.; NOTICE), preset HIGH, luma edges, no predication / temporal
 * parts. Three full-screen passes at the presented surface's own size, before
 * it is scaled into the window: edges (RG8) -> blending weights (RGBA8, with
 * the area and search look-up textures) -> neighbourhood blending into a
 * BGRA8 image the present then blits instead of the surface. Screenshots and
 * dumps stay the surface itself. The shader text is SMAA.hlsl in its GLSL 4
 * mode (smaa_hlsl.h, made by CMake) behind a small header per pass; compiled
 * once and kept in the SPIR-V cache like every shader. Any failure turns
 * SMAA off with a log line (the present goes on with the plain blit). */
#include "smaa_hlsl.h"
#include "third_party/smaa/AreaTex.h"
#include "third_party/smaa/SearchTex.h"

static VkSurf s_smaa_tex[2];                    /* area (RG8 160x560), search (R8 64x16) */
static VkSurf s_smaa_img[3];                    /* edges, weights, result: at the surface's size */
static VkDescriptorSetLayout s_smaa_dsl;
static VkPipelineLayout      s_smaa_layout;
static VkSampler             s_smaa_point;
static VkPipeline            s_smaa_pipe[3];
static int                   s_smaa_bad;

static const VkFormat s_smaa_fmt[3] = { VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM };

/* pass p's vertex and fragment shader around SMAA.hlsl */
static char *smaa_source(int p, int fragment)
{
    static const char *const vs_main[3] = {
        "layout(location = 0) out vec2 v_tc;\n"
        "layout(location = 1) out vec4 v_off[3];\n"
        "void main() {\n"
        "    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "    v_tc = p;\n"
        "    SMAAEdgeDetectionVS(v_tc, v_off);\n"
        "}\n",
        "layout(location = 0) out vec2 v_tc;\n"
        "layout(location = 1) out vec2 v_pix;\n"
        "layout(location = 2) out vec4 v_off[3];\n"
        "void main() {\n"
        "    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "    v_tc = p;\n"
        "    SMAABlendingWeightCalculationVS(v_tc, v_pix, v_off);\n"
        "}\n",
        "layout(location = 0) out vec2 v_tc;\n"
        "layout(location = 1) out vec4 v_off;\n"
        "void main() {\n"
        "    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "    v_tc = p;\n"
        "    SMAANeighborhoodBlendingVS(v_tc, v_off);\n"
        "}\n",
    };
    static const char *const fs_main[3] = {
        "layout(set = 0, binding = 0) uniform sampler2D colorTex;\n"
        "layout(location = 0) in vec2 v_tc;\n"
        "layout(location = 1) in vec4 v_off[3];\n"
        "layout(location = 0) out vec4 o;\n"
        "void main() { o = vec4(SMAALumaEdgeDetectionPS(v_tc, v_off, colorTex), 0.0, 0.0); }\n",
        "layout(set = 0, binding = 0) uniform sampler2D edgesTex;\n"
        "layout(set = 0, binding = 1) uniform sampler2D areaTex;\n"
        "layout(set = 0, binding = 2) uniform sampler2D searchTex;\n"
        "layout(location = 0) in vec2 v_tc;\n"
        "layout(location = 1) in vec2 v_pix;\n"
        "layout(location = 2) in vec4 v_off[3];\n"
        "layout(location = 0) out vec4 o;\n"
        "void main() { o = SMAABlendingWeightCalculationPS(v_tc, v_pix, v_off, edgesTex, areaTex, searchTex,"
        " vec4(0.0)); }\n",
        "layout(set = 0, binding = 0) uniform sampler2D colorTex;\n"
        "layout(set = 0, binding = 1) uniform sampler2D blendTex;\n"
        "layout(location = 0) in vec2 v_tc;\n"
        "layout(location = 1) in vec4 v_off;\n"
        "layout(location = 0) out vec4 o;\n"
        "void main() { o = SMAANeighborhoodBlendingPS(v_tc, v_off, colorTex, blendTex); }\n",
    };
    const char *head = fragment
        ? "#version 450\n"
          "layout(push_constant) uniform PC { vec4 metrics; } pc;\n"
          "#define SMAA_RT_METRICS pc.metrics\n#define SMAA_GLSL_4\n#define SMAA_PRESET_HIGH\n"
          "#define SMAA_INCLUDE_VS 0\n"
        : "#version 450\n"
          "layout(push_constant) uniform PC { vec4 metrics; } pc;\n"
          "#define SMAA_RT_METRICS pc.metrics\n#define SMAA_GLSL_4\n#define SMAA_PRESET_HIGH\n"
          "#define SMAA_INCLUDE_PS 0\n";
    const char *tail = fragment ? fs_main[p] : vs_main[p];
    size_t n = strlen(head) + sizeof s_smaa_hlsl + strlen(tail) + 2;
    char *src = (char *)malloc(n);
    if (src)
        snprintf(src, n, "%s%s\n%s", head, s_smaa_hlsl, tail);
    return src;
}

static int smaa_pipelines(void)
{
    int p;
    if (s_smaa_pipe[2])
        return 1;
    if (!s_smaa_layout) {
        VkDescriptorSetLayoutBinding b[3];
        VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
        VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        int i;
        for (i = 0; i < 3; i++) {
            b[i].binding = (uint32_t)i;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[i].pImmutableSamplers = NULL;
        }
        lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        lci.bindingCount = 3;
        lci.pBindings = b;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &s_smaa_dsl;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateDescriptorSetLayout(s_dev, &lci, NULL, &s_smaa_dsl) != VK_SUCCESS
            || vkCreatePipelineLayout(s_dev, &pli, NULL, &s_smaa_layout) != VK_SUCCESS
            || vkCreateSampler(s_dev, &sci, NULL, &s_smaa_point) != VK_SUCCESS)
            return 0;
    }
    for (p = 0; p < 3; p++) {
        VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        VkPipelineShaderStageCreateInfo st[2];
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        VkPipelineColorBlendAttachmentState ba;
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        char *vsrc = smaa_source(p, 0), *fsrc = smaa_source(p, 1);
        VkShaderModule vs = vsrc ? compile(GLSLANG_STAGE_VERTEX, vsrc) : VK_NULL_HANDLE;
        VkShaderModule fs = fsrc ? compile(GLSLANG_STAGE_FRAGMENT, fsrc) : VK_NULL_HANDLE;
        free(vsrc);
        free(fsrc);
        if (vs && fs) {
            memset(st, 0, sizeof st);
            st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            st[0].module = vs;
            st[0].pName = "main";
            st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            st[1].module = fs;
            st[1].pName = "main";
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            vp.viewportCount = vp.scissorCount = 1;
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.lineWidth = 1.0f;
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            memset(&ba, 0, sizeof ba);
            ba.colorWriteMask = 0xF;
            cb.attachmentCount = 1;
            cb.pAttachments = &ba;
            dy.dynamicStateCount = 2;
            dy.pDynamicStates = dyn;
            rci.colorAttachmentCount = 1;
            rci.pColorAttachmentFormats = &s_smaa_fmt[p];
            ci.pNext = &rci;
            ci.stageCount = 2;
            ci.pStages = st;
            ci.pVertexInputState = &vi;
            ci.pInputAssemblyState = &ia;
            ci.pViewportState = &vp;
            ci.pRasterizationState = &rs;
            ci.pMultisampleState = &ms;
            ci.pColorBlendState = &cb;
            ci.pDynamicState = &dy;
            ci.layout = s_smaa_layout;
            if (vkCreateGraphicsPipelines(s_dev, s_pcache, 1, &ci, NULL, &s_smaa_pipe[p]) != VK_SUCCESS)
                s_smaa_pipe[p] = VK_NULL_HANDLE;
        }
        if (vs) vkDestroyShaderModule(s_dev, vs, NULL);
        if (fs) vkDestroyShaderModule(s_dev, fs, NULL);
        if (!s_smaa_pipe[p])
            return 0;
    }
    return 1;
}

/* the area and search textures, uploaded once (a host-visible staging buffer
 * kept for the run: 180 KB) */
static int smaa_lookup_textures(void)
{
    static const struct { uint32_t w, h, bpp; VkFormat fmt; const unsigned char *bytes; } t[2] = {
        { AREATEX_WIDTH, AREATEX_HEIGHT, 2, VK_FORMAT_R8G8_UNORM, areaTexBytes },
        { SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1, VK_FORMAT_R8_UNORM, searchTexBytes },
    };
    static VkBuffer buf;
    static VkDeviceMemory mem;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements rq;
    VkDeviceSize off[2], total;
    void *map;
    int i;
    if (s_smaa_tex[1].image)
        return 1;
    off[0] = 0;
    off[1] = ((VkDeviceSize)t[0].w * t[0].h * t[0].bpp + 255) & ~(VkDeviceSize)255;
    total = off[1] + (VkDeviceSize)t[1].w * t[1].h * t[1].bpp;
    bci.size = total;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(s_dev, &bci, NULL, &buf) != VK_SUCCESS)
        return 0;
    vkGetBufferMemoryRequirements(s_dev, buf, &rq);
    if (!(mem = alloc_mem(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        || vkBindBufferMemory(s_dev, buf, mem, 0) != VK_SUCCESS
        || vkMapMemory(s_dev, mem, 0, total, 0, &map) != VK_SUCCESS)
        return 0;
    for (i = 0; i < 2; i++)
        memcpy((char *)map + off[i], t[i].bytes, (size_t)t[i].w * t[i].h * t[i].bpp);
    vkUnmapMemory(s_dev, mem);
    for (i = 0; i < 2; i++) {
        VkBufferImageCopy rg;
        VkSurf *x = &s_smaa_tex[i];
        if (!make_image(t[i].w, t[i].h, t[i].fmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, &x->image, &x->view, &x->mem))
            return 0;
        x->pw = t[i].w; x->ph = t[i].h;
        memset(&rg, 0, sizeof rg);
        rg.bufferOffset = off[i];
        rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rg.imageSubresource.layerCount = 1;
        rg.imageExtent.width = t[i].w;
        rg.imageExtent.height = t[i].h;
        rg.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(s_cb, buf, x->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    }
    barrier_all();
    return 1;
}

/* one SMAA pass into s_smaa_img[p]: the sampled inputs, the metrics */
static void smaa_pass(int p, VkImageView in0, VkImageView in1, VkImageView in2, uint32_t w, uint32_t h)
{
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { w, h } };
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet wr[3];
    VkImageView in[3] = { in0, in1, in2 };
    float pc[4] = { 1.0f / (float)w, 1.0f / (float)h, (float)w, (float)h };
    int i, n = 0;
    ca.imageView = s_smaa_img[p].view;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = p < 2 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;   /* edges / weights discard */
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea.extent.width = w;
    ri.renderArea.extent.height = h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    p_begin_rendering(s_cb, &ri);
    vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_smaa_pipe[p]);
    vkCmdSetViewport(s_cb, 0, 1, &vp);
    vkCmdSetScissor(s_cb, 0, 1, &sc);
    for (i = 0; i < 3; i++) {
        if (!in[i])
            continue;
        ii[n].sampler = p == 1 && i == 2 ? s_smaa_point : s_pp_sampler;   /* the search texture: point */
        ii[n].imageView = in[i];
        ii[n].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        memset(&wr[n], 0, sizeof wr[n]);
        wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[n].dstBinding = (uint32_t)i;
        wr[n].descriptorCount = 1;
        wr[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[n].pImageInfo = &ii[n];
        n++;
    }
    p_push_desc(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_smaa_layout, 0, (uint32_t)n, wr);
    vkCmdPushConstants(s_cb, s_smaa_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof pc, pc);
    vkCmdDraw(s_cb, 3, 1, 0, 0);
    p_end_rendering(s_cb);
    {
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
    }
}

/* Surface s through SMAA; the result (at s's size) to present instead, or
 * NULL (SMAA off after a failure: the plain present). Outside rendering. */
static VkSurf *smaa_run(VkSurf *s)
{
    int i;
    if (s_smaa_bad || !s || !s->view)
        return NULL;
    end_rendering();
    if ((!s_pp_sampler && !pp_pipe(VK_FORMAT_B8G8R8A8_UNORM))   /* the linear sampler lives there */
        || !smaa_pipelines() || !smaa_lookup_textures()) {
        LOGE("SMAA could not be set up -- presenting without it\n");
        s_smaa_bad = 1;
        return NULL;
    }
    for (i = 0; i < 3; i++) {
        VkSurf *x = &s_smaa_img[i];
        if (x->image && (x->pw != s->pw || x->ph != s->ph)) {
            garbage_add(x->image, x->view, x->mem);
            memset(x, 0, sizeof *x);
        }
        if (!x->image) {
            if (!make_image(s->pw, s->ph, s_smaa_fmt[i],
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, &x->image, &x->view, &x->mem)) {
                static uint32_t told_w, told_h;
                if (told_w != s->pw || told_h != s->ph)
                    LOGE("SMAA: %ux%u images failed to allocate -- presented without SMAA\n", s->pw, s->ph);
                told_w = s->pw; told_h = s->ph;
                memset(x, 0, sizeof *x);
                return NULL;
            }
            x->pw = s->pw; x->ph = s->ph;
        }
    }
    smaa_pass(0, s->view, VK_NULL_HANDLE, VK_NULL_HANDLE, s->pw, s->ph);
    smaa_pass(1, s_smaa_img[0].view, s_smaa_tex[0].view, s_smaa_tex[1].view, s->pw, s->ph);
    smaa_pass(2, s->view, s_smaa_img[1].view, VK_NULL_HANDLE, s->pw, s->ph);
    s_dyn_valid = 0;
    return &s_smaa_img[2];
}

/* ── MSAA (ANTIALIASING = MSAA 2X / 4X) ───────────────────────────────
 * The presented surface only (maintainer, 2026-10-26) and its depth: at the
 * flip it gets a multisampled image of the chosen count (ms_flip), filled
 * once from its own image by a full-screen draw (ms_seed) so a title that
 * does not clear sees what was there; from then on its passes draw into the
 * multisampled image and resolve into the surface's image at every pass end
 * (begin_rendering) -- textures made from it, readback, the RT watch,
 * screenshots and the present all read that one. Pipelines take the sample
 * count in their key. A count that fails to allocate (or that the device
 * lacks) goes back to the count that last worked, with a log line, and is
 * not tried again this run (nv2a_vk_antialiasing_fits). No VRAM estimate
 * (maintainer, 2026-10-26). Other render targets (shadows, reflections) stay
 * single-sampled; a depth buffer a title shares between the presented
 * surface and another target is kept apart per sample count. */
static VkPipeline s_ms_seed_pipe[2];            /* [2x, 4x] */

static void ms_seed(VkSurf *s)
{
    VkPipeline *pp = &s_ms_seed_pipe[s->ms >= 4];
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkViewport vp = { 0.0f, 0.0f, (float)s->pw, (float)s->ph, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { s->pw, s->ph } };
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    if (!*pp) {
        VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        VkPipelineShaderStageCreateInfo st[2];
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        VkPipelineColorBlendAttachmentState ba;
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
        VkShaderModule vs, fs;
        if (!s_pp_layout && !pp_pipe(fmt))      /* its layout and sampler */
            return;
        vs = compile(GLSLANG_STAGE_VERTEX, s_pp_vs);
        fs = compile(GLSLANG_STAGE_FRAGMENT,
            "#version 450\n"
            "layout(set = 0, binding = 0) uniform sampler2D src;\n"
            "layout(location = 0) out vec4 o;\n"
            "void main() { o = texelFetch(src, ivec2(gl_FragCoord.xy), 0); }\n");
        if (vs && fs) {
            memset(st, 0, sizeof st);
            st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            st[0].module = vs;
            st[0].pName = "main";
            st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            st[1].module = fs;
            st[1].pName = "main";
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            vps.viewportCount = vps.scissorCount = 1;
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.lineWidth = 1.0f;
            ms.rasterizationSamples = (VkSampleCountFlagBits)s->ms;
            memset(&ba, 0, sizeof ba);
            ba.colorWriteMask = 0xF;
            cb.attachmentCount = 1;
            cb.pAttachments = &ba;
            dy.dynamicStateCount = 2;
            dy.pDynamicStates = dyn;
            rci.colorAttachmentCount = 1;
            rci.pColorAttachmentFormats = &fmt;
            ci.pNext = &rci;
            ci.stageCount = 2;
            ci.pStages = st;
            ci.pVertexInputState = &vi;
            ci.pInputAssemblyState = &ia;
            ci.pViewportState = &vps;
            ci.pRasterizationState = &rs;
            ci.pMultisampleState = &ms;
            ci.pColorBlendState = &cb;
            ci.pDynamicState = &dy;
            ci.layout = s_pp_layout;
            if (vkCreateGraphicsPipelines(s_dev, s_pcache, 1, &ci, NULL, pp) != VK_SUCCESS)
                *pp = VK_NULL_HANDLE;
        }
        if (vs) vkDestroyShaderModule(s_dev, vs, NULL);
        if (fs) vkDestroyShaderModule(s_dev, fs, NULL);
        if (!*pp) {
            LOGE("MSAA seed pipeline failed -- the multisampled image starts empty\n");
            return;
        }
    }
    end_rendering();
    barrier_all();
    ca.imageView = s->ms_view;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea.extent.width = s->pw;
    ri.renderArea.extent.height = s->ph;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    p_begin_rendering(s_cb, &ri);
    vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, *pp);
    vkCmdSetViewport(s_cb, 0, 1, &vp);
    vkCmdSetScissor(s_cb, 0, 1, &sc);
    ii.sampler = s_pp_sampler;
    ii.imageView = s->view;
    ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    p_push_desc(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pp_layout, 0, 1, &wr);
    vkCmdDraw(s_cb, 3, 1, 0, 0);
    p_end_rendering(s_cb);
    barrier_all();
    s_dyn_valid = 0;
    s_pipe_foreign = 1;                         /* the next draw rebinds its own */
}

/* s gets a multisampled image of n samples (filled from its own image) */
static int ms_set(VkSurf *s, uint32_t n)
{
    ms_drop(s);
    if (!make_image_ms(s->pw, s->ph, n, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, &s->ms_image, &s->ms_view, &s->ms_mem)) {
        s->ms_image = VK_NULL_HANDLE; s->ms_view = VK_NULL_HANDLE; s->ms_mem = VK_NULL_HANDLE;
        return 0;
    }
    s->ms = n;
    ms_seed(s);
    return 1;
}

/* the sample count wanted now: the option, within the device's counts, never
 * one that failed this run (then the one that last worked) */
static uint32_t ms_want(void)
{
    int m = aa_mode();
    uint32_t n = m == NV2A_VK_AA_MSAA4 ? 4u : m == NV2A_VK_AA_MSAA2 ? 2u : 0u;
    if (n && !(s_ms_max & n)) {
        static uint32_t told;
        if (!(told & n))
            LOGE("MSAA %ux: the device has no such sample count -- off\n", n);
        told |= n;
        s_ms_bad |= n;
    }
    return n && (n & s_ms_bad) ? s_ms_good : n;
}

/* At the flip (outside rendering): MSAA for the presented surface s */
static void ms_flip(VkSurf *s)
{
    uint32_t n;
    int k;
    end_rendering();
    if (s_ms_fail) {                            /* a multisampled depth failed since the last flip */
        s_ms_bad |= s_ms_fail;
        LOGE("MSAA %ux: depth failed to allocate -- back to %s\n", s_ms_fail >= 4 ? 4u : 2u,
             s_ms_good == 4 ? "MSAA 4x" : s_ms_good == 2 ? "MSAA 2x" : "no MSAA");
        s_ms_fail = 0;
    }
    n = ms_want();
    for (k = 0; k < VK_MAX_SURF; k++) {
        VkSurf *x = &s_surf[k];
        if (x->image && x->ms && x->ms != n)
            ms_drop(x);                         /* another count, or off */
    }
    if (!n) {
        s_ms_good = 0;
        return;
    }
    if (!s || !s->image)
        return;
    if (s->ms != n) {
        if (!ms_set(s, n)) {
            s_ms_bad |= n;
            LOGE("MSAA %ux: %ux%u failed to allocate -- back to %s\n", n, s->pw, s->ph,
                 s_ms_good == 4 ? "MSAA 4x" : s_ms_good == 2 ? "MSAA 2x" : "no MSAA");
            return;                             /* the next flip takes s_ms_good */
        }
        LOGE("MSAA %ux: surface 0x%08X %ux%u\n", n, s->va, s->pw, s->ph);
    }
    s_ms_good = n;
}

int nv2a_vk_antialiasing_fits(int mode)
{
    uint32_t n = mode == NV2A_VK_AA_MSAA4 ? 4u : mode == NV2A_VK_AA_MSAA2 ? 2u : 0u;
    return !n || !s_state || ((s_ms_max & n) && !(s_ms_bad & n));    /* before the device: 1 */
}

static void vk_flip(void)
{
    static int dump_every = -1;
    static char dump_prefix[256];
    VkSurf *s = s_scan ? s_scan : s_last;
    VkSurf *pres = NULL;                /* what the window shows: s, or its SMAA copy */
    int shader = 0;                     /* a slang shader chain is in place (shader_pass.c) */
    uint32_t idx = 0;
    int have_image = 0;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT |
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    if (!ready())
        return;
    pend_flush();
    s_frame++;
    if (s_inst_alt)
        s_inst_on = s_frame & 1;
    {
        static int st = -1;
        static uint32_t st_frame;
        if (st < 0) {
            const char *e = getenv("RECOMP_DRAW_STATS");
            st = e && *e == '1';
        }
        if (st && s_frame - st_frame >= 300) {
            fprintf(stderr, "[vk] per frame: %u draws -> %u draw calls (%u instances appended)\n",
                    s_st_draws / (s_frame - st_frame), s_st_calls / (s_frame - st_frame),
                    s_st_inst / (s_frame - st_frame));
            s_st_draws = s_st_calls = s_st_inst = 0;
            st_frame = s_frame;
        }
    }
    {
        static uint64_t last_ns, window_ns;
        static int lines;
        uint64_t now = hz_now();
        if (last_ns) {
            uint64_t dt = now - last_ns;
            if (now - window_ns > 10000000000ull) { window_ns = now; lines = 0; }
            if (dt > 50000000ull && lines++ < 30)
                snprintf(s_hitch, sizeof s_hitch, "[hitch] frame %u: %.0f ms -- %u programs compiled (%.0f ms),"
                         " %u textures %.1f MB (%.0f ms), %u draws", s_frame, dt / 1e6,
                         s_hz_progs, s_hz_prog_ns / 1e6, s_hz_texs,
                         s_hz_tex_bytes / 1048576.0, s_hz_tex_ns / 1e6, s_hz_draws);
        }
        last_ns = now;
        s_hz_progs = s_hz_texs = s_hz_draws = 0;
        s_hz_prog_ns = s_hz_tex_ns = s_hz_tex_bytes = 0;
    }
    {
        /* [fps] every 5 s (v0.9.2: diagnostics for testers, e.g. the Steam
         * Deck): with RECOMP_FPS_LOG=1, or while the FPS counter is shown, so
         * a tester's log has it with the in-game option alone. The frame-time
         * split (game / translate / gpu wait / present, frame_time.c) is
         * counted only while this is on. */
        static int env = -1;
        static uint32_t last_frame;
        static uint64_t last;
        uint64_t now;
        int on;
        if (env < 0) {
            const char *e = getenv("RECOMP_FPS_LOG");
            env = e && *e == '1';
            last = hz_now();
        }
        static int was_on, fresh;
        on = env || hud_on();
        xbox_ft_want(on);
        if (on && !was_on)
            fresh = 1;                             /* (again) on: a new window */
        was_on = on;
        if (on) {
            /* Flip-to-flip intervals, whichever thread runs the executor (the
             * frame-time counter only sees flips on its owner thread): a
             * 0.25 ms histogram up to 64 ms, p50 / p99 and the counts of
             * doubled (< half the median) and dropped (> 1.5x) frames. */
            static uint32_t hist[256], n_int;
            static uint64_t prev;
            double dt;
            now = hz_now();
            if (fresh) {
                extern void xbox_vblank_report(double, char *, size_t);
                char vb[128], ft[256];
                fresh = 0;
                memset(hist, 0, sizeof hist);
                n_int = 0;
                prev = 0;
                last = now;
                last_frame = s_frame;
                xbox_vblank_report(0.0, vb, sizeof vb);   /* restart their counts too */
                xbox_ft_window_text(ft, sizeof ft);
            }
            if (prev) {
                uint64_t b = (now - prev) / 250000u;
                hist[b > 255 ? 255 : b]++;
                n_int++;
            }
            prev = now;
            dt = (double)(now - last) / 1e9;
            if (dt >= 5.0) {
                extern void xbox_vblank_report(double, char *, size_t);
                char vb[128], ft[256];
                uint32_t acc = 0, p50 = 0, p99 = 0, i, lo = 0, hi = 0;
                for (i = 0; i < 256; i++) {
                    acc += hist[i];
                    if (!p50 && acc * 2 >= n_int) p50 = i + 1;
                    if (!p99 && acc * 100 >= n_int * 99u) p99 = i + 1;
                }
                for (i = 0; i < 256; i++) {
                    if (i + 1 < p50 / 2) lo += hist[i];
                    if (i > p50 * 3 / 2) hi += hist[i];
                }
                xbox_vblank_report(dt, vb, sizeof vb);
                if (!xbox_ft_window_text(ft, sizeof ft))
                    snprintf(ft, sizeof ft, "frame split not known (other thread)");
                fprintf(stderr, "[fps] %.1f fps (frame %u), %s; flip interval p50 %.2f p99 %.2f ms,"
                        " %u short, %u long of %u; %s\n",
                        (s_frame - last_frame) / dt, s_frame, vb, p50 * 0.25, p99 * 0.25, lo, hi, n_int, ft);
                memset(hist, 0, sizeof hist);
                n_int = 0;
                last_frame = s_frame;
                last = now;
            }
        }
    }
    if (dump_every < 0) {
        const char *dd = getenv("RECOMP_GL_DUMP");
        dump_every = 0;
        if (dd && *dd) {
            const char *comma = strchr(dd, ',');
            size_t nn = comma ? (size_t)(comma - dd) : strlen(dd);
            if (nn >= sizeof dump_prefix) nn = sizeof dump_prefix - 1;
            memcpy(dump_prefix, dd, nn);
            dump_prefix[nn] = 0;
            dump_every = comma ? atoi(comma + 1) : 60;
            if (dump_every <= 0) dump_every = 60;
        }
    }
    end_rendering();
    s_rt = NULL;
    s_rt_depth = NULL;
    zstats_report(s_frame);
    xbox_PageTrackFrame();
    {
        /* RECOMP_PAGE_TRACK_STATS=1: the vertex mirror, every 300 frames. */
        static int on = -1;
        if (on < 0) {
            const char *e = getenv("RECOMP_PAGE_TRACK_STATS");
            on = e && *e == '1';
        }
        if (on && s_frame % 300 == 0) {
            fprintf(stderr, "[VMIRROR] frame %u: %u runs from the mirror, %u through the ring; %u pages copied in %u copies\n",
                    s_frame, s_vm_st_hit, s_vm_st_ring, s_vm_st_pages, s_vm_st_copies);
            s_vm_st_hit = s_vm_st_ring = s_vm_st_pages = s_vm_st_copies = 0;
        }
    }
    {
        extern uint32_t xbox_GetDisplayFramebuffer(uint32_t *pitch);
        uint32_t pitch = 0;
        xbox_GetDisplayFramebuffer(&pitch);
        s_scan_pitch = pitch;
    }
    {
        /* The render scale: the port's option (nv2a_vk_set_render_scale) unless
         * RECOMP_GL_SCALE[_X] is set, else a percentage (nv2a_vk_scale_pct).
         * An image that failed to allocate since the last flip (scale_failed)
         * sends the whole set back to the last scale that ran without one --
         * the old step is kept, the factor stays unfit; else this one is good. */
        int q = s_opt_scale_q, pct = nv2a_vk_scale_pct;
        if (s_scale_fail) {
            s_scale_fail = 0;
            if (s_scale != s_good_k || s_xratio != s_good_xr || s_square != s_good_sq) {
                fprintf(stderr, "  [VK] render scale %g x %g failed to allocate -- back to %g x %g\n",
                        s_scale_x, s_scale, s_good_k * s_good_xr, s_good_k);
                s_square = s_good_sq;
                s_xratio = s_good_xr;
                rescale_surfaces(s_good_k);
            }
        } else {
            s_good_k = s_scale;
            s_good_xr = s_xratio;
            s_good_sq = s_square;
        }
        int sq = s_square_env && nv2a_vk_square;
        double k, xr = s_xratio_env;
        if (q > 0 && s_scale_env) {
            static int told;
            if (!told++)
                fprintf(stderr, "  [VK] render scale option ignored: RECOMP_GL_SCALE[_X] is set\n");
            q = 0;
        }
        if (q > 0) {
            k = q / 1000.0;
            if (k < 0.5) k = 0.5;
            if (k > VK_SCALE_MAX) k = VK_SCALE_MAX;
            /* 16:9: 4/3 more columns, except on frames marked 4:3 (menus). */
            xr = s_opt_wide && xbox_present_wide_mark() != 0 ? 4.0 / 3.0 : 1.0;
            sq = 0;                     /* the option's X already holds the 4/3 */
        } else {
            k = pct < 50 ? 0.0 : pct > 500 ? VK_SCALE_MAX : pct / 100.0;
            if (!(k > 0.0))
                k = s_scale;
        }
        if (k != s_scale || xr != s_xratio || sq != s_square) {
            /* refused: beyond the image limits, or a factor that failed to
             * allocate before (scale_failed) -- no retry every flip */
            if (q > 0 && k * xr > s_scale_x && (!scale_fits(k * xr, k) || !nv2a_vk_render_scale_fits(k, xr > 1.01))) {
                static int last_q = -1;
                if (last_q != q)
                    fprintf(stderr, "  [VK] render scale %g x %g does not fit the device -- staying at %g x %g\n",
                            k * xr, k, s_scale_x, s_scale);
                last_q = q;
            } else {
                s_square = sq;
                s_xratio = xr;
                rescale_surfaces(k);
            }
        }
        if (!s_fit_known || s_frame % 60 == 0)
            update_fit_mask();
    }
    if (s && s_drew_any && s_shot_pending) {
        /* A screenshot asked for by name (g_xbox_screenshot_hook). */
        extern const char *xbox_getenv_utf8(const char *name);
        const char *pre = xbox_getenv_utf8("RECOMP_SHOT_PREFIX");
        char path[320];
        snprintf(path, sizeof path, "%s%s.bmp", pre && *pre ? pre : "shot_", s_shot_name);
        s_shot_pending = 0;
        dump_surface(s, path);
        fprintf(stderr, "[VK] screenshot %s (frame %u)\n", path, s_frame);
    }
    if (s && s_drew_any && dump_every && ((s_frame % (uint32_t)dump_every) == 0
                                          || (s_inst_alt && s_frame % (uint32_t)dump_every == 1))) {
        char path[320];
        snprintf(path, sizeof path, "%s%05u.bmp", dump_prefix, s_frame);
        dump_surface(s, path);
        if (s_headless && fxaa_on()) {
            snprintf(path, sizeof path, "%s%05u_fxaa.bmp", dump_prefix, s_frame);
            barrier_all();
            dump_fxaa(s, path);
        }
        {
            /* RECOMP_VK_DUMP_SURF=<hex va>[,<hex va>...]: those surfaces too,
             * as <prefix>NNNNN_s.bmp, _s1.bmp, ... (off-screen targets: the
             * scene a title composites, the faces of a reflection cube). */
            static int init, n_extra;
            static uint32_t extra[8];
            VkSurf *x;
            int k;
            if (!init) {
                const char *e = getenv("RECOMP_VK_DUMP_SURF");
                char *end;
                init = 1;
                while (e && *e && n_extra < 8) {
                    extra[n_extra++] = (uint32_t)strtoul(e, &end, 16);
                    e = *end == ',' ? end + 1 : NULL;
                }
            }
            for (k = 0; k < n_extra; k++) {
                if (!extra[k] || (x = surf_find(extra[k])) == NULL)
                    continue;
                if (k)
                    snprintf(path, sizeof path, "%s%05u_s%d.bmp", dump_prefix, s_frame, k);
                else
                    snprintf(path, sizeof path, "%s%05u_s.bmp", dump_prefix, s_frame);
                dump_surface(x, path);
            }
        }
    }
    barrier_all();
    /* ANTIALIASING = MSAA: the presented surface's multisampled image, made
     * or dropped here for the next frame's passes */
    ms_flip(s);
    /* ANTIALIASING = SMAA: the surface's anti-aliased copy, presented below
     * in its place (pres); the letterbox stays worked out from s */
    if (s && s_drew_any && !s_headless && aa_mode() == NV2A_VK_AA_SMAA)
        pres = smaa_run(s);
    if (!pres)
        pres = s;
    {
        /* SHADERS: a preset asked for is built / put in place / dropped here,
         * before the image is acquired (a swap waits for the device) */
#if !defined(__SWITCH__)
        SpDevice spd = { s_inst, s_pd, s_dev, s_queue, s_qfam, s_nframes };
        shader = !s_headless && sp_tick(&spd);
#endif
    }

    /* No pacing here: the guest is paced by its own swap throttle against
     * the kernel's 60 Hz guest vblank (xbox_GuestVblankCount, RECOMP_GUEST_HZ),
     * which serves the pushbuffer path and an HLE Present alike. */

    VT("flip %u: surface %08X drew %d, acquiring\n", s_frame, s ? s->va : 0, s_drew_any);
    if (!s_headless) {
        VkResult rr;
        long long t_acq = xbox_ft_on() ? xbox_ft_now() : 0;
        xbox_ft_push(FT_PRESENT);
        rr = vkAcquireNextImageKHR(s_dev, s_swapchain, UINT64_MAX, s_f->acquired,
                                   VK_NULL_HANDLE, &idx);
        xbox_ft_pop();
        if (t_acq)
            xbox_ft_acquire(xbox_ft_ms(t_acq));  /* shown on its own: which of the two calls blocks */
        if (rr == VK_ERROR_OUT_OF_DATE_KHR) {
            vkDeviceWaitIdle(s_dev);
            create_swapchain();
        } else if (rr == VK_SUCCESS || rr == VK_SUBOPTIMAL_KHR) {
            VkImage dst = s_sc_images[idx];
            VT("flip %u: acquired image %u\n", s_frame, idx);
            VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkClearColorValue black = { { 0, 0, 0, 1 } };
            int dw = 0, dh = 0, dx = 0, dy = 0;
            have_image = 1;
            if (s && s_drew_any) {
                /* The logical aspect (16:9 for a widescreen title),
                 * letterboxed. Rows are top-first on both sides. */
                float lw = (float)s->w / (float)s->aa_sx, lh = (float)s->h / (float)s->aa_sy;
                float ww = (float)s_sc_extent.width, wh = (float)s_sc_extent.height, scale;
                /* xbox_present_wide: 16:9 unless the port marked this frame
                 * as a 4:3 one (menus of a title without native 16:9); with
                 * square pixels the 4:3 frame is the same surface, shown
                 * pillarboxed. */
                if (!s_boot_on                     /* the boot video keeps its own aspect */
                    && (xbox_present_wide() || (xbox_video_widescreen() && stretch_43())) && lw < lh * 1.5f)
                    lw = lh * 16.0f / 9.0f;
                scale = ww / lw < wh / lh ? ww / lw : wh / lh;
                dw = (int)(lw * scale); dh = (int)(lh * scale);
                dx = ((int)ww - dw) / 2; dy = ((int)wh - dh) / 2;
            }
            /* a shader chain in place replaces FXAA (maintainer, 2026-10-27);
             * if its frame fails, FXAA / the blit present this one */
            if (dw > 0 && dh > 0
                && ((shader && present_shader(pres, dst, dx, dy, dw, dh))
                    || (fxaa_on()
                        && present_fxaa(s, dst, s_sc_views[idx], s_sc_format, s_sc_extent, dx, dy, dw, dh,
                                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT)))) {
                if (hud_on()) {
                    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.image = dst;
                    b.subresourceRange = rg;
                    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
                    hud_draw(dst);
                    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    b.dstAccessMask = 0;
                    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
                }
                goto presented;
            }
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = dst;
            b.subresourceRange = rg;
            vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &b);
            vkCmdClearColorImage(s_cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rg);
            if (dw > 0 && dh > 0) {
                VkImageBlit bl;
                VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 1, &mb, 0, NULL, 0, NULL);
                memset(&bl, 0, sizeof bl);
                bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                bl.srcSubresource.layerCount = 1;
                bl.srcOffsets[1].x = (int32_t)pres->pw;
                bl.srcOffsets[1].y = (int32_t)pres->ph;
                bl.srcOffsets[1].z = 1;
                bl.dstSubresource = bl.srcSubresource;
                bl.dstOffsets[0].x = dx;
                bl.dstOffsets[0].y = dy;
                bl.dstOffsets[1].x = dx + dw;
                bl.dstOffsets[1].y = dy + dh;
                bl.dstOffsets[1].z = 1;
                vkCmdBlitImage(s_cb, pres->image, VK_IMAGE_LAYOUT_GENERAL, dst,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
            }
            if (hud_on())
                hud_draw(dst);
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = 0;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        presented:;
        }
    }
    VT("flip %u: submit (image %d)\n", s_frame, have_image);
    if (s_f->stamping) {
        vkCmdWriteTimestamp(s_cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_ts_pool, s_fi * 2 + 1);
        s_f->stamped = 1;
    }
    vkEndCommandBuffer(s_cb);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_cb;
    if (have_image) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &s_f->acquired;
        si.pWaitDstStageMask = &ws;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &s_f->rendered;
    }
    if (vkQueueSubmit(s_queue, 1, &si, s_f->fence) != VK_SUCCESS)
        LOGE("vkQueueSubmit failed\n");
    s_f->submitted = 1;
    if (have_image) {
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        VkResult pr;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &s_f->rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &s_swapchain;
        pi.pImageIndices = &idx;
        VT("flip %u: vkQueuePresentKHR\n", s_frame);
        xbox_ft_push(FT_PRESENT);
        pr = vkQueuePresentKHR(s_queue, &pi);
        xbox_ft_pop();
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
            vkDeviceWaitIdle(s_dev);
            create_swapchain();
        }
    }
    if (hud_on()) {
        hud_tick();
    } else if (s_hud_last) {                        /* turned off: start afresh when it comes back */
        s_hud_last = 0;
        s_hud_n = 0;
        s_hud_max = 0;
        s_hud_txt[0][0] = 0;
    }
    {
        int counted = xbox_ft_frame();
        if (s_hitch[0]) {                          /* noted at the start: now with the frame's split */
            char ft[256];
            if (counted && xbox_ft_last_text(ft, sizeof ft))
                fprintf(stderr, "%s; %s\n", s_hitch, ft);
            else if (xbox_ft_on())                 /* flipped on another thread than the counted one */
                fprintf(stderr, "%s; split not counted (another thread)\n", s_hitch);
            else
                fprintf(stderr, "%s\n", s_hitch);
            s_hitch[0] = 0;
        }
    }
    /* RECOMP_DEBUG: what the renderer holds, every 10 s -- a slow climb in
     * process memory over many level set-ups shows up as the cache that grows. */
    if (debug_on()) {
        static uint64_t last_stat;
        uint64_t now = hz_now();
        if (now - last_stat >= 10000000000ull) {
            int i, ns = 0, nd = 0, nt = 0;
            uint64_t sb = 0, db = 0;
            last_stat = now;
            for (i = 0; i < VK_MAX_SURF; i++)
                if (s_surf[i].image) { ns++; sb += (uint64_t)s_surf[i].pw * s_surf[i].ph * 4u; }
            for (i = 0; i < VK_MAX_DEPTH; i++)
                if (s_depth[i].image) { nd++; db += (uint64_t)s_depth[i].pw * s_depth[i].ph * 4u; }
            int nb = 0, nown = 0;
            uint64_t bused = 0;
            for (i = 0; i < VK_MAX_TEX; i++)
                if (s_tex[i].image) { nt++; if (s_tex[i].mem) nown++; }
            for (i = 0; i < TEX_BLOCK_SLOTS; i++)
                if (s_tblk[i].mem) { nb++; bused += s_tblk[i].ra.used; }
            uint64_t vused = 0, vfree = vram_free(sb + db, &vused);
            LOGE("cache: %d surfaces %llu KB, %d depth %llu KB, %d textures %llu KB (%d blocks of %llu MB, "
                 "%llu KB in ranges, %d own), %d pipelines, %u programs, "
                 "garbage %u (cap %u); VRAM %s %llu MB, free %llu MB; scale %g x %g\n", ns,
                 (unsigned long long)(sb >> 10), nd, (unsigned long long)(db >> 10),
                 nt, (unsigned long long)(s_tex_bytes >> 10), nb, (unsigned long long)(s_tblk_bytes >> 20),
                 (unsigned long long)(bused >> 10), nown, s_npipes, s_prog_live, s_f->ngarbage, s_f->garbage_cap,
                 s_have_budget ? "used" : "held", (unsigned long long)(vused >> 20),
                 (unsigned long long)(vfree >> 20), s_scale_x, s_scale);
        }
    }
    /* RECOMP_RT_WATCH: re-arm every surface drawn into since the last flip,
     * so a CPU read of it shows up (kernel rt_watch.c). */
    if (xbox_RtWatchOn()) {
        static uint32_t armed_gen[VK_MAX_SURF];
        int k;
        for (k = 0; k < VK_MAX_SURF; k++)
            if (s_surf[k].image && s_surf[k].gen && s_surf[k].gen != armed_gen[k]) {
                armed_gen[k] = s_surf[k].gen;
                xbox_RtWatchArm(s_surf[k].va, s_surf[k].bytes);
            }
    }
    {
        static uint64_t last_save;
        uint64_t now = hz_now();
        if (s_pipes_unsaved && now - last_save > 20000000000ull) {
            last_save = now;
            pcache_save();
        }
    }
    VT("flip %u: presented, waiting for frame slot\n", s_frame);
    s_fi = (s_fi + 1) % s_nframes;
    frame_begin();
    VT("flip %u: next frame recording\n", s_frame);
    s_dyn_valid = 0;
#if !defined(__SWITCH__)
    if (s_win) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                /* Edges only: auto-repeat would read as a held key anyway. */
                if (s_key_handler && !e.key.repeat)
                    s_key_handler((int)e.key.scancode, e.type == SDL_EVENT_KEY_DOWN);
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                /* The key-ups go to the other window: release everything. */
                if (s_key_handler)
                    s_key_handler(-1, 0);
                break;
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (s_quit_handler)
                    s_quit_handler();
                break;
            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
                display_hz_update();
                break;
            case SDL_EVENT_WINDOW_MOVED:
            case SDL_EVENT_WINDOW_RESIZED:
                wr_record();                       /* the windowed rectangle for the port to save */
                break;
            default:
                break;
            }
        }
        dm_apply();                                /* a DISPLAY MODE change from the port */
    }
#endif
}

static void vk_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    (void)s; (void)b;                        /* draw_raw handles every batch */
}

static const Nv2aBackend s_backend = {
    vk_clear,
    vk_draw,
    vk_flip,
    vk_draw_raw,
    NV2A_BACKEND_RAW_DIRECT,
};

/* Diagnostics for CPU readback (M3.5 reduced 4b): 1 + the slot of a surface
 * this renderer drew into that covers the guest address (compared without
 * the 0x80000000/0xF0000000 aliases), 0 if none. Locking such memory from the
 * CPU reads what the guest last wrote there, not the rendered image. */
int nv2a_vk_render_target_at(uint32_t va)
{
    int i;
    uint32_t a = va & 0x0FFFFFFFu;
    for (i = 0; i < VK_MAX_SURF; i++) {
        uint32_t lo = s_surf[i].va & 0x0FFFFFFFu;
        if (s_surf[i].image && s_surf[i].gen && a >= lo && a < lo + s_surf[i].w * s_surf[i].h * 4u)
            return i + 1;
    }
    return 0;
}

void nv2a_vk_set_key_handler(void (*fn)(int scancode, int down))
{
    s_key_handler = fn;
}

void nv2a_vk_set_quit_handler(void (*fn)(void))
{
    s_quit_handler = fn;
}

void nv2a_vk_set_fatal_handler(void (*fn)(void), const char *title, const char *help)
{
    s_fatal_handler = fn;
    s_fatal_title = title;
    s_fatal_help = help;
}

/* The boot animation (src/bootvideo, before the title starts). The port's
 * thread decodes and hands each frame over (nv2a_vk_boot_frame); the NV2A
 * thread -- it drives the executor, owns the window and pumps its events --
 * presents it through g_xbox_nv2a_idle_hook (boot_idle): the frame becomes the
 * presented surface for one flip, so letterboxing, the display mode, SMAA /
 * FXAA, a shader chain and the event pump work as for the title. It cannot be
 * skipped (maintainer, 2026-10-27: the one video that is never skippable);
 * closing the window still quits. */
#if !defined(__SWITCH__)
extern void (*volatile g_xbox_nv2a_idle_hook)(void);   /* kernel.h */
static SDL_Mutex *s_bv_mx;
static uint8_t *s_bv_px;                /* the newest handed-over frame (B8G8R8A8), under s_bv_mx */
static int s_bv_w, s_bv_h, s_bv_new, s_bv_cap;
static SDL_AtomicInt s_bv_state;        /* 0 idle, 1 running, 2 the port asked to stop */
static SDL_AtomicInt s_bv_skip;         /* no window (headless, renderer failed): the port stops */

/* NV2A thread: the handed-over frame into s_boot (made or remade at its size) */
static int boot_upload(void)
{
    VkBufferImageCopy rg;
    VkDeviceSize off;
    void *p = NULL;
    int w = s_bv_w, h = s_bv_h;
    if (w <= 0 || h <= 0)
        return 0;
    if (!s_boot.image || s_boot.pw != (uint32_t)w || s_boot.ph != (uint32_t)h) {
        if (s_boot.image) {
            ms_drop(&s_boot);
            garbage_add(s_boot.image, s_boot.view, s_boot.mem);
        }
        memset(&s_boot, 0, sizeof s_boot);
        if (!make_image((uint32_t)w, (uint32_t)h, VK_FORMAT_B8G8R8A8_UNORM,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, &s_boot.image, &s_boot.view, &s_boot.mem))
            return 0;
        s_boot.w = s_boot.pw = (uint32_t)w;
        s_boot.h = s_boot.ph = (uint32_t)h;
        s_boot.aa_sx = s_boot.aa_sy = 1;
    }
    off = ring_alloc((VkDeviceSize)w * (VkDeviceSize)h * 4, 16, &p);
    if (!p)
        return 0;
    memcpy(p, s_bv_px, (size_t)w * (size_t)h * 4);
    memset(&rg, 0, sizeof rg);
    rg.bufferOffset = off;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = (uint32_t)w;
    rg.imageExtent.height = (uint32_t)h;
    rg.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cb, s_f->ring, s_boot.image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    barrier_all();
    return 1;
}

/* NV2A thread, once per pass of its loop while the animation runs */
static void boot_idle(void)
{
    static int no_window;
    int have = 0;
    if (SDL_GetAtomicInt(&s_bv_state) == 2) {      /* over: the surface goes, the title starts on black */
        if (s_last == &s_boot)
            s_last = NULL;
        if (s_scan == &s_boot)
            s_scan = NULL;
        s_drew_any = 0;
        if (s_boot.image) {
            ms_drop(&s_boot);
            garbage_add(s_boot.image, s_boot.view, s_boot.mem);
        }
        memset(&s_boot, 0, sizeof s_boot);
        s_boot_on = 0;
        g_xbox_nv2a_idle_hook = NULL;
        SDL_SetAtomicInt(&s_bv_state, 0);
        return;
    }
    if (no_window || !ready() || s_headless) {
        no_window = 1;
        SDL_SetAtomicInt(&s_bv_skip, 1);
        return;
    }
    SDL_LockMutex(s_bv_mx);
    if (s_bv_new) {
        have = boot_upload();
        s_bv_new = 0;
    }
    SDL_UnlockMutex(s_bv_mx);
    if (!have)
        return;
    s_boot_on = 1;
    s_last = &s_boot;
    s_drew_any = 1;
    vk_flip();
}

int nv2a_vk_boot_begin(void)
{
    if (!s_bv_mx && !(s_bv_mx = SDL_CreateMutex()))
        return 0;
    SDL_SetAtomicInt(&s_bv_skip, 0);
    SDL_SetAtomicInt(&s_bv_state, 1);
    g_xbox_nv2a_idle_hook = boot_idle;
    return 1;
}

int nv2a_vk_boot_frame(const uint8_t *bgra, int w, int h)
{
    size_t n = (size_t)w * (size_t)h * 4;
    if (SDL_GetAtomicInt(&s_bv_state) != 1 || w <= 0 || h <= 0)
        return 0;
    SDL_LockMutex(s_bv_mx);
    if (n > (size_t)s_bv_cap) {
        uint8_t *q = (uint8_t *)realloc(s_bv_px, n);
        if (!q) {
            SDL_UnlockMutex(s_bv_mx);
            return 0;
        }
        s_bv_px = q;
        s_bv_cap = (int)n;
    }
    memcpy(s_bv_px, bgra, n);
    s_bv_w = w;
    s_bv_h = h;
    s_bv_new = 1;
    SDL_UnlockMutex(s_bv_mx);
    return !SDL_GetAtomicInt(&s_bv_skip);
}

void nv2a_vk_boot_end(void)
{
    int i;
    if (SDL_GetAtomicInt(&s_bv_state) != 1)
        return;
    SDL_SetAtomicInt(&s_bv_state, 2);
    for (i = 0; i < 2000 && SDL_GetAtomicInt(&s_bv_state) != 0; i++)   /* the NV2A thread cleans up */
        SDL_Delay(1);
    if (SDL_GetAtomicInt(&s_bv_state) != 0)
        LOGE("boot video: the NV2A thread did not end the animation within 2 s\n");
    SDL_LockMutex(s_bv_mx);
    free(s_bv_px);
    s_bv_px = NULL;
    s_bv_cap = s_bv_w = s_bv_h = s_bv_new = 0;
    SDL_UnlockMutex(s_bv_mx);
}
#else
int nv2a_vk_boot_begin(void) { return 0; }
int nv2a_vk_boot_frame(const uint8_t *bgra, int w, int h) { (void)bgra; (void)w; (void)h; return 0; }
void nv2a_vk_boot_end(void) {}
#endif

/* g_xbox_screenshot_hook (kernel.h): taken at the next flip that drew
 * something; s_shot_name / s_shot_pending are declared with the window
 * statics above. */
extern void (*g_xbox_screenshot_hook)(const char *name);

static void vk_screenshot(const char *name)
{
    snprintf(s_shot_name, sizeof s_shot_name, "%s", name && *name ? name : "shot");
    s_shot_pending = 1;
}

void nv2a_vk_install(void)
{
    g_xbox_screenshot_hook = vk_screenshot;
    xbox_SetContiguousFreeHook(on_contig_free);
    nv2a_backend_register(&s_backend);
    fprintf(stderr, "[BOOT] NV2A Vulkan renderer registered\n");
#if !defined(__SWITCH__)
    {
        SDL_Thread *t = SDL_CreateThread(vk_precheck_thread, "vk_precheck", NULL);
        if (t)
            SDL_DetachThread(t);
        else
            vk_precheck();
    }
#else
    vk_precheck();
#endif
#if !defined(__SWITCH__)
    {
        extern const char *xbox_getenv_utf8(const char *name);
        const char *e = xbox_getenv_utf8("RECOMP_SHADER_PRESET");
        if (e) {
            sp_set_preset(e);
            s_shader_env = 1;
        }
    }
#endif
}
