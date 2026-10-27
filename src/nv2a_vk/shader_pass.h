/* shader_pass.h -- the optional RetroArch slang shader pass of nv2a_vk
 * (shader module, porting_guide.md M6): librashader, loaded at run time from
 * the exe's folder, applied to the presented surface into the window's game
 * rectangle. Render-thread functions unless noted. */
#ifndef NV2A_VK_SHADER_PASS_H
#define NV2A_VK_SHADER_PASS_H

#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    uint32_t frames;          /* frames in flight */
} SpDevice;

/* Any thread: the preset to use (UTF-8 path of a .slangp), NULL or "" = none.
 * Drops the parameter overrides of the previous preset. */
void sp_set_preset(const char *path);
/* Any thread: a parameter override for the current preset, applied to its
 * chain now or once it is built. */
void sp_set_param(const char *name, float value);
/* Any thread: NV2A_VK_SHADER_* (nv2a_vk.h). */
int sp_status(void);

/* Every flip, before the pass: starts a build the port asked for (on a
 * worker thread), puts a finished one in place (its set-up commands submitted
 * and waited for), drops the old chain after the device is idle. 1 if a chain
 * is in place for sp_frame. */
int sp_tick(const SpDevice *d);
/* Records the chain into cb: in = the presented surface (SHADER_READ_ONLY
 * layout), out = the swapchain image (COLOR_ATTACHMENT layout), the game
 * rectangle (vx, vy, vw, vh) as the viewport -- the chain clears the rest of
 * out to black. history = 0 drops the feedback/history images (the rectangle
 * or the input size changed). 0 on an error (logged; the chain is dropped and
 * the caller presents without it). */
int sp_frame(VkCommandBuffer cb, uint64_t frame, VkImage in, VkFormat in_fmt, uint32_t iw, uint32_t ih,
             VkImage out, VkFormat out_fmt, uint32_t ow, uint32_t oh, int vx, int vy, int vw, int vh,
             int history, float fps, uint32_t dt_ms);

#endif
