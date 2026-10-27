/*
 * xdk_soundtrack.h -- the XDK's custom-soundtrack API over the port's media\
 * folder, and a music media object that plays the user's files.
 *
 * On a console the dashboard rips CDs into E:\TDATA\FFFE0000\MUSIC (ST.DB +
 * WMA files); titles list them with XFindFirstSoundtrack / XFindNextSoundtrack
 * / XGetSoundtrackSongInfo, open a song with XOpenSoundtrackSong and decode
 * the handle with the WMA file media object (WmaCreateDecoderEx). Here the
 * albums of media\ (src/media/media_scan.h) are the soundtracks, and the
 * decoder a title creates for one of their songs is a host object: its
 * methods decode MP3/FLAC/WAV/Vorbis to 16-bit stereo PCM at 44100 Hz. With a
 * playlist (src/media/media_playlist.h) it moves on to the next song at the
 * end of one -- game tracks included, through a source the port provides --
 * so the title's stream never sees a song end; without one it ends like the
 * title's own WMA object (a short packet), and the title decides.
 *
 * Bound per title by address, like xdk_dsound.h: arguments on the guest
 * stack at g_esp + 4, result in g_eax, stdcall arguments popped here. The
 * media object's methods are bound through the title's own WMA object vtable
 * (the object starts with it); xdk_music_owns() tells the title's method
 * overrides which objects are host ones.
 *
 * IDs: soundtrack id = album index + 1, song id = soundtrack id << 16 | track.
 * Song lengths are reported as 0 (unknown) -- no file is opened to list.
 * RECOMP_SOUNDTRACK_HLE=0: the title's own code (dashboard DB: none found).
 *
 * Reference design (not code): NoRain211/doaxvb-re docs/custom-soundtracks.md
 * (GPL-3.0); written new for this toolkit.
 */
#ifndef XDK_SOUNDTRACK_H
#define XDK_SOUNDTRACK_H

#include <stdint.h>

#include "media_playlist.h"
#include "media_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Scans media_dir once (later calls rescan). max_songs: what the title's
 * soundtrack screens hold per soundtrack (XAPI allows 500; THPS 2X 128) --
 * the catalog shows that many, playlists use every track. */
void xdk_soundtrack_init(const char *media_dir, int max_songs);
const MediaLibrary *xdk_soundtrack_library(void);
int  xdk_soundtrack_active(void);

void xdk_XFindFirstSoundtrack(void);    /* (pData)                            4 */
void xdk_XFindNextSoundtrack(void);     /* (hFind, pData)                     8 */
/* (hFind) 4: 1 = an enumeration of ours, closed and popped; 0 = not ours
 * (the title's XFindClose handles it, stack untouched). */
int  xdk_XFindClose_ours(void);
void xdk_XGetSoundtrackSongInfo(void);  /* (st, index, pId, pMs, name, cch)  24 */
void xdk_XOpenSoundtrackSong(void);     /* (songId, fAsync)                   8 */

/* A handle from xdk_XOpenSoundtrackSong: 1 and its song. */
int  xdk_soundtrack_song_of_handle(uint32_t handle, MediaSong *song);

/* ── Music media object ─────────────────────────────────────────────── */

/* PCM at 44100 Hz, 2 channels, s16 interleaved. read: frames written, 0 at
 * the end, -1 on an error. */
typedef struct {
    void *ctx;
    long (*read)(void *ctx, int16_t *out, long frames);
    int  (*rewind)(void *ctx);
    void (*close)(void *ctx);
} XdkMusicSource;

/* The port's game tracks (index as in media_playlist.h): 1 and a source. */
typedef int (*XdkOpenGameTrack)(int track, XdkMusicSource *src);

/* vtable: the title's WMA file media object vtable (11 methods: AddRef,
 * Release, GetInfo, GetStatus, Process, Discontinuity, Flush, Seek,
 * GetLength, GetFormat, DoWork). */
void xdk_music_bind(uint32_t vtable, XdkOpenGameTrack open_game);

/* A host media object that starts with `first`. pl: the playlist to follow
 * at the end of a song (taken over; NULL = this song only). pwfx: the
 * caller's WAVEFORMATEX, filled with the PCM format (0 = none). Returns the
 * guest object, 0 when no song of the list can be opened. */
uint32_t xdk_music_create(MediaSong first, MediaPlaylist *pl, uint32_t pwfx);
int  xdk_music_owns(uint32_t obj);
/* The newest live music object: 1 and the song it plays (a title's "now
 * playing" display); has_playlist: whether it follows a playlist. */
int  xdk_music_current(MediaSong *song, int *has_playlist);
/* The newest live music object with a playlist moves on to its next song at
 * once (a "next song" entry). 1 when done; 0 when there is none -- the port
 * then asks the title for its next track itself. */
int  xdk_music_skip(void);
/* The newest live music object with a playlist plays `song` at once and
 * follows its playlist from there (a "select track" entry; a shuffled list
 * goes on in its own order). 1 when done; 0 when there is none. */
int  xdk_music_jump(MediaSong song);

void xdk_music_AddRef(void);            /* (this)                    4 */
void xdk_music_Release(void);           /* (this)                    4 */
void xdk_music_GetInfo(void);           /* (this, pInfo)             8 */
void xdk_music_GetStatus(void);         /* (this, pdwStatus)         8 */
void xdk_music_Process(void);           /* (this, pIn, pOut)        12 */
void xdk_music_Discontinuity(void);     /* (this)                    4 */
void xdk_music_Flush(void);             /* (this)                    4 */
void xdk_music_Seek(void);              /* (this, off, origin, pAbs) 16 */
void xdk_music_GetLength(void);         /* (this, pdwLength)         8 */
void xdk_music_GetFormat(void);         /* (this, ppwfx)             8 */
void xdk_music_DoWork(void);            /* (this, unused)            8 */

#ifdef __cplusplus
}
#endif

#endif
