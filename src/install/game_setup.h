/*
 * game_setup.h - a port's first start: the game's files into <layout>/game/.
 *
 * The player puts either the extracted disc (default.xbe and its folders
 * directly in game/) or one disc image (.iso / .xiso) into game/. At start the
 * port calls xr_game_setup before anything else:
 *
 *   game/default.xbe there        ready (the image, if any, is left alone)
 *   one image in game/            its files are extracted into game/.extracting/,
 *                                 every file checked, moved up into game/
 *                                 (default.xbe last: it marks a whole install),
 *                                 then the image is deleted
 *   default.xbe one folder down   reported (the folder must be flat)
 *   several images, none, a       reported, nothing written
 *   damaged or foreign image
 *
 * The check (maintainer, 2026-10-24): every extracted file is compared with
 * its directory entry in the image -- its size, and a CRC-32 taken while the
 * file is written and taken again by reading the written file back. Any
 * mismatch keeps the image and names the file. There is no journal: a run
 * that stopped half way starts over (game/.extracting/ is cleared first).
 * Version rules (maintainer, 2026-10-24; replace "unknown version: warn"):
 * default.xbe is extracted and checked first, before any other file (fail
 * fast): its certificate's title ID must be the port's (else "This is not
 * the <game> image. Please provide the correct game.") and its SHA-1 one of
 * the supported versions (else "This <game> version is not supported.
 * Supported version(s): <names>. Please provide a supported version."). On
 * failure the image is kept and game/.extracting/ cleared. The same rules
 * apply to an extracted folder (game/default.xbe already there).
 *
 * It only ever touches game/: never media/, saves/ or data/, nothing outside
 * the layout. Paths are UTF-8. No thread, no global state; the caller shows
 * the progress (game_setup_ui.h for an SDL3 window).
 *
 * The image reader is xdvdfs.c (takedawn-xboxrecomp, MIT); the flow is ours.
 */
#ifndef XR_GAME_SETUP_H
#define XR_GAME_SETUP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XR_SETUP_READY = 0,         /* game/default.xbe was there */
    XR_SETUP_INSTALLED,         /* extracted from an image just now */
    XR_SETUP_NO_GAME,           /* game/ holds neither default.xbe nor an image */
    XR_SETUP_NESTED,            /* default.xbe one folder down: not flat */
    XR_SETUP_SEVERAL_IMAGES,    /* more than one image in game/ */
    XR_SETUP_BAD_IMAGE,         /* not an Xbox image, truncated or damaged */
    XR_SETUP_NO_XBE,            /* an Xbox image without default.xbe */
    XR_SETUP_DISK_FULL,
    XR_SETUP_WRITE_FAILED,      /* creating, writing or moving a file failed */
    XR_SETUP_VERIFY_FAILED,     /* a written file differs from the image */
    XR_SETUP_CANCELLED,
    XR_SETUP_INTERNAL,          /* out of memory, a path too long */
    XR_SETUP_WRONG_GAME,        /* default.xbe's title ID is not the port's */
    XR_SETUP_UNSUPPORTED        /* the right game, a default.xbe SHA-1 the port does not support */
};

/* A version the port supports: its name for the player and default.xbe's SHA-1. */
typedef struct {
    const char *name;                   /* "USA" */
    const char *xbe_sha1;               /* 40 hex digits */
} xr_game_version;

typedef struct {
    const char *game_dir;               /* <layout>/game, UTF-8 */
    /* The port's config (porting_guide.md M5): */
    const char *game;                   /* "THPS 2X": the name in the messages */
    uint32_t    title_id;               /* expected title ID (XBE certificate), e.g. 0x41560001; 0 = no check */
    const xr_game_version *versions;    /* the supported versions */
    unsigned    version_count;          /* 0 = any version of the right game */
    void       *ctx;
    void      (*progress)(void *ctx, double fraction, const char *text);
    void      (*log)(void *ctx, const char *text);
    int       (*cancelled)(void *ctx);  /* asked between blocks of 1 MiB */
    uint64_t    test_free_bytes;        /* tests only: the drive's free space instead of asking the
                                           OS (0 = ask) -- the disk-full path without a full disk */
    const char *image_path;             /* a disc image somewhere else (the launcher's "Choose disc image..."):
                                           installed from there into game_dir (created if missing) when game_dir
                                           has no default.xbe yet, and never deleted - it is the player's file */
} xr_game_setup_opts;

typedef struct {
    int      code;
    char     message[1024];             /* a sentence for the player, for every code */
    char     image[260];                /* the image's file name (INSTALLED, or the failing one) */
    char     xbe_sha1[41];              /* of default.xbe, when it could be read */
    uint32_t title_id;                  /* from default.xbe's certificate (0: no XBE header) */
    const char *version;                /* the supported version it is, or NULL */
    int      image_kept;                /* INSTALLED, but the image could not be deleted */
    unsigned files;
    uint64_t bytes;
} xr_game_setup_result;

/* The check and, when game/ holds one image, the install. Returns r->code. */
int xr_game_setup(const xr_game_setup_opts *o, xr_game_setup_result *r);

/* The code as a word for logs: "ready", "installed", "several-images" ... */
const char *xr_game_setup_code_name(int code);

#ifdef __cplusplus
}
#endif

#endif /* XR_GAME_SETUP_H */
