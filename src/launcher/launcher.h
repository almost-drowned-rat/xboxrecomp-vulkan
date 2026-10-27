/*
 * launcher.h -- the one exe a player downloads (porting guide M7, "Release";
 * THPS docs/release_plan.md). It carries no game code and no game data: the
 * port's own code (prebuilt libraries, its sources), the lifter with an
 * embedded Python, licences - as a zip appended to the exe (the payload) - and
 * builds the game on the player's machine at the first start:
 *
 *   folders + READMEs -> the game (installer; "Choose disc image..." when
 *   none) -> unpack the payload -> fetch the compiler (pinned, SHA-256) ->
 *   lift -> compile -> link -> start the game.
 *
 * Later starts start the game at once (a stamp: release + XBE SHA-1);
 * `--repair` builds again. Everything stays inside the exe's folder. What to
 * run is data in the payload (lift.txt, build.txt, toolchain.txt, made by the
 * port's release script from its real build), so this file is the same for
 * every port; the port gives only its name, versions and icon (XrLauncherConfig).
 * SDL3 only (processes, dialogs, file system) - no OS code here.
 */
#ifndef XR_LAUNCHER_H
#define XR_LAUNCHER_H

#include <stddef.h>
#include <stdint.h>

#include "game_setup.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *game;                   /* "THPS 2X": the name in the messages */
    const char *disc;                   /* "THPS 2X (USA)": what the player has to provide */
    const char *window_title;           /* "THPS 2X Recomp" */
    const char *game_exe;               /* "THPS2X.exe": built next to the launcher */
    uint32_t    title_id;               /* the installer's checks */
    const xr_game_version *versions;
    unsigned    version_count;
    const void *icon_bmp;               /* the port's window icon (xbox_set_window_icon) */
    size_t      icon_bmp_size;
} XrLauncherConfig;

/* The whole launcher; returns the process exit code. */
int xr_launcher_main(const XrLauncherConfig *cfg, int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
