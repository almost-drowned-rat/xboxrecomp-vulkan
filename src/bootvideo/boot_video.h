/*
 * boot_video.h -- the optional boot animation of a port (porting guide M6,
 * "Boot animation"; the same for every port).
 *
 * The player puts media/boot.mpg (MPEG-1 video + MP2 audio, the README gives
 * the one-time conversion with ffmpeg) into the port's media folder; it plays
 * before the title starts and **cannot be skipped** - the one video of a port
 * that is never skippable (maintainer, 2026-10-27; the title's own movies keep
 * their skip rules). Decoded on the
 * CPU with pl_mpeg (MIT, third_party/pl_mpeg), audio through SDL3, the frames
 * handed to the renderer (BootVideoSink; nv2a_vk: nv2a_vk_boot_frame).
 *
 * media/ is the user's folder: nothing here writes, converts or deletes a file
 * there. A boot.mp4 / boot.mkv without a boot.mpg is only reported (convert),
 * so the port can say how to convert it.
 */
#ifndef BOOT_VIDEO_H
#define BOOT_VIDEO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* one decoded frame, B8G8R8A8, w x h, rows top first (copy it: the buffer
     * is reused); return 0 to stop (no window to show it in) */
    int (*frame)(const uint8_t *bgra, int w, int h, void *user);
    void *user;
} BootVideoSink;

enum { BOOT_VIDEO_NOT_PLAYED, BOOT_VIDEO_PLAYED, BOOT_VIDEO_STOPPED };

/* Plays path (UTF-8) to the end or until the sink stops it; returns one of the
 * values above (NOT_PLAYED: missing, unreadable or no MPEG-1 video stream --
 * the reason is logged). Blocks; call it before the title starts. */
int boot_video_play(const char *path, const BootVideoSink *sink);

/* The boot file in media_dir: 1 and its path in out when media_dir/boot.mpg
 * exists, else 0. *convert (may be NULL) = 1 when boot.mp4 or boot.mkv is
 * there but no boot.mpg (the port shows how to convert it). */
int boot_video_find(const char *media_dir, char *out, size_t n, int *convert);

#ifdef __cplusplus
}
#endif

#endif
