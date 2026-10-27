/* nv2a_vk.h -- Vulkan renderer for the pushbuffer executor (see nv2a_vk.c). */
#ifndef NV2A_VK_H
#define NV2A_VK_H

#include <stdint.h>

/* Register the Vulkan back end with the executor (nv2a_backend.h). */
void nv2a_vk_install(void);

/* Window events, delivered on the render thread (it pumps the SDL window in
 * every flip). Key: SDL scancode = USB HID usage ID and 1/0 for down/up,
 * auto-repeat dropped; scancode -1 when the window loses the focus, meaning
 * "release every key" -- fits xbox_InputKeyEvent directly. Quit: the window
 * was closed. Set either before nv2a_vk_install; NULL ignores the events. */
void nv2a_vk_set_key_handler(void (*fn)(int scancode, int down));
void nv2a_vk_set_quit_handler(void (*fn)(void));
/* 1 + slot of a drawn-into surface covering the guest address, 0 if none
 * (CPU readback diagnostics). */
int nv2a_vk_render_target_at(uint32_t va);
/* 1 while the swapchain presents with FIFO -- the display then paces too, next
 * to the guest vblank -- or before the first swapchain; 0 with MAILBOX or
 * IMMEDIATE (default MAILBOX when the device has it; RECOMP_PRESENT). A port's
 * frame limiter keeps its FIFO behaviour while this is 1. Any thread. */
int nv2a_vk_present_fifo(void);

/* The refresh rate of the display the window is on, in Hz (59.94, 120, 144 ...);
 * 0 while not known (headless, before the window). Read on the render thread at
 * the window's creation and when it moves to another display or that display's
 * mode changes. A port's frame-rate option uses it (THPS 2X: 120 only on a display
 * at 120 Hz or more). Any thread. */
float nv2a_vk_display_hz(void);

/* RENDER RESOLUTION (porting_guide.md M6): factor = the vertical scale on the
 * title's own lines, 1.00..5.00 (the menu offers 0.25 steps at 4:3; a port
 * may offer the standard 16:9 sizes, e.g. 1.875 = 1600x900, kept to a
 * thousandth); wide = 1 when
 * gameplay is 16:9 -- then X = factor x 4/3 on every frame the port has not
 * marked 4:3 (xbox_SetPresentWide), else X = factor. 0 = no option (the
 * RECOMP_GL_SCALE default). Any thread; applied at the next flip, refused
 * with a log line if the surfaces would not fit. RECOMP_GL_SCALE[_X] set in
 * the environment overrides it. */
void nv2a_vk_set_render_scale(double factor, int wide);
/* 1 if the device can hold the current surfaces at that factor (image
 * limits, no earlier allocation failure there -- no free-VRAM estimate:
 * maintainer, 2026-10-26), 0 if the menu should refuse it. A factor that
 * fails to allocate sends the render scale back to the last good one. The 17 quarter steps are tracked; a factor between
 * two (1.875 = 900 lines) answers for the larger one; 1 before the first
 * frame. */
int nv2a_vk_render_scale_fits(double factor, int wide);

/* FPS COUNTER (porting_guide.md M6): which lines the counter in the window's
 * top-left corner shows -- frames per second, the longest frame of the last
 * half second in ms, both, or none (default). Any thread; the next frame.
 * RECOMP_FPS_HUD set in the environment overrides it (1 = both, fps, ms, 0). */
enum { NV2A_VK_HUD_OFF, NV2A_VK_HUD_FPS, NV2A_VK_HUD_BOTH, NV2A_VK_HUD_MS };
void nv2a_vk_set_fps_hud(int mode);

/* "Menus stretched" (aspect-ratio rule, porting_guide.md M6): with 16:9 on,
 * frames the port marked 4:3 (xbox_SetPresentWide(0): menus, movies) are
 * stretched to the 16:9 window instead of pillarboxed. Default off. Any
 * thread; the next flip. RECOMP_STRETCH_43=1|0 overrides it. */
void nv2a_vk_set_stretch_43(int on);

/* ANTIALIASING (porting_guide.md M6): OFF, FXAA (the present pass, the default
 * -- the picture as before the option), SMAA (iryoku/smaa, three passes at the
 * presented surface's size before it is scaled into the window), MSAA 2X / 4X
 * (the presented surface and its depth only). Any thread; the next flip.
 * RECOMP_VK_AA=off|fxaa|smaa|msaa2|msaa4 overrides it; RECOMP_VK_FXAA=0 still
 * turns FXAA off. */
enum { NV2A_VK_AA_OFF, NV2A_VK_AA_FXAA, NV2A_VK_AA_SMAA, NV2A_VK_AA_MSAA2, NV2A_VK_AA_MSAA4 };
void nv2a_vk_set_antialiasing(int mode);
/* 0 once an MSAA count failed to allocate this run or the device lacks it --
 * the renderer went back to the count that last worked; the port's menu then
 * takes its setting back too. 1 for every other mode, and before the first
 * frame. Any thread. */
int nv2a_vk_antialiasing_fits(int mode);

/* SHADERS (shader module, porting_guide.md M6): a RetroArch slang preset
 * (.slangp, UTF-8 path) applied by librashader to the game picture -- after
 * the render scale and SMAA / MSAA, into the window's game rectangle only
 * (the bars stay black, the FPS counter stays on top). FXAA is skipped while a
 * shader is on. The library is the user's librashader.dll / .so / .dylib next
 * to the exe (C ABI 2), loaded on the first preset; librashader's own disk
 * cache stays off. A preset is built on a worker thread -- the picture stays
 * unshaded until it is ready (NV2A_VK_SHADER_LOADING). NULL or "" = none (the
 * default; no pass, no cost). Any thread; RECOMP_SHADER_PRESET set in the
 * environment overrides it. A new preset drops the parameter overrides. */
enum { NV2A_VK_SHADER_OFF, NV2A_VK_SHADER_LOADING, NV2A_VK_SHADER_ON, NV2A_VK_SHADER_FAILED,
       NV2A_VK_SHADER_NO_LIBRARY };
void nv2a_vk_set_shader_preset(const char *path);
/* A parameter of the current preset (its #pragma parameter name), applied now
 * or once the preset is built. Any thread. */
void nv2a_vk_set_shader_param(const char *name, float value);
/* The parameters of the preset in place (its #pragma parameter lines), for a
 * port's menu: up to max of them into out; returns their count (0 while none
 * is in place). initial = the value the chain uses without an override (the
 * shader's default or the preset file's own value); step 0 means "none
 * given". *gen changes whenever the list does (a new preset). Any thread. */
typedef struct {
    char name[64];
    char desc[128];
    float initial, minimum, maximum, step;
} Nv2aVkShaderParam;
int nv2a_vk_shader_params(Nv2aVkShaderParam *out, int max, unsigned *gen);
/* NV2A_VK_SHADER_*: OFF, LOADING, ON, FAILED (the preset or a frame failed,
 * logged; the picture is unshaded) or NO_LIBRARY (no librashader next to the
 * exe, or another ABI). Any thread. */
int nv2a_vk_shader_status(void);
/* 1 if librashader's file is next to the exe (the menu greys the shader
 * lines out otherwise) -- the file only; its ABI is checked at the first
 * preset. Any thread. */
int nv2a_vk_shader_library_present(void);

/* DISPLAY MODE (porting_guide.md M6): WINDOWED, BORDERLESS (fullscreen at the
 * desktop size) or FULLSCREEN (exclusive, the display's native mode). Any
 * thread; applied by the render thread at the next flip, or when the window
 * is created. RECOMP_DISPLAY_MODE=windowed|borderless|fullscreen overrides it
 * (automated runs: windowed). Before the first call: windowed. */
enum { NV2A_VK_DISPLAY_WINDOWED, NV2A_VK_DISPLAY_BORDERLESS, NV2A_VK_DISPLAY_FULLSCREEN };
void nv2a_vk_set_display_mode(int mode);

/* The windowed rectangle the port saved last time (client area, screen
 * coordinates): call before the first frame. The height is kept, the width
 * follows the title's aspect ratio; a position on no display is centred
 * instead; w or h <= 0 = the default 960x720 / 1280x720. */
void nv2a_vk_set_window_rect(int x, int y, int w, int h);

/* The windowed rectangle as it is now, and a counter that changes whenever it
 * moved or was resized while windowed (not maximised, not fullscreen): the
 * port saves it when the counter changed. Any thread. */
unsigned nv2a_vk_window_rect(int *x, int *y, int *w, int *h);

/* The boot animation (src/bootvideo, porting_guide.md M6), before the title
 * starts, from the port's thread: nv2a_vk_boot_begin, then one call per
 * decoded frame (B8G8R8A8, w x h, rows top first; copied) - the NV2A thread,
 * which owns the window, presents it like a title frame (letterboxed in its
 * own aspect, display mode, antialiasing and a shader chain in place) through
 * g_xbox_nv2a_idle_hook. It cannot be skipped (the one video that is never
 * skippable, maintainer 2026-10-27). nv2a_vk_boot_frame returns 1 to go on, 0
 * when there is no window (headless, renderer failed). nv2a_vk_boot_end waits
 * until the frame is dropped; the title starts on black. */
int nv2a_vk_boot_begin(void);
int nv2a_vk_boot_frame(const uint8_t *bgra, int w, int h);
void nv2a_vk_boot_end(void);

#endif
