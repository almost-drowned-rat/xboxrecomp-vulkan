/*
 * game_setup_ui.h - a small SDL3 window for xr_game_setup: a progress bar
 * and a line of text, drawn with SDL_RenderDebugText (no font library).
 * Closing the window cancels the install. Messages (an error, an unknown
 * version) go into an SDL message box, which needs no window.
 *
 * Usage: ui = xr_setup_ui_open("THPS 2X"); opts.ctx = ui;
 * opts.progress = xr_setup_ui_progress; opts.cancelled = xr_setup_ui_cancelled;
 * xr_game_setup(&opts, &res); xr_setup_ui_close(ui);
 * The window opens only when there is something to install (on the first
 * progress call), so a normal start shows nothing.
 */
#ifndef XR_GAME_SETUP_UI_H
#define XR_GAME_SETUP_UI_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XrSetupUi XrSetupUi;

XrSetupUi *xr_setup_ui_open(const char *title);
void xr_setup_ui_progress(void *ui, double fraction, const char *text);
int  xr_setup_ui_cancelled(void *ui);
void xr_setup_ui_close(XrSetupUi *ui);

/* A message box (error = 1: an error icon). Works with ui == NULL. Nothing
 * with RECOMP_NO_DIALOGS=1 (automated tests: errors go to the log only). */
void xr_setup_ui_message(XrSetupUi *ui, int error, const char *text);
int  xr_no_dialogs(void);

#ifdef __cplusplus
}
#endif

#endif /* XR_GAME_SETUP_UI_H */
