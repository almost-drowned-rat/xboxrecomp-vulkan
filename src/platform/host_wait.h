/*
 * host_wait.h - host clock and waits for every title (platform layer)
 *
 * One monotonic clock and two waits, so that the kernel's guest waits and the
 * game overrides that replace a title's own spin loops (frame limiters, A10:
 * no busy waiting) do not each build their own timer:
 *
 *   host_time_ns       monotonic nanoseconds (QueryPerformanceCounter on
 *                      Windows, CLOCK_MONOTONIC elsewhere); the same clock as
 *                      the guest's KeQueryPerformanceCounter on Windows
 *   host_sleep_ns      sleep at least ns without spinning: a high-resolution
 *                      waitable timer on Windows (Sleep() rounds up to the
 *                      15.6 ms tick); wakes up to ~0.5-1 ms late
 *   host_wait_until_ns wait until a host_time_ns deadline, precisely: the
 *                      timer to HOST_WAIT_SLACK_NS before it, then yields for
 *                      the rest (the tail is at most the slack, not a spin
 *                      loop over the whole wait). For a limiter that must
 *                      hold an exact deadline; host_sleep_ns where waking a
 *                      little late does not matter.
 */

#ifndef HOST_WAIT_H
#define HOST_WAIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The timer's lead before a host_wait_until_ns deadline: the high-resolution
 * waitable timer wakes up to ~0.6 ms late (measured, THPS 2X frame limiter). */
#if defined(_WIN32)
#define HOST_WAIT_SLACK_NS 1000000ull
#else
#define HOST_WAIT_SLACK_NS 200000ull
#endif

uint64_t host_time_ns(void);
void     host_sleep_ns(uint64_t ns);
void     host_wait_until_ns(uint64_t deadline_ns);

#ifdef __cplusplus
}
#endif

#endif /* HOST_WAIT_H */
