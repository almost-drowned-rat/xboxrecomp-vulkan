/*
 * xbox_devbus.c -- device interrupt lines
 *
 * From burnout3-sw (antoxa2584x/burnout3-sw, 0bf84b5,
 * xboxrecomp/src/kernel/xbox_devbus.c), MIT. Only the interrupt lines are
 * ported; the contiguous-arena physical range belongs to that fork's arena,
 * which sits above the image -- this kernel's starts at XBOX_CONTIG_BASE
 * (see dma_resolve in nv2a_pb_exec.c).
 *
 * Device interrupt lines, level-triggered, one bit per vector.
 *
 * A device model (the pushbuffer executor's PGRAPH trap on vector 3) asserts
 * its line; the timer thread in kernel_bridge.c wakes at once and calls the
 * connected ISR, and keeps calling it each tick while the line stays up. The
 * ISR acknowledges in the device's own status register, which is what lowers
 * the line.
 *
 * ponytail: ISRs run on the timer thread with no interrupt spinlock, like the
 * vblank in kernel_bridge.c. Add the interrupt object's lock if a driver is
 * seen racing its ISR.
 */

#include "kernel.h"

static volatile LONG g_irq_lines;
static HANDLE        g_irq_event;

HANDLE xbox_irq_line_event(void)
{
    if (!g_irq_event) {
        HANDLE e = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (InterlockedCompareExchangePointer(&g_irq_event, e, NULL))
            CloseHandle(e);
    }
    return g_irq_event;
}

void xbox_set_irq_line(uint32_t vector, int level)
{
    if (vector >= 32)
        return;
    if (level) {
        InterlockedOr(&g_irq_lines, (LONG)(1u << vector));
        SetEvent(xbox_irq_line_event());
    } else {
        InterlockedAnd(&g_irq_lines, ~(LONG)(1u << vector));
    }
}

uint32_t xbox_irq_lines(void)
{
    return (uint32_t)g_irq_lines;
}
