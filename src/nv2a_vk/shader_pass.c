/* shader_pass.c -- RetroArch slang shaders on the presented picture through
 * librashader (shader module, porting_guide.md M6; the decisions are in the
 * port's docs/shader_module_phase1.md).
 *
 * - The library is the user's librashader.dll / .so / .dylib next to the exe,
 *   loaded with SDL_LoadObject from that absolute path only -- never through
 *   the system search order -- and only once a preset is asked for. Its C ABI
 *   (librashader.h, MIT, third_party/librashader) is checked before any other
 *   symbol is used; a missing file or another ABI leaves the module off.
 * - librashader's own shader cache stays disabled: it writes to the user's
 *   platform cache folder (%LOCALAPPDATA%\librashader on Windows), and nothing
 *   may be written outside the port's folder. Every preset load compiles.
 * - A chain is built on a worker thread with the deferred create (its GPU
 *   set-up recorded into a command buffer of the worker's own pool -- the
 *   plain create submits to the queue and waits on it, which would race the
 *   render thread). The render thread submits that set-up, waits for it and
 *   puts the chain in place at a flip; until then the picture stays unshaded.
 * - A broken preset or a failed frame is logged and leaves the picture
 *   unshaded; nothing here can stop the game.
 * - No preset: sp_tick returns 0 at once -- no pass, no cost. */
#include "shader_pass.h"
#include "nv2a_vk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#define LIBRA_RUNTIME_VULKAN
#include "third_party/librashader/librashader.h"

#if defined(_WIN32)
#define SP_LIB_NAME "librashader.dll"
#elif defined(__APPLE__)
#define SP_LIB_NAME "librashader.dylib"
#else
#define SP_LIB_NAME "librashader.so"
#endif

/* ── The library ──────────────────────────────────────────────────── */

static struct {
    SDL_SharedObject *so;
    size_t api;                                   /* the library's API version, capped at ours */
    PFN_libra_preset_create_with_options preset_create;
    PFN_libra_preset_free preset_free;
    PFN_libra_preset_get_runtime_params params;
    PFN_libra_preset_free_runtime_params params_free;
    PFN_libra_error_write err_write;
    PFN_libra_error_free_string err_free_string;
    PFN_libra_error_free err_free;
    PFN_libra_vk_filter_chain_create_deferred create;
    PFN_libra_vk_filter_chain_frame frame;
    PFN_libra_vk_filter_chain_set_param set_param;
    PFN_libra_vk_filter_chain_get_param get_param;
    PFN_libra_vk_filter_chain_get_active_pass_count pass_count;
    PFN_libra_vk_filter_chain_free free;
} L;
static int s_lib = -1;                            /* -1 not tried, 0 unusable, 1 loaded (worker thread) */

static int lib_path(char *out, size_t n)
{
    const char *base = SDL_GetBasePath();         /* the exe's folder, with the separator; SDL owns it */
    if (!base)
        return 0;
    return (size_t)snprintf(out, n, "%s%s", base, SP_LIB_NAME) < n;
}

static int lib_load(void)
{
    char path[1024];
    PFN_libra_instance_abi_version abi;
    PFN_libra_instance_api_version api;
    if (s_lib >= 0)
        return s_lib;
    s_lib = 0;
    if (!lib_path(path, sizeof path))
        return 0;
    L.so = SDL_LoadObject(path);
    if (!L.so) {
        /* present but not loadable: a library it needs is missing -- the
         * official Windows build imports D3DX9_43.dll (DirectX end-user
         * runtime), which Windows does not ship */
        if (nv2a_vk_shader_library_present())
            fprintf(stderr, "  [VK] shader: %s does not load (%s) -- a library it needs is missing"
                    " (Windows: D3DX9_43.dll); shaders off\n", path, SDL_GetError());
        else
            fprintf(stderr, "  [VK] shader: no %s next to the exe -- shaders off\n", SP_LIB_NAME);
        return 0;
    }
    abi = (PFN_libra_instance_abi_version)SDL_LoadFunction(L.so, "libra_instance_abi_version");
    api = (PFN_libra_instance_api_version)SDL_LoadFunction(L.so, "libra_instance_api_version");
    if (!abi || !api || abi() != LIBRASHADER_CURRENT_ABI || api() < 1) {
        fprintf(stderr, "  [VK] shader: %s has C ABI %d, API %d -- this build needs ABI %d; shaders off\n",
                path, abi ? (int)abi() : -1, api ? (int)api() : -1, LIBRASHADER_CURRENT_ABI);
        SDL_UnloadObject(L.so);
        memset(&L, 0, sizeof L);
        return 0;
    }
    L.api = api() < LIBRASHADER_CURRENT_VERSION ? api() : LIBRASHADER_CURRENT_VERSION;
#define SP_FN(f, name) (L.f = (__typeof__(L.f))SDL_LoadFunction(L.so, name)) != NULL
    if (!(SP_FN(preset_create, "libra_preset_create_with_options") && SP_FN(preset_free, "libra_preset_free")
          && SP_FN(params, "libra_preset_get_runtime_params")
          && SP_FN(params_free, "libra_preset_free_runtime_params")
          && SP_FN(err_write, "libra_error_write") && SP_FN(err_free_string, "libra_error_free_string")
          && SP_FN(err_free, "libra_error_free")
          && SP_FN(create, "libra_vk_filter_chain_create_deferred")
          && SP_FN(frame, "libra_vk_filter_chain_frame")
          && SP_FN(set_param, "libra_vk_filter_chain_set_param")
          && SP_FN(get_param, "libra_vk_filter_chain_get_param")
          && SP_FN(pass_count, "libra_vk_filter_chain_get_active_pass_count")
          && SP_FN(free, "libra_vk_filter_chain_free"))) {
        fprintf(stderr, "  [VK] shader: %s lacks a function this build needs -- shaders off\n", path);
        SDL_UnloadObject(L.so);
        memset(&L, 0, sizeof L);
        return 0;
    }
#undef SP_FN
    fprintf(stderr, "  [VK] shader: %s loaded (C ABI %d, API %d)\n", path, LIBRASHADER_CURRENT_ABI,
            (int)api());
    s_lib = 1;
    return 1;
}

/* Logs and frees a librashader error; 1 if there was one. */
static int lib_err(libra_error_t e, const char *what)
{
    char *msg = NULL;
    if (!e)
        return 0;
    if (L.err_write(e, &msg) == 0 && msg) {
        fprintf(stderr, "  [VK] shader: %s failed: %s\n", what, msg);
        L.err_free_string(&msg);
    } else {
        fprintf(stderr, "  [VK] shader: %s failed\n", what);
    }
    L.err_free(&e);
    return 1;
}

/* ── What the port asked for (any thread) ─────────────────────────── */

typedef struct { char name[64]; float value; } SpParam;

static SDL_Mutex *s_mx;
static SDL_InitState s_mx_init;
static char s_want[1024];                         /* the preset, "" none */
static SDL_AtomicInt s_want_gen;
static SpParam *s_ov;                             /* parameter overrides of that preset */
static int s_nov, s_cap_ov;
static SDL_AtomicInt s_ov_dirty;
static SDL_AtomicInt s_status;                    /* NV2A_VK_SHADER_* */

static void mx_lock(void)
{
    if (SDL_ShouldInit(&s_mx_init)) {
        s_mx = SDL_CreateMutex();
        SDL_SetInitialized(&s_mx_init, true);
    }
    SDL_LockMutex(s_mx);
}

void sp_set_preset(const char *path)
{
    mx_lock();
    if (strcmp(s_want, path ? path : "")) {
        snprintf(s_want, sizeof s_want, "%s", path ? path : "");
        s_nov = 0;
        SDL_AddAtomicInt(&s_want_gen, 1);
    }
    SDL_UnlockMutex(s_mx);
}

void sp_set_param(const char *name, float value)
{
    int i;
    if (!name || !*name || strlen(name) >= sizeof s_ov[0].name)
        return;
    mx_lock();
    for (i = 0; i < s_nov && strcmp(s_ov[i].name, name); i++)
        ;
    if (i == s_nov) {
        if (s_nov == s_cap_ov) {
            int cap = s_cap_ov ? s_cap_ov * 2 : 32;
            SpParam *p = realloc(s_ov, (size_t)cap * sizeof *p);
            if (!p) {
                SDL_UnlockMutex(s_mx);
                return;
            }
            s_ov = p;
            s_cap_ov = cap;
        }
        snprintf(s_ov[s_nov++].name, sizeof s_ov[0].name, "%s", name);
    }
    s_ov[i].value = value;
    SDL_SetAtomicInt(&s_ov_dirty, 1);
    SDL_UnlockMutex(s_mx);
}

int sp_status(void)
{
    return SDL_GetAtomicInt(&s_status);
}

int nv2a_vk_shader_library_present(void)
{
    char path[1024];
    SDL_PathInfo pi;
    return lib_path(path, sizeof path) && SDL_GetPathInfo(path, &pi) && pi.type == SDL_PATHTYPE_FILE;
}

/* ── The build (worker thread) ────────────────────────────────────── */

typedef struct {
    char path[1024];
    SpDevice d;
    libra_vk_filter_chain_t chain;
    VkCommandPool pool;
    VkCommandBuffer cb;
    int nparams;
    Nv2aVkShaderParam *params;                    /* the preset's #pragma parameters, copied (the menu) */
    double ms;
    int no_lib;
} SpBuild;

/* The parameters of the chain in place, for nv2a_vk_shader_params (under s_mx). */
static Nv2aVkShaderParam *s_params;
static int s_nparams;
static unsigned s_params_gen;

static void copy_text(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src ? src : "");
}

int nv2a_vk_shader_params(Nv2aVkShaderParam *out, int max, unsigned *gen)
{
    int n;
    mx_lock();
    n = s_nparams;
    if (out && max > 0)
        memcpy(out, s_params, (size_t)(n < max ? n : max) * sizeof *out);
    if (gen)
        *gen = s_params_gen;
    SDL_UnlockMutex(s_mx);
    return n;
}

static void params_publish(Nv2aVkShaderParam *p, int n)
{
    mx_lock();
    free(s_params);
    s_params = p;
    s_nparams = p ? n : 0;
    s_params_gen++;
    SDL_UnlockMutex(s_mx);
}

static SpBuild s_b;
static SDL_Thread *s_thread;
static SDL_AtomicInt s_done;

static int SDLCALL build_thread(void *arg)
{
    SpBuild *b = arg;
    uint64_t t0 = SDL_GetTicksNS();
    libra_shader_preset_t preset = NULL;
    libra_preset_opt_t po;
    libra_preset_param_list_t pl;
    libra_device_vk_t dv;
    filter_chain_vk_opt_t co;
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };

    if (!lib_load()) {
        b->no_lib = 1;
        goto done;
    }
    memset(&po, 0, sizeof po);
    po.version = L.api;
    po.original_aspect_uniforms = true;           /* the frame options pass the aspect and the rate */
    po.frametime_uniforms = true;
    if (lib_err(L.preset_create(b->path, NULL, &po, &preset), "loading the preset"))
        goto done;
    memset(&pl, 0, sizeof pl);
    if (L.params(&preset, &pl) == NULL) {
        uint64_t i;
        b->nparams = (int)pl.length;
        b->params = pl.length ? calloc((size_t)pl.length, sizeof *b->params) : NULL;
        for (i = 0; b->params && i < pl.length; i++) {
            const libra_preset_param_t *s = &pl.parameters[i];
            Nv2aVkShaderParam *d = &b->params[i];
            copy_text(d->name, sizeof d->name, s->name);
            copy_text(d->desc, sizeof d->desc, s->description);
            d->initial = s->initial;
            d->minimum = s->minimum;
            d->maximum = s->maximum;
            d->step = s->step;
        }
        L.params_free(pl);
    }
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = b->d.qfam;
    if (vkCreateCommandPool(b->d.dev, &pci, NULL, &b->pool) != VK_SUCCESS)
        goto done;
    cai.commandPool = b->pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkAllocateCommandBuffers(b->d.dev, &cai, &b->cb) != VK_SUCCESS
        || vkBeginCommandBuffer(b->cb, &bi) != VK_SUCCESS)
        goto done;
    memset(&co, 0, sizeof co);
    co.version = L.api;
    co.frames_in_flight = b->d.frames;
    co.use_dynamic_rendering = true;              /* Vulkan 1.3 core, enabled on our device */
    co.disable_cache = true;                      /* its cache would write outside the folder */
    dv.physical_device = b->d.pd;
    dv.instance = b->d.inst;
    dv.device = b->d.dev;
    dv.queue = b->d.queue;
    dv.entry = vkGetInstanceProcAddr;
    if (lib_err(L.create(&preset, dv, b->cb, &co, &b->chain), "building the shader chain"))
        b->chain = NULL;
    preset = NULL;                                /* taken by the create, even when it failed */
    if (vkEndCommandBuffer(b->cb) != VK_SUCCESS && b->chain) {
        L.free(&b->chain);
        b->chain = NULL;
    }
done:
    if (preset)
        L.preset_free(&preset);
    b->ms = (SDL_GetTicksNS() - t0) / 1e6;
    SDL_SetAtomicInt(&s_done, 1);
    return 0;
}

/* ── In place (render thread) ─────────────────────────────────────── */

static libra_vk_filter_chain_t s_chain;
static unsigned s_have_gen;
static int s_fresh;                               /* the chain's first frame: no history yet */
static int s_bad;                                 /* a frame failed: dropped at the next tick */

static void chain_drop(VkDevice dev)
{
    if (!s_chain)
        return;
    vkDeviceWaitIdle(dev);                        /* frames in flight still read its images */
    L.free(&s_chain);
    s_chain = NULL;
}

static void build_finish(void)
{
    SpBuild *b = &s_b;
    VkFence f = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    uint32_t passes = 0;
    uint64_t t0 = SDL_GetTicksNS();

    SDL_WaitThread(s_thread, NULL);
    s_thread = NULL;
    SDL_SetAtomicInt(&s_done, 0);
    if (b->chain) {
        /* its set-up (buffer uploads, image layouts) has to have run before
         * its first frame */
        si.commandBufferCount = 1;
        si.pCommandBuffers = &b->cb;
        if (vkCreateFence(b->d.dev, &fci, NULL, &f) != VK_SUCCESS
            || vkQueueSubmit(b->d.queue, 1, &si, f) != VK_SUCCESS
            || vkWaitForFences(b->d.dev, 1, &f, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            fprintf(stderr, "  [VK] shader: the chain's set-up did not run -- shaders off\n");
            vkDeviceWaitIdle(b->d.dev);
            L.free(&b->chain);
            b->chain = NULL;
        }
        if (f)
            vkDestroyFence(b->d.dev, f, NULL);
    }
    if (b->pool)
        vkDestroyCommandPool(b->d.dev, b->pool, NULL);
    b->pool = VK_NULL_HANDLE;
    b->cb = VK_NULL_HANDLE;
    chain_drop(b->d.dev);
    if (b->chain) {
        s_chain = b->chain;
        b->chain = NULL;
        s_fresh = 1;
        {
            /* "initial" = what the chain uses before any override: the
             * shader's default or the value the preset file sets (a simple
             * preset's whole point) -- the target of a reset in the menu */
            int i;
            for (i = 0; b->params && i < b->nparams; i++) {
                float v;
                libra_error_t e = L.get_param(&s_chain, b->params[i].name, &v);
                if (e)
                    L.err_free(&e);
                else
                    b->params[i].initial = v;
            }
        }
        params_publish(b->params, b->nparams);
        b->params = NULL;
        SDL_SetAtomicInt(&s_ov_dirty, 1);         /* overrides set while it was built */
        lib_err(L.pass_count(&s_chain, &passes), "the pass count");
        fprintf(stderr, "  [VK] shader: %s -- %u passes, %d parameters, built in %.0f ms"
                " (+ %.1f ms set-up on the render thread)\n", b->path, passes, b->nparams, b->ms,
                (SDL_GetTicksNS() - t0) / 1e6);
        SDL_SetAtomicInt(&s_status, NV2A_VK_SHADER_ON);
    } else {
        free(b->params);
        b->params = NULL;
        params_publish(NULL, 0);
        if (!b->no_lib)
            fprintf(stderr, "  [VK] shader: %s not used -- the picture stays unshaded\n", b->path);
        SDL_SetAtomicInt(&s_status, b->no_lib ? NV2A_VK_SHADER_NO_LIBRARY : NV2A_VK_SHADER_FAILED);
    }
}

static void apply_overrides(void)
{
    int i;
    mx_lock();
    SDL_SetAtomicInt(&s_ov_dirty, 0);
    for (i = 0; i < s_nov; i++) {
        libra_error_t e = L.set_param(&s_chain, s_ov[i].name, s_ov[i].value);
        if (e) {
            char what[96];
            snprintf(what, sizeof what, "parameter %s", s_ov[i].name);
            lib_err(e, what);                     /* not in this preset: ignored */
        }
    }
    SDL_UnlockMutex(s_mx);
}

int sp_tick(const SpDevice *d)
{
    unsigned gen = (unsigned)SDL_GetAtomicInt(&s_want_gen);
    if (s_bad) {
        s_bad = 0;
        chain_drop(d->dev);                       /* the status stays FAILED until another preset */
        params_publish(NULL, 0);
    }
    if (s_thread && SDL_GetAtomicInt(&s_done))
        build_finish();
    if (!s_thread && gen != s_have_gen) {
        mx_lock();
        gen = (unsigned)SDL_GetAtomicInt(&s_want_gen);
        snprintf(s_b.path, sizeof s_b.path, "%s", s_want);
        SDL_UnlockMutex(s_mx);
        s_have_gen = gen;
        if (!s_b.path[0]) {
            chain_drop(d->dev);
            params_publish(NULL, 0);
            SDL_SetAtomicInt(&s_status, NV2A_VK_SHADER_OFF);
        } else {
            s_b.d = *d;
            s_b.chain = NULL;
            s_b.no_lib = 0;
            s_b.nparams = 0;
            s_b.params = NULL;
            SDL_SetAtomicInt(&s_status, NV2A_VK_SHADER_LOADING);
            s_thread = SDL_CreateThread(build_thread, "shader", &s_b);
            if (!s_thread) {
                fprintf(stderr, "  [VK] shader: no worker thread -- shaders off\n");
                SDL_SetAtomicInt(&s_status, NV2A_VK_SHADER_FAILED);
            }
        }
    }
    if (s_chain && SDL_GetAtomicInt(&s_ov_dirty))
        apply_overrides();
    return s_chain != NULL;
}

int sp_frame(VkCommandBuffer cb, uint64_t frame, VkImage in, VkFormat in_fmt, uint32_t iw, uint32_t ih,
             VkImage out, VkFormat out_fmt, uint32_t ow, uint32_t oh, int vx, int vy, int vw, int vh,
             int history, float fps, uint32_t dt_ms)
{
    libra_image_vk_t li, lo;
    libra_viewport_t vp;
    frame_vk_opt_t fo;
    if (!s_chain)
        return 0;
    li.handle = in; li.format = in_fmt; li.width = iw; li.height = ih;
    lo.handle = out; lo.format = out_fmt; lo.width = ow; lo.height = oh;
    vp.x = (float)vx; vp.y = (float)vy; vp.width = (uint32_t)vw; vp.height = (uint32_t)vh;
    memset(&fo, 0, sizeof fo);
    fo.version = L.api;
    fo.clear_history = !history || s_fresh;
    fo.frame_direction = 1;
    fo.total_subframes = 1;
    fo.current_subframe = 1;
    fo.aspect_ratio = (float)vw / (float)vh;      /* what the picture is shown at: 4:3 or 16:9 */
    fo.frames_per_second = fps;
    fo.frametime_delta = dt_ms;
    fo.brightness_nits = 200.0f;
    s_fresh = 0;
    if (lib_err(L.frame(&s_chain, cb, (size_t)frame, li, lo, &vp, NULL, &fo), "a frame")) {
        /* recorded in part at most: the caller ends the frame unshaded; the
         * chain goes at the next flip (its commands are in flight now) */
        SDL_SetAtomicInt(&s_status, NV2A_VK_SHADER_FAILED);
        s_bad = 1;
        return 0;
    }
    return 1;
}
