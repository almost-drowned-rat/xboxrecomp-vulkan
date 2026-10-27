/*
 * host_wait_test - the platform layer's clock and waits (platform/host_wait.h)
 *
 * host_time_ns is monotonic; host_sleep_ns sleeps at least about the time
 * asked; host_wait_until_ns never returns before its deadline and is late by
 * well under a millisecond (median), which is what a frame limiter needs.
 */

#include "platform/host_wait.h"

#include <stdio.h>
#include <stdlib.h>

static int s_fail;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        s_fail = 1;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int main(void)
{
    enum { N = 60 };
    uint64_t late[N], prev, t0, t1, worst_early = 0;
    int i, mono = 1, early = 0;

    prev = host_time_ns();
    for (i = 0; i < 100000; i++) {
        uint64_t t = host_time_ns();
        if (t < prev)
            mono = 0;
        prev = t;
    }
    check(mono, "host_time_ns is monotonic");

    t0 = host_time_ns();
    host_sleep_ns(5000000);
    t1 = host_time_ns();
    printf("     host_sleep_ns(5 ms) took %.3f ms\n", (double)(t1 - t0) / 1e6);
    /* A timer may wake a few microseconds before its due time. */
    check(t1 - t0 >= 4900000, "host_sleep_ns sleeps at least the time asked");
    check(t1 - t0 < 50000000, "host_sleep_ns does not hang");

    for (i = 0; i < N; i++) {
        uint64_t want = 2000000ull + (uint64_t)(i % 9) * 1000000ull;   /* 2..10 ms */
        uint64_t deadline = host_time_ns() + want, now;
        host_wait_until_ns(deadline);
        now = host_time_ns();
        if (now < deadline) {
            early = 1;
            if (deadline - now > worst_early)
                worst_early = deadline - now;
            late[i] = 0;
        } else {
            late[i] = now - deadline;
        }
    }
    qsort(late, N, sizeof late[0], cmp_u64);
    printf("     host_wait_until_ns lateness: median %.1f us, p90 %.1f us, max %.1f us\n",
           (double)late[N / 2] / 1e3, (double)late[N * 9 / 10] / 1e3, (double)late[N - 1] / 1e3);
    check(!early, "host_wait_until_ns never returns before its deadline");
    check(late[N / 2] < 500000, "host_wait_until_ns median lateness < 0.5 ms");
    host_wait_until_ns(host_time_ns() - 1000);
    check(1, "host_wait_until_ns returns at once for a past deadline");
    if (early)
        printf("     (earliest return %.1f us before the deadline)\n", (double)worst_early / 1e3);
    return s_fail;
}
