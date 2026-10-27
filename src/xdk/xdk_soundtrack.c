/*
 * xdk_soundtrack.c -- the XDK custom-soundtrack API over media\ and a host
 * music media object (see xdk_soundtrack.h).
 *
 * Layouts (XDK headers, checked against THPS 2X's soundtrack screen):
 *   XSOUNDTRACK_DATA  +0 uSoundtrackId, +4 uSongCount, +8 uSoundtrackLength (ms),
 *                     +0xC szName[32] (WCHAR)                       (0x4C bytes)
 *   XMEDIAINFO        +0 dwFlags, +4 dwInputSize, +8 dwOutputSize, +0xC dwMaxLookahead
 *   XMEDIAPACKET      +0 pvBuffer, +4 dwMaxSize, +8 pdwCompletedSize,
 *                     +0xC pdwStatus, +0x10 hCompletionEvent, +0x14 prtTimestamp
 *   WAVEFORMATEX      +0 wFormatTag, +2 nChannels, +4 nSamplesPerSec,
 *                     +8 nAvgBytesPerSec, +0xC nBlockAlign, +0xE wBitsPerSample, +0x10 cbSize
 *
 * A music object is a small block of guest memory: +0 the title's WMA object
 * vtable, +4 a magic, +8 the host slot, +0x10 its WAVEFORMATEX (what
 * GetFormat hands out). Everything else lives in the slot.
 */

#include "xdk_soundtrack.h"

#include "kernel.h"
#include "media_decode.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;

/* ---- guest memory ------------------------------------------------------ */

static uint8_t *gp(uint32_t va) { return (uint8_t *)((uintptr_t)va + g_xbox_mem_offset); }
static uint32_t r32(uint32_t va)
{
    const uint8_t *p = gp(va);
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void w16(uint32_t va, uint16_t v) { uint8_t *p = gp(va); p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint32_t va, uint32_t v)
{
    uint8_t *p = gp(va);
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

#define ARG(n) r32(g_esp + 4u + 4u * (n))
#define RETURN(v, nargs) do { g_eax = (uint32_t)(v); g_esp += 4u + 4u * (nargs); return; } while (0)

#define S_OK_         0u
#define E_FAIL_       0x80004005u
#define INVALID_H     0xFFFFFFFFu
#define MAX_ST        100u              /* XAPI: at most 100 soundtracks */
#define FIND_BASE     0x5D000000u       /* our enumeration handles: FIND_BASE | slot */
#define SONG_BASE     0x5E000000u       /* our song handles: SONG_BASE | song id */
#define MAX_FIND      8

/* ---- catalog ----------------------------------------------------------- */

static MediaLibrary s_lib;
static int s_inited, s_max_songs = 500;
static int s_find[MAX_FIND];            /* 0 free, else 1 + the album shown last */

void xdk_soundtrack_init(const char *media_dir, int max_songs)
{
    int i;
    media_free(&s_lib);
    media_scan(media_dir, &s_lib);
    if (s_lib.nalbums > (int)MAX_ST) {
        fprintf(stderr, "[MEDIA] %d soundtracks: the first %u are listed\n", s_lib.nalbums, MAX_ST);
    }
    s_max_songs = max_songs > 0 && max_songs < MEDIA_MAX_TRACKS ? max_songs : MEDIA_MAX_TRACKS;
    s_inited = 1;
    fprintf(stderr, "[MEDIA] %s: %d soundtrack(s)\n", media_dir, s_lib.nalbums);
    for (i = 0; i < s_lib.nalbums; i++)
        fprintf(stderr, "[MEDIA]   %d: \"%s\" (%s), %d track(s)%s\n", i + 1, s_lib.albums[i].name,
                s_lib.albums[i].rel, s_lib.albums[i].ntracks,
                s_lib.albums[i].ntracks > s_max_songs ? " -- the title's soundtrack screen lists the first ones" : "");
}

const MediaLibrary *xdk_soundtrack_library(void) { return &s_lib; }

int xdk_soundtrack_active(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_SOUNDTRACK_HLE");
        on = !(e && *e == '0');
    }
    return on && s_inited;
}

static int nst(void) { return s_lib.nalbums < (int)MAX_ST ? s_lib.nalbums : (int)MAX_ST; }

static int nsongs(int a)
{
    return s_lib.albums[a].ntracks < s_max_songs ? s_lib.albums[a].ntracks : s_max_songs;
}

/* UTF-8 to UTF-16 into guest memory, at most cch units including the 0. */
static void put_wname(uint32_t va, uint32_t cch, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    uint32_t n = 0;
    if (!va || !cch)
        return;
    while (*p && n + 1 < cch) {
        uint32_t c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2]) {
            c = (c & 7) << 18 | (p[0] & 0x3Fu) << 12 | (p[1] & 0x3Fu) << 6 | (p[2] & 0x3Fu); p += 3;
        } else if (c >= 0xE0 && p[0] && p[1]) {
            c = (c & 0xF) << 12 | (p[0] & 0x3Fu) << 6 | (p[1] & 0x3Fu); p += 2;
        } else if (c >= 0xC0 && p[0]) {
            c = (c & 0x1F) << 6 | (p[0] & 0x3Fu); p += 1;
        } else if (c >= 0x80) {
            c = '?';
        }
        if (c >= 0x10000) {
            if (n + 2 >= cch)
                break;
            c -= 0x10000;
            w16(va + 2 * n++, (uint16_t)(0xD800 | c >> 10));
            w16(va + 2 * n++, (uint16_t)(0xDC00 | (c & 0x3FF)));
        } else {
            w16(va + 2 * n++, (uint16_t)c);
        }
    }
    w16(va + 2 * n, 0);
}

static void fill_st(uint32_t data, int a)
{
    w32(data + 0, (uint32_t)a + 1);
    w32(data + 4, (uint32_t)nsongs(a));
    w32(data + 8, 0);
    put_wname(data + 0xC, 32, s_lib.albums[a].name);
}

void xdk_XFindFirstSoundtrack(void)
{
    uint32_t data = ARG(0);
    int i;
    if (!data || !nst())
        RETURN(INVALID_H, 1);
    for (i = 0; i < MAX_FIND; i++)
        if (!s_find[i])
            break;
    if (i == MAX_FIND)
        RETURN(INVALID_H, 1);
    s_find[i] = 1;
    fill_st(data, 0);
    RETURN(FIND_BASE | (uint32_t)i, 1);
}

static int find_slot(uint32_t h)
{
    uint32_t i = h - FIND_BASE;
    return (h & 0xFF000000u) == FIND_BASE && i < MAX_FIND && s_find[i] ? (int)i : -1;
}

void xdk_XFindNextSoundtrack(void)
{
    int i = find_slot(ARG(0));
    uint32_t data = ARG(1);
    if (i < 0 || !data || s_find[i] >= nst())
        RETURN(0, 2);
    fill_st(data, s_find[i]);
    s_find[i]++;
    RETURN(1, 2);
}

int xdk_XFindClose_ours(void)
{
    int i = find_slot(ARG(0));
    if (i < 0)
        return 0;
    s_find[i] = 0;
    g_eax = 1;
    g_esp += 8;
    return 1;
}

void xdk_XGetSoundtrackSongInfo(void)
{
    int a = (int)ARG(0) - 1, t = (int)ARG(1);
    uint32_t pid = ARG(2), pms = ARG(3), name = ARG(4), cch = ARG(5);
    if (a < 0 || a >= nst() || t < 0 || t >= nsongs(a))
        RETURN(0, 6);
    if (pid)
        w32(pid, (uint32_t)(a + 1) << 16 | (uint32_t)t);
    if (pms)
        w32(pms, 0);
    put_wname(name, cch, s_lib.albums[a].tracks[t].name);
    RETURN(1, 6);
}

static int song_of_id(uint32_t id, MediaSong *s)
{
    int a = (int)(id >> 16) - 1, t = (int)(id & 0xFFFF);
    if (a < 0 || a >= s_lib.nalbums || t >= s_lib.albums[a].ntracks)
        return 0;
    s->album = a;
    s->track = t;
    return 1;
}

void xdk_XOpenSoundtrackSong(void)
{
    uint32_t id = ARG(0);
    MediaSong s;
    if (id >= 0x01000000u || !song_of_id(id, &s))
        RETURN(INVALID_H, 2);
    RETURN(SONG_BASE | id, 2);
}

int xdk_soundtrack_song_of_handle(uint32_t h, MediaSong *song)
{
    return (h & 0xFF000000u) == SONG_BASE && song_of_id(h & 0x00FFFFFFu, song);
}

/* ---- music media object -------------------------------------------------- */

#define MUS_MAGIC  0x53554D58u          /* 'XMUS' */
#define MAX_MUS    8
#define OBJ_SIZE   0x40

typedef struct {
    uint32_t       obj;
    int            ref;
    int            has_pl;
    MediaPlaylist  pl;
    MediaSong      cur;
    XdkMusicSource src;
    int            open;                /* src is valid */
    int            silent;              /* single song that failed: silence */
} Music;

static Music s_mus[MAX_MUS];
static int s_newest = -1;               /* the slot created last */
static uint32_t s_vtable;
static XdkOpenGameTrack s_open_game;

void xdk_music_bind(uint32_t vtable, XdkOpenGameTrack open_game)
{
    s_vtable = vtable;
    s_open_game = open_game;
}

static long dec_read(void *c, int16_t *out, long n) { return media_decoder_read((MediaDecoder *)c, out, n); }
static int  dec_rewind(void *c) { return media_decoder_rewind((MediaDecoder *)c); }
static void dec_close(void *c) { media_decoder_close((MediaDecoder *)c); }

static void song_name(MediaSong s, char *out, size_t n)
{
    if (s.album < 0)
        snprintf(out, n, "game track %d", s.track + 1);
    else
        snprintf(out, n, "\"%s\" / \"%s\"", s_lib.albums[s.album].name, s_lib.albums[s.album].tracks[s.track].name);
}

static int open_song(Music *m, MediaSong s)
{
    char nm[600];
    if (m->open && m->src.close)
        m->src.close(m->src.ctx);
    m->open = 0;
    m->cur = s;
    if (s.album < 0) {
        if (s_open_game && s_open_game(s.track, &m->src))
            m->open = 1;
    } else if (s.album < s_lib.nalbums && s.track < s_lib.albums[s.album].ntracks) {
        MediaDecoder *d = media_decoder_open(s_lib.albums[s.album].tracks[s.track].path);
        if (d) {
            m->src.ctx = d;
            m->src.read = dec_read;
            m->src.rewind = dec_rewind;
            m->src.close = dec_close;
            m->open = 1;
        }
    }
    song_name(s, nm, sizeof nm);
    fprintf(stderr, m->open ? "[MUSIC] playing %s\n" : "[MUSIC] %s cannot be played\n", nm);
    return m->open;
}

/* The playlist's next song that opens (at most one pass); 0 if none does. */
static int advance(Music *m)
{
    int k;
    for (k = 0; k < m->pl.n; k++) {
        const MediaSong *s = media_playlist_next(&m->pl);
        if (s && open_song(m, *s))
            return 1;
    }
    return 0;
}

static Music *music_of(uint32_t obj)
{
    uint32_t i;
    if (!obj || r32(obj + 4) != MUS_MAGIC)
        return NULL;
    i = r32(obj + 8);
    return i < MAX_MUS && s_mus[i].obj == obj ? &s_mus[i] : NULL;
}

int xdk_music_owns(uint32_t obj) { return music_of(obj) != NULL; }

static void put_format(uint32_t w)
{
    w16(w + 0, 1);                      /* WAVE_FORMAT_PCM */
    w16(w + 2, 2);
    w32(w + 4, MEDIA_RATE);
    w32(w + 8, MEDIA_RATE * 4u);
    w16(w + 0xC, 4);
    w16(w + 0xE, 16);
    w16(w + 0x10, 0);
}

uint32_t xdk_music_create(MediaSong first, MediaPlaylist *pl, uint32_t pwfx)
{
    Music *m = NULL;
    int i;
    for (i = 0; i < MAX_MUS; i++)
        if (!s_mus[i].obj) {
            m = &s_mus[i];
            break;
        }
    if (!m || !s_vtable) {
        if (pl)
            media_playlist_free(pl);
        return 0;
    }
    memset(m, 0, sizeof *m);
    if (pl) {
        m->pl = *pl;
        m->has_pl = 1;
        memset(pl, 0, sizeof *pl);
        media_playlist_seek(&m->pl, first);
    }
    if (!open_song(m, first) && !(m->has_pl && advance(m))) {
        if (!m->has_pl) {
            m->silent = 1;                  /* a single song that does not open: silence, like an empty track */
        } else {
            media_playlist_free(&m->pl);
            memset(m, 0, sizeof *m);
            return 0;
        }
    }
    m->obj = xbox_HeapAlloc(OBJ_SIZE, 16);
    if (!m->obj) {
        if (m->open && m->src.close)
            m->src.close(m->src.ctx);
        if (m->has_pl)
            media_playlist_free(&m->pl);
        memset(m, 0, sizeof *m);
        return 0;
    }
    memset(gp(m->obj), 0, OBJ_SIZE);
    w32(m->obj + 0, s_vtable);
    w32(m->obj + 4, MUS_MAGIC);
    w32(m->obj + 8, (uint32_t)i);
    put_format(m->obj + 0x10);
    m->ref = 1;
    if (pwfx)
        put_format(pwfx);
    s_newest = i;
    return m->obj;
}

int xdk_music_current(MediaSong *song, int *has_playlist)
{
    const Music *m = s_newest >= 0 ? &s_mus[s_newest] : NULL;
    if (!m || !m->obj)
        return 0;
    if (song)
        *song = m->cur;
    if (has_playlist)
        *has_playlist = m->has_pl;
    return 1;
}

int xdk_music_skip(void)
{
    Music *m = s_newest >= 0 ? &s_mus[s_newest] : NULL;
    if (!m || !m->obj || !m->has_pl)
        return 0;
    if (!advance(m))
        m->open = 0;                    /* nothing opens: silence until the title stops it */
    return 1;
}

int xdk_music_jump(MediaSong song)
{
    Music *m = s_newest >= 0 ? &s_mus[s_newest] : NULL;
    if (!m || !m->obj || !m->has_pl)
        return 0;
    media_playlist_seek(&m->pl, song);
    if (!open_song(m, song) && !advance(m))
        m->open = 0;
    return 1;
}

void xdk_music_AddRef(void)
{
    Music *m = music_of(ARG(0));
    RETURN(m ? (uint32_t)++m->ref : 0, 1);
}

void xdk_music_Release(void)
{
    Music *m = music_of(ARG(0));
    if (!m)
        RETURN(0, 1);
    if (--m->ref > 0)
        RETURN((uint32_t)m->ref, 1);
    if (m->open && m->src.close)
        m->src.close(m->src.ctx);
    if (m->has_pl)
        media_playlist_free(&m->pl);
    w32(m->obj + 4, 0);
    xbox_HeapFree(m->obj);
    memset(m, 0, sizeof *m);
    RETURN(0, 1);
}

void xdk_music_GetInfo(void)
{
    uint32_t info = ARG(1);
    if (info) {
        w32(info + 0, 0x1u | 0x2u);     /* XMO_STREAMF_FIXED_SAMPLE_SIZE | FIXED_PACKET_ALIGNMENT */
        w32(info + 4, 0);
        w32(info + 8, 4);               /* one stereo s16 frame */
        w32(info + 0xC, 0);
    }
    RETURN(S_OK_, 2);
}

void xdk_music_GetStatus(void)
{
    uint32_t p = ARG(1);
    if (p)
        w32(p, 0x2u);                   /* XMO_STATUSF_ACCEPT_OUTPUT_DATA */
    RETURN(S_OK_, 2);
}

void xdk_music_Process(void)
{
    Music *m = music_of(ARG(0));
    uint32_t pkt = ARG(2), buf, max, pdone, pstatus, ev;
    long want, done = 0;
    if (!m || !pkt)
        RETURN(E_FAIL_, 3);
    buf = r32(pkt + 0); max = r32(pkt + 4); pdone = r32(pkt + 8); pstatus = r32(pkt + 0xC); ev = r32(pkt + 0x10);
    want = (long)(max / 4u);
    while (done < want) {
        long got = -1;
        if (m->open)
            got = m->src.read(m->src.ctx, (int16_t *)gp(buf + (uint32_t)done * 4u), want - done);
        if (got > 0) {
            done += got;
            continue;
        }
        if (m->has_pl) {
            if (advance(m))
                continue;
        } else if (got < 0 || m->silent) {
            if (!m->silent) {
                char nm[600];
                song_name(m->cur, nm, sizeof nm);
                fprintf(stderr, "[MUSIC] %s: decode error -- silence\n", nm);
                m->silent = 1;
            }
            memset(gp(buf + (uint32_t)done * 4u), 0, (size_t)(want - done) * 4u);
            done = want;
            break;
        }
        break;                          /* end of the song: a short packet, the title decides */
    }
    if (done < want && m->has_pl) {
        /* no song of the list opens: silence, the stream stays alive */
        memset(gp(buf + (uint32_t)done * 4u), 0, (size_t)(want - done) * 4u);
        done = want;
    }
    if (pdone)
        w32(pdone, (uint32_t)done * 4u);
    if (pstatus)
        w32(pstatus, 0);                /* XMEDIAPACKET_STATUS_SUCCESS */
    if (ev)
        xbox_GuestSetEvent(ev);
    RETURN(S_OK_, 3);
}

void xdk_music_Discontinuity(void)
{
    RETURN(S_OK_, 1);
}

void xdk_music_Flush(void)
{
    Music *m = music_of(ARG(0));
    if (m && m->open && m->src.rewind)
        m->src.rewind(m->src.ctx);
    RETURN(S_OK_, 1);
}

void xdk_music_Seek(void)
{
    Music *m = music_of(ARG(0));
    int32_t off = (int32_t)ARG(1);
    uint32_t origin = ARG(2), pabs = ARG(3);
    if (m && m->open && off == 0 && origin == 0 && m->src.rewind)   /* FILE_BEGIN, 0: rewind */
        m->src.rewind(m->src.ctx);
    if (pabs)
        w32(pabs, 0);
    RETURN(S_OK_, 4);
}

void xdk_music_GetLength(void)
{
    Music *m = music_of(ARG(0));
    uint32_t p = ARG(1), len = 0x7FFFFFFFu;
    if (m && !m->has_pl && m->open && m->cur.album >= 0) {
        /* one song: its PCM bytes when the file says (a playlist never ends) */
        uint32_t ms = media_decoder_length_ms((const MediaDecoder *)m->src.ctx);
        if (ms)
            len = (uint32_t)((uint64_t)ms * MEDIA_RATE / 1000u * 4u);
    }
    if (p)
        w32(p, len);
    RETURN(S_OK_, 2);
}

void xdk_music_GetFormat(void)
{
    Music *m = music_of(ARG(0));
    uint32_t pp = ARG(1);
    if (pp)
        w32(pp, m ? m->obj + 0x10 : 0);
    RETURN(m ? S_OK_ : E_FAIL_, 2);
}

void xdk_music_DoWork(void)
{
    RETURN(S_OK_, 2);
}
