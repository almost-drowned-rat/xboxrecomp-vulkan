/*
 * game_setup_ui.c - the SDL3 progress window for xr_game_setup. See
 * game_setup_ui.h.
 */
#include "game_setup_ui.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UI_W      640
#define UI_H      240                   /* room for UI_LINES wrapped text lines */
#define UI_SCALE  2.0f                  /* the debug font is 8x8: drawn at 16x16 */
#define UI_LINES  4
#define UI_COLS   ((int)(UI_W / UI_SCALE - 24) / 8)   /* 37 characters per line */

struct XrSetupUi {
    char          title[128];
    SDL_Window   *win;
    SDL_Renderer *ren;
    int           sdl_up, failed, closed;
    Uint64        last_draw;
};

XrSetupUi *xr_setup_ui_open(const char *title)
{
    XrSetupUi *ui = (XrSetupUi *)calloc(1, sizeof *ui);
    if (ui)
        snprintf(ui->title, sizeof ui->title, "%s - first start", title ? title : "Game");
    return ui;
}

/* The window on the first progress call: a start with the game in place
 * never opens one. */
static int ui_show(XrSetupUi *ui)
{
    if (ui->win || ui->failed)
        return ui->win != NULL;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        ui->failed = 1;                 /* headless: install without a window */
        return 0;
    }
    ui->sdl_up = 1;
    if (!SDL_CreateWindowAndRenderer(ui->title, UI_W, UI_H, 0, &ui->win, &ui->ren)) {
        ui->failed = 1;
        return 0;
    }
    SDL_SetRenderScale(ui->ren, UI_SCALE, UI_SCALE);
    {
        /* the port's icon (kernel.h xbox_set_window_icon; the kernel library is in every port's exe) */
        extern const void *xbox_window_icon(size_t *n);
        size_t in = 0;
        const void *ib = xbox_window_icon(&in);
        SDL_Surface *is = ib ? SDL_LoadBMP_IO(SDL_IOFromConstMem(ib, in), true) : NULL;
        if (is) {
            SDL_SetWindowIcon(ui->win, is);
            SDL_DestroySurface(is);
        }
    }
    return 1;
}

/* The text word-wrapped to UI_COLS (a word longer than a line is cut),
 * at most UI_LINES lines; '\n' starts a new line. */
static void ui_text(SDL_Renderer *ren, float x, float y, const char *s)
{
    char line[UI_COLS + 1];
    int row;

    for (row = 0; row < UI_LINES && *s; row++) {
        int n = 0, brk = -1;
        while (s[n] && s[n] != '\n' && n < UI_COLS) {
            if (s[n] == ' ')
                brk = n;
            n++;
        }
        if (s[n] && s[n] != '\n' && s[n] != ' ' && brk > 0)
            n = brk;                    /* the line is full: break at the last space */
        memcpy(line, s, (size_t)n);
        line[n] = 0;
        SDL_RenderDebugText(ren, x, y + 12.0f * row, line);
        s += n;
        while (*s == ' ')
            s++;
        if (*s == '\n')
            s++;
    }
}

void xr_setup_ui_progress(void *p, double fraction, const char *text)
{
    XrSetupUi *ui = (XrSetupUi *)p;
    SDL_Event e;
    SDL_FRect bar;
    Uint64 now;
    const float w = UI_W / UI_SCALE, h = UI_H / UI_SCALE;

    if (!ui || !ui_show(ui))
        return;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            ui->closed = 1;
    now = SDL_GetTicks();
    if (fraction < 1.0 && now - ui->last_draw < 50)
        return;                         /* 20 redraws a second are plenty */
    ui->last_draw = now;
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    SDL_SetRenderDrawColor(ui->ren, 16, 20, 32, 255);
    SDL_RenderClear(ui->ren);
    SDL_SetRenderDrawColor(ui->ren, 230, 230, 230, 255);
    SDL_RenderDebugText(ui->ren, 12, 10, "Installing the game files");
    ui_text(ui->ren, 12, 30, text ? text : "");
    bar.x = 12; bar.y = h - 28; bar.w = w - 24; bar.h = 12;
    SDL_SetRenderDrawColor(ui->ren, 60, 66, 84, 255);
    SDL_RenderFillRect(ui->ren, &bar);
    bar.w = (float)((w - 24) * fraction);
    SDL_SetRenderDrawColor(ui->ren, 240, 200, 40, 255);
    SDL_RenderFillRect(ui->ren, &bar);
    SDL_RenderPresent(ui->ren);
}

int xr_setup_ui_cancelled(void *p)
{
    XrSetupUi *ui = (XrSetupUi *)p;
    return ui && ui->closed;
}

/* RECOMP_NO_DIALOGS=1 (every automated test): no message box, the caller's
 * log line is all there is -- a box would wait for a click forever. */
int xr_no_dialogs(void)
{
    const char *e = getenv("RECOMP_NO_DIALOGS");
    return e && *e == '1';
}

void xr_setup_ui_message(XrSetupUi *ui, int error, const char *text)
{
    if (xr_no_dialogs())
        return;
    SDL_ShowSimpleMessageBox(error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_WARNING,
                             ui ? ui->title : "First start", text, ui ? ui->win : NULL);
}

void xr_setup_ui_close(XrSetupUi *ui)
{
    if (!ui)
        return;
    if (ui->ren)
        SDL_DestroyRenderer(ui->ren);
    if (ui->win)
        SDL_DestroyWindow(ui->win);
    if (ui->sdl_up)
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    free(ui);
}
