/*
 * host_vblank.h - wait for the display's vertical blank (platform layer)
 *
 * FRAME PACING Display (maintainer, 2026-11-05; the idea of NG2's option, our
 * own code): on a display whose rate is a whole multiple of the guest's vblank
 * rate (120 / 180 / 240 Hz for a 60 Hz title) the kernel's guest vblank follows
 * the display's own vblank instead of a 60 Hz software timer, so every game
 * frame stays on screen exactly N refreshes (the timer drifted against the
 * refresh: frames shown 1 and 3 refreshes in turn).
 *
 *   host_vblank_open   bind to the display a native window is on (Windows:
 *                      the HWND; gdi32's D3DKMT kernel thunks, no D3D
 *                      runtime). 0 = not available (other hosts for now:
 *                      VK_KHR_present_wait later, the timer meanwhile)
 *   host_vblank_wait   block until the next vblank of that display; 0 when the
 *                      wait failed (display gone / mode change) -- the caller
 *                      falls back to its timer
 *   host_vblank_close  release it
 *
 * One waiting thread only.
 */

#ifndef HOST_VBLANK_H
#define HOST_VBLANK_H

#ifdef __cplusplus
extern "C" {
#endif

int  host_vblank_open(void *native_window);
int  host_vblank_wait(void);
void host_vblank_close(void);

#ifdef __cplusplus
}
#endif

#endif /* HOST_VBLANK_H */
