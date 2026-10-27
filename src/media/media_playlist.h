/*
 * media_playlist.h -- the order songs play in, for the soundtrack choice and
 * playback mode of the port's SOUNDTRACKS options entry (porting guide, M6).
 *
 * A song is a game track (the title's own music, index 0..ngame-1) or a
 * track of a custom album (media_scan.h). The choice ("SOUNDTRACK") is game
 * music, COMBINED (game music, then every custom album) or one album; modes:
 *
 *   MEDIA_GAME_ORDER     the title's own behaviour -- the port does not use a
 *                        playlist for it (the list is the choice in order)
 *   MEDIA_ALBUM_ORDER    the choice in order
 *   MEDIA_GAME_SHUFFLE   game music shuffled
 *   MEDIA_ALBUM_SHUFFLE  the choice shuffled
 *   MEDIA_CUSTOM_SHUFFLE every custom album, one shuffled pool
 *   MEDIA_ALL_SHUFFLE    game music and every custom album, shuffled
 *   MEDIA_ALL_ORDER      game music, then every custom album, in order
 *   MEDIA_CUSTOM_ORDER   every custom album, one after another
 *
 * A pool that comes out empty (no custom albums) falls back to game music in
 * order. Shuffle: a full pass through the pool without repeats, reshuffled
 * at the wrap so the last song of a pass is not the first of the next.
 */
#ifndef MEDIA_PLAYLIST_H
#define MEDIA_PLAYLIST_H

#include <stdint.h>

#include "media_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MEDIA_GAME_ORDER = 0,
    MEDIA_ALBUM_ORDER,
    MEDIA_GAME_SHUFFLE,
    MEDIA_ALBUM_SHUFFLE,
    MEDIA_CUSTOM_SHUFFLE,
    MEDIA_ALL_SHUFFLE,
    MEDIA_ALL_ORDER,
    MEDIA_CUSTOM_ORDER,
    MEDIA_MODE_COUNT
} MediaPlayMode;

#define MEDIA_SEL_GAME     (-1)         /* the choice: game music */
#define MEDIA_SEL_COMBINED (-2)         /* the choice: game music + every custom album */

typedef struct {
    int album;                          /* -1: game track; else the album index */
    int track;
} MediaSong;

typedef struct {
    MediaSong *items;
    int        n, pos;
    int        shuffle;
    uint32_t   rng;
} MediaPlaylist;

/* Builds the list for mode + choice (sel: MEDIA_SEL_* or an album index).
 * Returns the song count (>= 1 when ngame >= 1). */
int  media_playlist_build(MediaPlaylist *p, const MediaLibrary *lib, int ngame,
                          MediaPlayMode mode, int sel, uint32_t seed);
void media_playlist_free(MediaPlaylist *p);
/* Positions at `s` when it is in the list (the title's own track choice);
 * otherwise stays. Returns the current song. */
const MediaSong *media_playlist_seek(MediaPlaylist *p, MediaSong s);
const MediaSong *media_playlist_current(const MediaPlaylist *p);
/* The next song (wraps; a shuffled list is reshuffled at the wrap). */
const MediaSong *media_playlist_next(MediaPlaylist *p);

const char *media_mode_name(MediaPlayMode m);

#ifdef __cplusplus
}
#endif

#endif
