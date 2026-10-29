/*
 * host_info.c - version and host lines for the log (see host_info.h).
 */
#include "host_info.h"

#include "xr_build_info.h"                        /* generated: XR_GAME_COMMIT, XR_TOOLKIT_COMMIT */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

/* the first line of <parent of game_dir>/tools/release.txt, or "" */
static void release_name(const char *game_dir, char *out, size_t n)
{
    char path[1100];
    size_t len = game_dir ? strlen(game_dir) : 0;
    FILE *f;
    out[0] = 0;
    while (len && (game_dir[len - 1] == '/' || game_dir[len - 1] == '\\'))
        len--;
    while (len && game_dir[len - 1] != '/' && game_dir[len - 1] != '\\')
        len--;
    snprintf(path, sizeof path, "%.*stools/release.txt", (int)len, len ? game_dir : "");
    f = fopen(path, "rb");
    if (!f)
        return;
    if (fgets(out, (int)n, f)) {
        size_t m = strlen(out);
        while (m && (out[m - 1] == '\n' || out[m - 1] == '\r' || out[m - 1] == ' '))
            out[--m] = 0;
    }
    fclose(f);
}

void host_version_text(const char *game_dir, char *buf, size_t n)
{
    char rel[64];
    release_name(game_dir, rel, sizeof rel);
    snprintf(buf, n, "%s (game %s, toolkit %s)", rel[0] ? rel : "dev build", XR_GAME_COMMIT, XR_TOOLKIT_COMMIT);
}

/* what the environment says about the machine around a Wine / Linux process */
static void env_notes(char *buf, size_t n)
{
    const char *deck = getenv("SteamDeck");
    const char *gs = getenv("GAMESCOPE_WAYLAND_DISPLAY");
    const char *de = getenv("XDG_CURRENT_DESKTOP");
    size_t k = strlen(buf);
    int first = 1;
    if (deck && !strcmp(deck, "1")) {
        k += (size_t)snprintf(buf + k, k < n ? n - k : 0, "%sSteam Deck", first ? "; " : ", ");
        first = 0;
    }
    if (gs && *gs) {
        k += (size_t)snprintf(buf + k, k < n ? n - k : 0, "%sGamescope", first ? "; " : ", ");
        first = 0;
    } else if (de && *de) {
        k += (size_t)snprintf(buf + k, k < n ? n - k : 0, "%sdesktop %s", first ? "; " : ", ", de);
        first = 0;
    }
}

void host_describe(char *buf, size_t n)
{
#if defined(_WIN32)
    typedef const char *(*wine_str_fn)(void);
    typedef void (*wine_host_fn)(const char **sysname, const char **release);
    typedef LONG (WINAPI *rtl_ver_fn)(OSVERSIONINFOW *);
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    wine_str_fn wver = nt ? (wine_str_fn)(void *)GetProcAddress(nt, "wine_get_version") : NULL;
    if (wver) {
        wine_str_fn wbuild = (wine_str_fn)(void *)GetProcAddress(nt, "wine_get_build_id");
        wine_host_fn whost = (wine_host_fn)(void *)GetProcAddress(nt, "wine_get_host_version");
        const char *sys = NULL, *rel = NULL;
        if (whost)
            whost(&sys, &rel);
        snprintf(buf, n, "Wine %s (%s) on %s %s", wver(), wbuild ? wbuild() : "?", sys ? sys : "?", rel ? rel : "");
        env_notes(buf, n);
        return;
    }
    {
        OSVERSIONINFOW v;
        rtl_ver_fn rtl = nt ? (rtl_ver_fn)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
        SYSTEM_INFO si;
        memset(&v, 0, sizeof v);
        v.dwOSVersionInfoSize = sizeof v;
        GetNativeSystemInfo(&si);
        if (rtl && rtl(&v) == 0)
            snprintf(buf, n, "Windows %lu.%lu.%lu %s", (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion,
                     (unsigned long)v.dwBuildNumber,
                     si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64" : "x64");
        else
            snprintf(buf, n, "Windows (version unknown)");
    }
#else
    struct utsname u;
    if (uname(&u) == 0)
        snprintf(buf, n, "%s %s %s", u.sysname, u.release, u.machine);
    else
        snprintf(buf, n, "unknown");
    env_notes(buf, n);
#endif
}

void host_log_version_and_host(const char *game_dir)
{
    char v[160], h[320];
    host_version_text(game_dir, v, sizeof v);
    host_describe(h, sizeof h);
    printf("version: %s\nhost: %s\n", v, h);
}
