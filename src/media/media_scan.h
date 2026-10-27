/*
 * media_scan.h -- the user's custom soundtracks in the port's media\ folder.
 *
 * Rules (porting guide, "Media folder"; same for every port): every folder
 * below media\ that holds music files itself is one soundtrack ("album"),
 * named after that folder; music directly in media\ is not used. Albums are
 * sorted naturally by their path below media\. Two albums with the same
 * folder name get the parent folder in front ("Artist A - CD1"). Track order:
 * an .m3u / .m3u8 in the folder (entries relative to the folder, missing
 * entries skipped, files it does not list follow sorted naturally); without
 * one, the files sorted naturally by name. At most MEDIA_MAX_TRACKS per album.
 * Formats: MP3, FLAC, WAV, Ogg Vorbis. AAC (.m4a/.aac) is skipped with a log
 * line until the AAC decoder is chosen (toolkit decision, with the boot video).
 *
 * Read only: the scan never writes, moves or deletes anything below media\.
 * Paths are UTF-8; on Windows the scan uses the wide-character API.
 */
#ifndef MEDIA_SCAN_H
#define MEDIA_SCAN_H

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_MAX_TRACKS 500

typedef struct {
    char *path;                 /* full path, UTF-8 */
    char *name;                 /* file name without extension (display) */
} MediaTrack;

typedef struct {
    char       *name;           /* display name (folder, or "parent - folder") */
    char       *rel;            /* path below media\, '/' separators: the album's stable key */
    MediaTrack *tracks;
    int         ntracks;
} MediaAlbum;

typedef struct {
    MediaAlbum *albums;
    int         nalbums;
} MediaLibrary;

/* Scans media_dir (missing folder = no albums). Returns the album count;
 * lines for skipped files and folders go to stderr ("[MEDIA] ..."). */
int  media_scan(const char *media_dir, MediaLibrary *lib);
void media_free(MediaLibrary *lib);

/* Album index by its path below media\ (case-insensitive), or -1. */
int  media_find_album(const MediaLibrary *lib, const char *rel);

/* Natural order: case-insensitive, digit runs compared by value
 * ("Track 2" < "Track 10"). */
int  media_natural_cmp(const char *a, const char *b);

/* 1 for a file name the decoders play (by extension). */
int  media_is_playable(const char *name);

#ifdef __cplusplus
}
#endif

#endif
