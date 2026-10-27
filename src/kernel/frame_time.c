/*
 * frame_time.c -- per-frame time split, as CSV (see frame_time.h).
 *
 * Costs two counter reads per category change while on, one branch while off.
 * Only the presenting thread pushes and pops; other threads' calls are
 * ignored, so a sound thread entering the executor cannot corrupt the stack.
 */
#include "frame_time.h"

#include "platform/xbox_winnt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEPTH 8

static int      s_on = -1;
static char     s_dir[1024];
static FILE    *s_f;
static double   s_tick_ms;
static LONGLONG s_t0, s_frame_start, s_since;   /* counter values */
static double   s_acc[XBOX_FT_COUNT];
static int      s_stack[DEPTH];
static int      s_depth;
static double   s_gpu = -1.0;
static unsigned s_frame;
static DWORD    s_owner;
static volatile LONGLONG s_input;               /* last pad read, 0 = none this frame */

static LONGLONG now(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

void xbox_ft_set_dir(const char *data_dir)
{
    snprintf(s_dir, sizeof s_dir, "%s", data_dir ? data_dir : "");
}

int xbox_ft_on(void)
{
    if (s_on < 0) {
        const char *e = getenv("RECOMP_FRAME_TIME");
        s_on = e && *e && *e != '0';
    }
    return s_on;
}

static int open_file(void)
{
    extern FILE *xbox_fopen_utf8(const char *path, const char *mode);
    const char *e = getenv("RECOMP_FRAME_TIME");
    char path[1100];
    LARGE_INTEGER fq;

    if (s_f)
        return 1;
    if (!strcmp(e, "1"))
        snprintf(path, sizeof path, "%s%s%s", s_dir, *s_dir ? "/" : "", "frametime.csv");
    else
        snprintf(path, sizeof path, "%s", e);
    s_f = xbox_fopen_utf8(path, "wb");
    if (!s_f) {
        fprintf(stderr, "  FRAMETIME: cannot write %s\n", path);
        s_on = 0;
        return 0;
    }
    setvbuf(s_f, NULL, _IOFBF, 1 << 16);
    fprintf(s_f, "frame,t_ms,frame_ms,game_ms,translate_ms,gpu_wait_ms,present_ms,gpu_ms,input_ms\n");
    QueryPerformanceFrequency(&fq);
    s_tick_ms = 1000.0 / (double)fq.QuadPart;
    s_t0 = s_frame_start = s_since = now();
    s_owner = GetCurrentThreadId();
    fprintf(stderr, "  FRAMETIME: writing %s\n", path);
    return 1;
}

static int current(void)
{
    return s_depth ? s_stack[s_depth - 1] : XBOX_FT_GAME;
}

/* Charge the time since the last change to the running category. */
static void charge(void)
{
    LONGLONG t = now();
    s_acc[current()] += (double)(t - s_since) * s_tick_ms;
    s_since = t;
}

void xbox_ft_push(int category)
{
    if (!xbox_ft_on() || !s_f || GetCurrentThreadId() != s_owner)
        return;
    charge();
    if (s_depth < DEPTH)
        s_stack[s_depth++] = category;
}

void xbox_ft_pop(void)
{
    if (!xbox_ft_on() || !s_f || GetCurrentThreadId() != s_owner || !s_depth)
        return;
    charge();
    s_depth--;
}

void xbox_ft_gpu(double ms)
{
    s_gpu = ms;
}

/* Any thread: titles may poll the pad from their own input thread. */
void xbox_ft_input(void)
{
    if (!xbox_ft_on() || !s_f)
        return;
    s_input = now();
}

void xbox_ft_frame(void)
{
    LONGLONG t, in;
    double total;

    if (!xbox_ft_on() || !open_file())
        return;
    /* The first presenting thread owns the counters (THPS 2X: the game
     * thread; the executor runs inside its PUT write). */
    if (GetCurrentThreadId() != s_owner)
        return;
    charge();
    t = now();
    total = (double)(t - s_frame_start) * s_tick_ms;
    in = s_input;
    s_input = 0;
    fprintf(s_f, "%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", s_frame,
            (double)(t - s_t0) * s_tick_ms, total, s_acc[XBOX_FT_GAME], s_acc[XBOX_FT_TRANSLATE],
            s_acc[XBOX_FT_GPU_WAIT], s_acc[XBOX_FT_PRESENT], s_gpu,
            in ? (double)(t - in) * s_tick_ms : -1.0);
    if (s_frame % 60 == 0)
        fflush(s_f);
    s_frame++;
    s_frame_start = t;
    memset(s_acc, 0, sizeof s_acc);
    s_gpu = -1.0;
}
