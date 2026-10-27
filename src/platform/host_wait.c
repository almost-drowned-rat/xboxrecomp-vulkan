/*
 * host_wait.c - host clock and waits for every title (see host_wait.h)
 */

#include "host_wait.h"

#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <sched.h>
#include <time.h>
#endif

#if defined(_WIN32)

uint64_t host_time_ns(void)
{
    static LONGLONG freq;
    LARGE_INTEGER c;
    if (!freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart;
    }
    QueryPerformanceCounter(&c);
    /* Split, so that the product cannot overflow for any uptime. */
    return (uint64_t)(c.QuadPart / freq) * 1000000000ull
         + (uint64_t)(c.QuadPart % freq) * 1000000000ull / (uint64_t)freq;
}

void host_sleep_ns(uint64_t ns)
{
    static _Thread_local HANDLE timer;   /* one timer per waiting thread */
    LARGE_INTEGER due;
    if (!timer) {
        static volatile LONG told;
        int hires = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */,
                                       0x1F0003 /* TIMER_ALL_ACCESS */);
        if (!timer) {
            hires = 0;
            timer = CreateWaitableTimerExW(NULL, NULL, 0, 0x1F0003);
        }
        if (!InterlockedExchange(&told, 1))
            fprintf(stderr, "  Host waits: %s waitable timer\n",
                    hires ? "high-resolution" : "standard (no high-resolution timer: ~1 ms or coarser)");
    }
    due.QuadPart = -(LONGLONG)(ns / 100);
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
        WaitForSingleObject(timer, INFINITE);
    else
        Sleep((DWORD)(ns / 1000000));
}

static void host_yield(void)
{
    if (!SwitchToThread())
        YieldProcessor();
}

#else

uint64_t host_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void host_sleep_ns(uint64_t ns)
{
    struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
}

static void host_yield(void)
{
    sched_yield();
}

#endif

void host_wait_until_ns(uint64_t deadline_ns)
{
    uint64_t now = host_time_ns();
    if (now >= deadline_ns)
        return;
    if (deadline_ns - now > HOST_WAIT_SLACK_NS)
        host_sleep_ns(deadline_ns - now - HOST_WAIT_SLACK_NS);
    while (host_time_ns() < deadline_ns)
        host_yield();
}
