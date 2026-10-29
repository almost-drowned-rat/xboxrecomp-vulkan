/*
 * frame_time.h -- per-frame time split, as CSV (off by default).
 *
 * RECOMP_FRAME_TIME=1 writes <data dir>/frametime.csv (xbox_ft_set_dir), any
 * other value is taken as the file path. One row per presented frame:
 *
 *   frame, t_ms, frame_ms, game_ms, translate_ms, gpu_wait_ms, present_ms, gpu_ms, input_ms, acquire_ms
 *
 * The categories are exclusive wall time on the thread that presents: game
 * (everything not in another category), translate (pushbuffer executor or D3D
 * HLE recording Vulkan work), gpu_wait (CPU blocked on a GPU fence), present
 * (acquire + present; acquire_ms, the last column, is the acquire part of it).
 * gpu_ms is the GPU's own span for a frame (timestamp queries, written while
 * the split is counted - the CSV or the renderer's lines), known only when its
 * fence has signalled, so it is reported in the row of the frame that read it
 * back (VK_FRAMES later); -1 = none yet.
 * input_ms is the input-to-present latency on the CPU side: from the frame's
 * last pad read by the title (xbox_ft_input, XInputGetState HLE) to the end
 * of its present call; -1 = no read this frame. What the screen adds on top
 * (the GPU finishing the frame, then the wait for the next vblank in FIFO,
 * frames queued in the swapchain) is not in it.
 * tools/frametime_summary.py turns a file into average / p50 / p99.
 *
 * The renderer's diagnostic lines (nv2a_vk.c: [fps] every 5 s, [hitch] per
 * long frame; on with RECOMP_FPS_LOG=1 or the FPS counter) use the same split
 * without the file: xbox_ft_want(1) turns the counting on, xbox_ft_last_text
 * gives the frame just closed (with the file reads and sleeps of any thread
 * during it: kernel_file.c / kernel_thread.c call xbox_ft_io / xbox_ft_sleep),
 * xbox_ft_window_text the averages since its last call. Text, so the renderer
 * needs no kernel header (it declares what it uses).
 */
#ifndef XBOX_FRAME_TIME_H
#define XBOX_FRAME_TIME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { XBOX_FT_GAME, XBOX_FT_TRANSLATE, XBOX_FT_GPU_WAIT, XBOX_FT_PRESENT, XBOX_FT_COUNT };

int  xbox_ft_on(void);
void xbox_ft_want(int on);                    /* the renderer's lines need the split */
/* the frame just closed, as text ("game .. translate .. ms, n file reads ..");
 * 0 = none yet */
int  xbox_ft_last_text(char *buf, size_t n);
/* the averages since the last call, as text; returns the frames counted */
unsigned xbox_ft_window_text(char *buf, size_t n);
void xbox_ft_io(double ms);                   /* any thread: a file read         */
void xbox_ft_sleep(double ms);                /* any thread: a sleep             */
void xbox_ft_vblank_wait(double ms);          /* the title waiting for a flip    */
void xbox_ft_acquire(double ms);              /* the acquire part of "present"   */
long long xbox_ft_now(void);                  /* a counter value for xbox_ft_ms  */
double xbox_ft_ms(long long since);
void xbox_ft_set_dir(const char *data_dir);   /* where "1" puts frametime.csv */
void xbox_ft_push(int category);              /* enter; the outer category pauses */
void xbox_ft_pop(void);                       /* leave, back to the outer one     */
void xbox_ft_gpu(double ms);                  /* GPU span of an earlier frame     */
int  xbox_ft_frame(void);                     /* a frame was presented; 1 = counted
                                                 (on, by the owner thread)          */
void xbox_ft_input(void);                     /* the title read a pad             */

#ifdef __cplusplus
}
#endif

#endif /* XBOX_FRAME_TIME_H */
