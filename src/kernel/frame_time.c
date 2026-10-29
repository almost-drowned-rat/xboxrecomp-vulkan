/*
 * frame_time.c -- per-frame time split, as CSV and for the renderer's
 * diagnostic lines (see frame_time.h).
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

static int      s_csv = -1;                     /* RECOMP_FRAME_TIME: the CSV */
static volatile int s_want;                     /* the renderer's lines (xbox_ft_want) */
static int      s_started;                      /* owner and clock set (first frame) */
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
/* file reads and sleeps of any thread, in microseconds (xbox_ft_io / _sleep);
 * 32 bits: taken back to 0 every frame (the POSIX layer has no 64-bit add) */
static volatile LONG s_io_n, s_io_us, s_sleep_us, s_vbw_us;   /* + flip waits (xbox_ft_vblank_wait) */
static double   s_acq;                          /* the acquire part of "present" (xbox_ft_acquire) */
/* the last closed frame and the sums since the last xbox_ft_window */
static double   s_last[XBOX_FT_COUNT], s_last_total, s_last_io_ms, s_last_sleep_ms, s_last_vbw, s_last_acq;
static unsigned s_last_io_n;
static double   s_win[XBOX_FT_COUNT], s_win_gpu, s_win_max, s_win_vbw, s_win_acq;
static unsigned s_win_n, s_win_gpu_n;

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

static int csv_on(void)
{
    if (s_csv < 0) {
        const char *e = getenv("RECOMP_FRAME_TIME");
        s_csv = e && *e && *e != '0';
    }
    return s_csv;
}

int xbox_ft_on(void)
{
    return csv_on() || s_want;
}

void xbox_ft_want(int on)
{
    s_want = on != 0;
}

/* The first frame on: the clock, the owner thread (THPS 2X: the game thread;
 * the executor runs inside its PUT write) and, with RECOMP_FRAME_TIME, the file. */
static int start(void)
{
    LARGE_INTEGER fq;
    if (s_started)
        return 1;
    QueryPerformanceFrequency(&fq);
    s_tick_ms = 1000.0 / (double)fq.QuadPart;
    s_t0 = s_frame_start = s_since = now();
    s_owner = GetCurrentThreadId();
    s_started = 1;
    if (csv_on()) {
        extern FILE *xbox_fopen_utf8(const char *path, const char *mode);
        const char *e = getenv("RECOMP_FRAME_TIME");
        char path[1100];
        if (!strcmp(e, "1"))
            snprintf(path, sizeof path, "%s%s%s", s_dir, *s_dir ? "/" : "", "frametime.csv");
        else
            snprintf(path, sizeof path, "%s", e);
        s_f = xbox_fopen_utf8(path, "wb");
        if (!s_f) {
            fprintf(stderr, "  FRAMETIME: cannot write %s\n", path);
            s_csv = 0;
        } else {
            setvbuf(s_f, NULL, _IOFBF, 1 << 16);
            fprintf(s_f, "frame,t_ms,frame_ms,game_ms,translate_ms,gpu_wait_ms,present_ms,gpu_ms,input_ms,"
                         "acquire_ms\n");
            fprintf(stderr, "  FRAMETIME: writing %s\n", path);
        }
    }
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
    if (!xbox_ft_on() || !s_started || GetCurrentThreadId() != s_owner)
        return;
    charge();
    if (s_depth < DEPTH)
        s_stack[s_depth++] = category;
}

void xbox_ft_pop(void)
{
    if (!xbox_ft_on() || !s_started || GetCurrentThreadId() != s_owner || !s_depth)
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
    if (!xbox_ft_on() || !s_started)
        return;
    s_input = now();
}

/* Any thread: a file read or a sleep that took ms (kernel_file.c, kernel_thread.c). */
void xbox_ft_io(double ms)
{
    if (!xbox_ft_on())
        return;
    InterlockedIncrement(&s_io_n);
    InterlockedExchangeAdd(&s_io_us, (LONG)(ms * 1000.0));
}

void xbox_ft_sleep(double ms)
{
    if (!xbox_ft_on())
        return;
    InterlockedExchangeAdd(&s_sleep_us, (LONG)(ms * 1000.0));
}

/* The title waiting for a flip / the guest vblank (xbox_GuestFlipWait,
 * xbox_GuestYield): part of "game", shown on its own - at the original rate a
 * fast machine spends most of the frame here, a slow one little. */
void xbox_ft_vblank_wait(double ms)
{
    if (!xbox_ft_on())
        return;
    InterlockedExchangeAdd(&s_vbw_us, (LONG)(ms * 1000.0));
}

/* The presenting thread: how long acquiring the swapchain image took - part of
 * "present" (which is acquire + present), shown on its own so a report tells
 * which of the two calls blocks. */
void xbox_ft_acquire(double ms)
{
    if (!xbox_ft_on() || !s_started || GetCurrentThreadId() != s_owner)
        return;
    s_acq += ms;
}

long long xbox_ft_now(void)
{
    return (long long)now();
}

double xbox_ft_ms(long long since)
{
    return s_tick_ms > 0.0 ? (double)(now() - (LONGLONG)since) * s_tick_ms : 0.0;
}

int xbox_ft_frame(void)
{
    LONGLONG t, in;
    double total;
    int c;

    if (!xbox_ft_on() || !start())
        return 0;
    if (GetCurrentThreadId() != s_owner)
        return 0;
    charge();
    t = now();
    total = (double)(t - s_frame_start) * s_tick_ms;
    in = s_input;
    s_input = 0;
    if (s_f) {
        fprintf(s_f, "%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", s_frame,
                (double)(t - s_t0) * s_tick_ms, total, s_acc[XBOX_FT_GAME], s_acc[XBOX_FT_TRANSLATE],
                s_acc[XBOX_FT_GPU_WAIT], s_acc[XBOX_FT_PRESENT], s_gpu,
                in ? (double)(t - in) * s_tick_ms : -1.0, s_acq);
        if (s_frame % 60 == 0)
            fflush(s_f);
    }
    for (c = 0; c < XBOX_FT_COUNT; c++) {
        s_last[c] = s_acc[c];
        s_win[c] += s_acc[c];
    }
    s_last_total = total;
    s_last_io_n = (unsigned)InterlockedExchange(&s_io_n, 0);
    s_last_io_ms = (double)InterlockedExchange(&s_io_us, 0) / 1000.0;
    s_last_sleep_ms = (double)InterlockedExchange(&s_sleep_us, 0) / 1000.0;
    s_last_vbw = (double)InterlockedExchange(&s_vbw_us, 0) / 1000.0;
    s_win_vbw += s_last_vbw;
    s_last_acq = s_acq;
    s_win_acq += s_acq;
    s_acq = 0.0;
    if (total > s_win_max)
        s_win_max = total;
    if (s_gpu >= 0.0) {
        s_win_gpu += s_gpu;
        s_win_gpu_n++;
    }
    s_win_n++;
    s_frame++;
    s_frame_start = t;
    memset(s_acc, 0, sizeof s_acc);
    s_gpu = -1.0;
    return 1;
}

int xbox_ft_last_text(char *buf, size_t n)
{
    if (!s_started || !s_frame)
        return 0;
    snprintf(buf, n, "counted %.1f ms = game %.1f (vblank wait %.1f), translate %.1f, gpu wait %.1f, present "
             "%.1f (acquire %.1f); %u file reads %.1f ms, sleeps %.1f ms", s_last_total, s_last[XBOX_FT_GAME],
             s_last_vbw, s_last[XBOX_FT_TRANSLATE], s_last[XBOX_FT_GPU_WAIT], s_last[XBOX_FT_PRESENT], s_last_acq,
             s_last_io_n, s_last_io_ms, s_last_sleep_ms);
    return 1;
}

unsigned xbox_ft_window_text(char *buf, size_t n)
{
    unsigned f = s_win_n;
    double d = f ? (double)f : 1.0;
    char gpu[32] = "";                            /* GPU timestamps: whenever the split is counted */
    if (s_win_gpu_n)
        snprintf(gpu, sizeof gpu, ", gpu %.1f", s_win_gpu / s_win_gpu_n);
    snprintf(buf, n, "frame split game %.1f (vblank wait %.1f), translate %.1f, gpu wait %.1f, present %.1f "
             "(acquire %.1f)%s ms (average), longest %.1f ms", s_win[XBOX_FT_GAME] / d, s_win_vbw / d,
             s_win[XBOX_FT_TRANSLATE] / d, s_win[XBOX_FT_GPU_WAIT] / d, s_win[XBOX_FT_PRESENT] / d, s_win_acq / d,
             gpu, s_win_max);
    memset(s_win, 0, sizeof s_win);
    s_win_n = s_win_gpu_n = 0;
    s_win_gpu = s_win_max = s_win_vbw = s_win_acq = 0.0;
    return f;
}
