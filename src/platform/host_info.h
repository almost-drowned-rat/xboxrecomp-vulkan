/*
 * host_info.h - which build runs where (platform layer), for the first lines
 * of every port's log:
 *
 *   version: v0.9.3.1 (game ae51948, toolkit ed5ca9a)
 *   host: Wine 9.0 (wine-9.0 Proton...) on Linux 6.1.52-valve16; Steam Deck, Gamescope
 *
 * The release name comes from <install root>\tools\release.txt (written by the
 * release launcher when it unpacks its files; "dev build" without it), the
 * commits from the build (cmake/build_info.cmake: the game repo and the
 * toolkit, "-dirty" with uncommitted changes). The host line names Windows
 * with its build, or Wine / Proton with the Unix system under it, plus what
 * the environment Wine passes through tells: Steam Deck (SteamDeck=1),
 * Gamescope (GAMESCOPE_WAYLAND_DISPLAY) or the desktop (XDG_CURRENT_DESKTOP).
 */

#ifndef HOST_INFO_H
#define HOST_INFO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "v0.9.3.1 (game ae51948, toolkit ed5ca9a)"; game_dir = the install's game
 * folder (its parent holds tools\release.txt) */
void host_version_text(const char *game_dir, char *buf, size_t n);
/* "Windows 10.0.26300 x64" / "Wine 9.0 (...) on Linux 6.x; Steam Deck, Gamescope" */
void host_describe(char *buf, size_t n);
/* both, as the lines "version: ..." and "host: ..." on stdout */
void host_log_version_and_host(const char *game_dir);

#ifdef __cplusplus
}
#endif

#endif
