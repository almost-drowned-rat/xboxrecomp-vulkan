/* window_icon.c -- the port's window icon (kernel.h xbox_set_window_icon): a
 * BMP the port keeps for the run. A file of its own, so a program that links
 * the kernel library only for this (the launcher) pulls in nothing else. */

#include <stddef.h>

static const void *s_window_icon;
static size_t s_window_icon_n;

void xbox_set_window_icon(const void *bmp, size_t n)
{
    s_window_icon = n ? bmp : NULL;
    s_window_icon_n = bmp ? n : 0;
}

const void *xbox_window_icon(size_t *n)
{
    if (n)
        *n = s_window_icon_n;
    return s_window_icon;
}
