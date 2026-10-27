/*
 * nv2a_vk_window -- open the Vulkan back end's window and present cleared
 * frames, without a title or the kernel.
 *
 * Plays the executor's part (nv2a_backend.h): registers nothing itself but
 * receives the back end from nv2a_vk_install(), hands it a 640x480 32-bit
 * colour surface, clears it to a colour that cycles red -> green -> blue,
 * draws one pre-transformed triangle with per-vertex colour through
 * draw_raw (shader generators, glslang at run time, pipeline) and flips.
 * nv2a_vk presents a surface only once something was drawn into it.
 * The surface's guest address only has to be stable; nv2a_vk keys its GPU
 * image by it.
 */
#include "nv2a_vk.h"
#include "../../src/kernel/nv2a_backend.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Executor stubs ─────────────────────────────────────────────── */

static const Nv2aBackend *g_backend;
static uint8_t g_guest[1 << 20];          /* stands in for guest RAM */
static uint8_t g_dirty[0x2000 / 64];

void nv2a_backend_register(const Nv2aBackend *backend) { g_backend = backend; }
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out)
{
    (void)tex; (void)argb_out;
    return 0;
}
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)(uintptr_t)g_guest; }
uint8_t *nv2a_pb_reg_dirty(void) { return g_dirty; }
int xbox_video_widescreen(void) { return 0; }
void xbox_vblank_report(double dt, char *buf, size_t n)
{
    (void)dt;
    snprintf(buf, n, "no vblank");
}

/* ── Test ───────────────────────────────────────────────────────── */

/* NV097 method shadow: what a D3D title would have set for an untextured,
 * vertex-coloured draw. Everything else stays 0 (blend, depth, cull off). */
static uint32_t g_regs[0x2000 / 4];

static void setup_regs(void)
{
    g_regs[0x358 / 4] = 0x01010101u;  /* SET_COLOR_MASK: A R G B */
    g_regs[0x1D98 / 4] = 639u << 16;  /* SET_CLEAR_RECT_HORIZONTAL: 0..639 */
    g_regs[0x1D9C / 4] = 479u << 16;  /* SET_CLEAR_RECT_VERTICAL: 0..479 */
    g_regs[0x1E60 / 4] = 1;           /* SET_COMBINER_CONTROL: one stage */
    /* Final combiner: rgb = D = diffuse (v0), alpha = G = diffuse alpha. */
    g_regs[0x288 / 4] = 0x00000004u;  /* SPECULAR_FOG_CW0: A B C D */
    g_regs[0x28C / 4] = 0x00001400u;  /* SPECULAR_FOG_CW1: E F G */
}

/* Attribute 0: position in surface pixels (w = 1/rhw = 1); attribute 3:
 * diffuse as float4 R G B A in 0..1 (the executor's conversion of a
 * D3DCOLOR). */
static float g_attrs[3][NV2A_RAW_ATTRS][4];
/* Transform constants: the executor always hands over the whole file (192
 * slots on the NV2A); unused by a pre-transformed draw but still uploaded. */
static float g_consts[192][4];

static void draw_triangle(const Nv2aSurface *s)
{
    static const float pos[3][2] = { { 320, 60 }, { 580, 420 }, { 60, 420 } };
    static const float col[3][4] = {
        { 1, 1, 1, 1 }, { 0, 0, 0, 1 }, { 1, 1, 0, 1 } };    /* white, black, yellow */
    static const uint32_t idx[3] = { 0, 1, 2 };
    Nv2aRawBatch b;
    int v;

    for (v = 0; v < 3; v++) {
        g_attrs[v][0][0] = pos[v][0];
        g_attrs[v][0][1] = pos[v][1];
        g_attrs[v][0][2] = 0.0f;
        g_attrs[v][0][3] = 1.0f;
        memcpy(g_attrs[v][3], col[v], sizeof col[v]);
    }
    memset(&b, 0, sizeof b);
    b.prim = 5;                                       /* TRIANGLES */
    b.vertex_count = 3;
    b.attr_present = (1u << 0) | (1u << 3);
    b.attrs = &g_attrs[0][0][0];
    b.indices = idx;
    b.index_count = 3;
    b.regs = g_regs;
    b.vp_consts = (const float (*)[4])g_consts;
    b.vp_const_count = 192;
    b.surface = *s;
    b.color_va = s->color_va;
    b.xform = 0;                                      /* pre-transformed */
    b.aa_sx = b.aa_sy = 1.0f;
    b.vp_prog_gen = b.vp_const_gen = b.attr_const_gen = 1;
    b.vtx_epoch = 1;
    g_backend->draw_raw(&b);
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 180;
    Nv2aSurface s;
    Nv2aRenderState rs;
    int i;

    if (frames <= 0)
        frames = 180;
    nv2a_vk_install();
    if (!g_backend || !g_backend->clear || !g_backend->flip || !g_backend->draw_raw) {
        fprintf(stderr, "nv2a_vk_window: back end not registered\n");
        return 1;
    }

    memset(&s, 0, sizeof s);
    s.color_va = 0x1000;
    s.width = 640;
    s.height = 480;
    s.pitch = 640 * 4;
    s.bytes_per_pixel = 4;
    s.aa_sx = s.aa_sy = 1;
    setup_regs();
    memset(&rs, 0, sizeof rs);
    rs.color_mask = 0x01010101u;
    rs.depth_max = 16777215.0f;

    for (i = 0; i < frames; i++) {
        static const uint32_t colours[3] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu };
        uint32_t argb = colours[(i / 60) % 3];
        g_backend->clear(&s, &rs, 0xF0, argb, 0);
        draw_triangle(&s);
        g_backend->flip();
    }
    printf("nv2a_vk_window: %d frames presented\n", frames);
    return 0;
}
