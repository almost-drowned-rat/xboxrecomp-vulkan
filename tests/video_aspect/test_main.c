/* video_aspect: 21:9 follows the display's exact ratio (xbox_video_aspect_fit;
 * THPS 2X maintainer, 2026-11-08). One case per process (the aspect is chosen
 * once): argv[1] = fit / keep / env / other. */
#define _XOPEN_SOURCE 700                          /* putenv on Linux */
#include "kernel.h"
#include "nv2a_backend.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* This settings-only test must never dispatch guest code. */
void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

static int expect(const char *what, double got, double want)
{
    if (fabs(got - want) > 1e-6) {
        fprintf(stderr, "FAIL %s: aspect %.6f, expected %.6f\n", what, got, want);
        return 1;
    }
    printf("PASS %s: %.6f\n", what, got);
    return 0;
}

static char s_ws169[] = "RECOMP_WIDESCREEN=16:9";
static char s_ws219[] = "RECOMP_WIDESCREEN=21:9";
static char s_ratio[] = "RECOMP_DISPLAY_RATIO=3840x1600";

int main(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "fit";
    if (!strcmp(c, "other")) {                    /* 16:9 chosen: a 21:9 display changes nothing */
        putenv(s_ws169);
        xbox_video_aspect_fit(3440.0 / 1440.0);
        return expect("16:9 on a 3440x1440 display", xbox_video_aspect(), 16.0 / 9.0);
    }
    putenv(s_ws219);
    if (!strcmp(c, "env")) {                      /* the test stand-in wins over the display */
        putenv(s_ratio);
        xbox_video_aspect_fit(3440.0 / 1440.0);
        return expect("21:9 with RECOMP_DISPLAY_RATIO=3840x1600", xbox_video_aspect(), 2.4);
    }
    if (!strcmp(c, "keep")) {                     /* a 16:9 desktop: 64:27 as before */
        xbox_video_aspect_fit(1920.0 / 1080.0);
        return expect("21:9 on a 1920x1080 display", xbox_video_aspect(), 64.0 / 27.0);
    }
    if (expect("21:9 before the display is known", xbox_video_aspect(), 64.0 / 27.0))
        return 1;
    xbox_video_aspect_fit(3440.0 / 1440.0);
    if (expect("21:9 on a 3440x1440 display", xbox_video_aspect(), 43.0 / 18.0))
        return 1;
    xbox_video_aspect_fit(2560.0 / 1080.0);       /* only the first display counts */
    return expect("a second display ignored", xbox_video_aspect(), 43.0 / 18.0);
}
