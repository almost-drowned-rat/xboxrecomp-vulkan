/* media_playlist.c -- song order for a soundtrack choice and mode (see media_playlist.h). */

#include "media_playlist.h"

#include <stdlib.h>
#include <string.h>

static uint32_t rnd(uint32_t *s)
{
    /* xorshift32; never 0 */
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

static void shuffle(MediaPlaylist *p)
{
    int i;
    for (i = p->n - 1; i > 0; i--) {
        int j = (int)(rnd(&p->rng) % (uint32_t)(i + 1));
        MediaSong t = p->items[i];
        p->items[i] = p->items[j];
        p->items[j] = t;
    }
}

static int push(MediaPlaylist *p, int *cap, int album, int track)
{
    if (p->n == *cap) {
        MediaSong *ni = (MediaSong *)realloc(p->items, (size_t)(*cap = *cap ? *cap * 2 : 64) * sizeof *ni);
        if (!ni)
            return 0;
        p->items = ni;
    }
    p->items[p->n].album = album;
    p->items[p->n].track = track;
    p->n++;
    return 1;
}

static void add_game(MediaPlaylist *p, int *cap, int ngame)
{
    int i;
    for (i = 0; i < ngame; i++)
        push(p, cap, -1, i);
}

static void add_album(MediaPlaylist *p, int *cap, const MediaLibrary *lib, int a)
{
    int i;
    if (a < 0 || a >= lib->nalbums)
        return;
    for (i = 0; i < lib->albums[a].ntracks; i++)
        push(p, cap, a, i);
}

static void add_custom(MediaPlaylist *p, int *cap, const MediaLibrary *lib)
{
    int a;
    for (a = 0; a < lib->nalbums; a++)
        add_album(p, cap, lib, a);
}

/* The choice as a pool: game music, COMBINED, or one album. */
static void add_choice(MediaPlaylist *p, int *cap, const MediaLibrary *lib, int ngame, int sel)
{
    if (sel == MEDIA_SEL_COMBINED) {
        add_game(p, cap, ngame);
        add_custom(p, cap, lib);
    } else if (sel >= 0) {
        add_album(p, cap, lib, sel);
    } else {
        add_game(p, cap, ngame);
    }
}

int media_playlist_build(MediaPlaylist *p, const MediaLibrary *lib, int ngame,
                         MediaPlayMode mode, int sel, uint32_t seed)
{
    int cap = 0;
    memset(p, 0, sizeof *p);
    p->rng = seed ? seed : 1;
    switch (mode) {
    case MEDIA_ALBUM_ORDER:
    case MEDIA_ALBUM_SHUFFLE:  add_choice(p, &cap, lib, ngame, sel); break;
    case MEDIA_GAME_SHUFFLE:   add_game(p, &cap, ngame); break;
    case MEDIA_CUSTOM_SHUFFLE:
    case MEDIA_CUSTOM_ORDER:   add_custom(p, &cap, lib); break;
    case MEDIA_ALL_SHUFFLE:
    case MEDIA_ALL_ORDER:      add_game(p, &cap, ngame); add_custom(p, &cap, lib); break;
    default:                   add_choice(p, &cap, lib, ngame, sel); break;
    }
    if (!p->n)
        add_game(p, &cap, ngame);               /* nothing custom: game music */
    p->shuffle = mode == MEDIA_GAME_SHUFFLE || mode == MEDIA_ALBUM_SHUFFLE
              || mode == MEDIA_CUSTOM_SHUFFLE || mode == MEDIA_ALL_SHUFFLE;
    if (p->shuffle)
        shuffle(p);
    return p->n;
}

void media_playlist_free(MediaPlaylist *p)
{
    free(p->items);
    memset(p, 0, sizeof *p);
}

const MediaSong *media_playlist_current(const MediaPlaylist *p)
{
    return p->n ? &p->items[p->pos] : NULL;
}

const MediaSong *media_playlist_seek(MediaPlaylist *p, MediaSong s)
{
    int i;
    for (i = 0; i < p->n; i++)
        if (p->items[i].album == s.album && p->items[i].track == s.track) {
            p->pos = i;
            break;
        }
    return media_playlist_current(p);
}

const MediaSong *media_playlist_next(MediaPlaylist *p)
{
    if (!p->n)
        return NULL;
    if (++p->pos < p->n)
        return &p->items[p->pos];
    p->pos = 0;
    if (p->shuffle && p->n > 1) {
        MediaSong last = p->items[p->n - 1];
        shuffle(p);
        if (p->items[0].album == last.album && p->items[0].track == last.track) {
            MediaSong t = p->items[0];
            int j = 1 + (int)(rnd(&p->rng) % (uint32_t)(p->n - 1));
            p->items[0] = p->items[j];
            p->items[j] = t;
        }
    }
    return &p->items[0];
}

const char *media_mode_name(MediaPlayMode m)
{
    static const char *const names[MEDIA_MODE_COUNT] = {
        "game music in order (original)", "album in order", "shuffle game music", "shuffle album",
        "shuffle all custom albums", "shuffle all", "all in order", "all custom albums in order",
    };
    return (unsigned)m < MEDIA_MODE_COUNT ? names[m] : "?";
}
