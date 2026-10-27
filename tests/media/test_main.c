/* media unit test: builds a throw-away media\ tree (WAV files written here,
 * an .m3u, nested folders, name collisions, AAC and loose files), then checks
 * the scan rules, the decoder's 44.1 kHz stereo output and every playlist
 * mode. Usage: media_test <empty scratch dir>. */
#include "media_decode.h"
#include "media_playlist.h"
#include "media_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)

static char s_root[1024];

static void path(char *out, const char *rel)
{
    snprintf(out, 1024, "%s/%s", s_root, rel);
}

static void mkd(const char *rel)
{
    char p[1024];
    path(p, rel);
    MKDIR(p);
}

static void put16(FILE *f, unsigned v) { fputc(v & 0xFF, f); fputc(v >> 8 & 0xFF, f); }
static void put32(FILE *f, unsigned v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

/* `frames` frames of a ramp, s16 PCM. */
static void wav(const char *rel, unsigned rate, unsigned ch, unsigned frames)
{
    char p[1024];
    FILE *f;
    unsigned i, c;
    path(p, rel);
    f = fopen(p, "wb");
    if (!f) { printf("cannot write %s\n", p); s_fail++; return; }
    fwrite("RIFF", 1, 4, f); put32(f, 36 + frames * ch * 2);
    fwrite("WAVEfmt ", 1, 8, f); put32(f, 16); put16(f, 1); put16(f, ch); put32(f, rate);
    put32(f, rate * ch * 2); put16(f, ch * 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, frames * ch * 2);
    for (i = 0; i < frames; i++)
        for (c = 0; c < ch; c++)
            put16(f, (unsigned)(int16_t)((int)(i % 1000) * 30 - 15000 + (int)c * 100) & 0xFFFF);
    fclose(f);
}

static void text(const char *rel, const char *t)
{
    char p[1024];
    FILE *f;
    path(p, rel);
    f = fopen(p, "wb");
    if (f) { fputs(t, f); fclose(f); }
}

static const MediaAlbum *album(const MediaLibrary *l, const char *rel)
{
    int i = media_find_album(l, rel);
    return i >= 0 ? &l->albums[i] : NULL;
}

static void scan_cases(MediaLibrary *lib)
{
    const MediaAlbum *a;
    mkd("media");
    wav("media/loose.wav", 44100, 2, 100);                  /* music directly in media\: not used */
    mkd("media/Nu Med");
    wav("media/Nu Med/Track 10.wav", 44100, 2, 100);
    wav("media/Nu Med/Track 2.wav", 44100, 2, 100);
    wav("media/Nu Med/track 1.wav", 44100, 2, 100);
    text("media/Nu Med/cover.jpg", "x");
    mkd("media/Artist A");                                   /* no music itself: no album */
    mkd("media/Artist A/CD1");
    wav("media/Artist A/CD1/a.wav", 22050, 1, 22050);       /* 1 s mono 22.05 kHz */
    mkd("media/Artist B");
    mkd("media/Artist B/CD1");
    wav("media/Artist B/CD1/b.wav", 44100, 2, 100);
    mkd("media/Mix");
    wav("media/Mix/one.wav", 44100, 2, 100);
    wav("media/Mix/two.wav", 44100, 2, 100);
    wav("media/Mix/three.wav", 44100, 2, 100);
    wav("media/Mix/extra 2.wav", 44100, 2, 100);
    wav("media/Mix/extra 10.wav", 44100, 2, 100);
    text("media/Mix/song.m4a", "not aac");
    text("media/Mix/list.m3u", "#EXTM3U\r\n# comment\r\nthree.wav\r\nmissing.wav\r\n  one.wav  \r\nsong.m4a\r\nthree.wav\r\n");
    mkd("media/OnlyAac");
    text("media/OnlyAac/x.m4a", "aac");

    {
        char m[1024];
        path(m, "no such folder");
        CHECK(media_scan(m, lib) == 0 && lib->nalbums == 0);
        media_free(lib);
        path(m, "media");
        CHECK(media_scan(m, lib) == 4);
    }
    CHECK(lib->nalbums == 4);
    if (lib->nalbums == 4) {
        /* natural order by path: Artist A/CD1, Artist B/CD1, Mix, Nu Med */
        CHECK(!strcmp(lib->albums[0].rel, "Artist A/CD1"));
        CHECK(!strcmp(lib->albums[1].rel, "Artist B/CD1"));
        CHECK(!strcmp(lib->albums[2].rel, "Mix"));
        CHECK(!strcmp(lib->albums[3].rel, "Nu Med"));
        CHECK(!strcmp(lib->albums[0].name, "Artist A - CD1"));
        CHECK(!strcmp(lib->albums[1].name, "Artist B - CD1"));
        CHECK(!strcmp(lib->albums[2].name, "Mix"));
    }
    a = album(lib, "nu med");                                /* case-insensitive key */
    CHECK(a && a->ntracks == 3);
    if (a && a->ntracks == 3) {
        CHECK(!strcmp(a->tracks[0].name, "track 1"));
        CHECK(!strcmp(a->tracks[1].name, "Track 2"));
        CHECK(!strcmp(a->tracks[2].name, "Track 10"));
    }
    a = album(lib, "Mix");
    CHECK(a && a->ntracks == 5);
    if (a && a->ntracks == 5) {
        /* .m3u order (missing and AAC skipped, duplicate once), then the rest naturally */
        CHECK(!strcmp(a->tracks[0].name, "three"));
        CHECK(!strcmp(a->tracks[1].name, "one"));
        CHECK(!strcmp(a->tracks[2].name, "extra 2"));
        CHECK(!strcmp(a->tracks[3].name, "extra 10"));
        CHECK(!strcmp(a->tracks[4].name, "two"));
    }
    CHECK(album(lib, "OnlyAac") == NULL);
    CHECK(media_find_album(lib, "Gone") == -1);
}

static void decode_cases(const MediaLibrary *lib)
{
    const MediaAlbum *a = album(lib, "Artist A/CD1");
    MediaDecoder *d;
    int16_t buf[4096 * 2];
    long n, total = 0;
    if (!a)
        return;
    d = media_decoder_open(a->tracks[0].path);
    CHECK(d != NULL);
    if (!d)
        return;
    CHECK(media_decoder_length_ms(d) == 1000);
    while ((n = media_decoder_read(d, buf, 4096)) > 0) {
        if (!total)
            CHECK(buf[0] == buf[1]);                         /* mono doubled */
        total += n;
    }
    CHECK(n == 0);
    CHECK(total >= 44090 && total <= 44100);                 /* 1 s at 44.1 kHz */
    CHECK(media_decoder_rewind(d));
    n = media_decoder_read(d, buf, 10);
    CHECK(n == 10 && buf[0] == -15000);
    media_decoder_close(d);
    CHECK(media_probe_length_ms(a->tracks[0].path) == 1000);
    {
        char p[1024];
        path(p, "media/Nu Med/cover.jpg");
        CHECK(media_decoder_open(p) == NULL);
    }
}

static int count_album(const MediaPlaylist *p, int album)
{
    int i, n = 0;
    for (i = 0; i < p->n; i++)
        n += p->items[i].album == album;
    return n;
}

static void playlist_cases(const MediaLibrary *lib)
{
    MediaPlaylist p;
    int custom = 0, i, k;
    const int ngame = 15, mix = media_find_album(lib, "Mix");
    for (i = 0; i < lib->nalbums; i++)
        custom += lib->albums[i].ntracks;

    CHECK(media_playlist_build(&p, lib, ngame, MEDIA_ALBUM_ORDER, mix, 1) == 5);
    CHECK(p.items[0].album == mix && p.items[0].track == 0 && p.items[4].track == 4);
    CHECK(media_playlist_next(&p)->track == 1);
    for (i = 0; i < 4; i++)
        media_playlist_next(&p);
    CHECK(media_playlist_current(&p)->track == 0);           /* wrapped */
    media_playlist_free(&p);

    CHECK(media_playlist_build(&p, lib, ngame, MEDIA_ALBUM_ORDER, MEDIA_SEL_COMBINED, 1) == ngame + custom);
    CHECK(p.items[0].album == -1 && p.items[ngame].album == 0);
    media_playlist_free(&p);

    CHECK(media_playlist_build(&p, lib, ngame, MEDIA_ALBUM_ORDER, MEDIA_SEL_GAME, 1) == ngame);
    media_playlist_free(&p);

    CHECK(media_playlist_build(&p, lib, ngame, MEDIA_GAME_SHUFFLE, mix, 7) == ngame);
    CHECK(count_album(&p, -1) == ngame);
    media_playlist_free(&p);

    CHECK(media_playlist_build(&p, lib, ngame, MEDIA_ALBUM_SHUFFLE, mix, 7) == 5);
    CHECK(count_album(&p, mix) == 5);
    {
        /* a pass plays every song once; no song twice in a row across the wrap */
        int seen[5] = { 0 }, prev = media_playlist_current(&p)->track;
        seen[prev]++;
        for (k = 0; k < 4; k++)
            seen[media_playlist_next(&p)->track]++;
        for (k = 0; k < 5; k++)
            CHECK(seen[k] == 1);
        for (k = 0; k < 200; k++) {
            int t = media_playlist_next(&p)->track;
            CHECK(t != prev);
            prev = t;
        }
    }
    media_playlist_free(&p);

    CHECK(media_playlist_build(&p, lib, ngame, MEDIA_CUSTOM_SHUFFLE, MEDIA_SEL_GAME, 3) == custom);
    CHECK(count_album(&p, -1) == 0);
    media_playlist_free(&p);

    CHECK(media_playlist_build(&p, lib, ngame, MEDIA_ALL_SHUFFLE, MEDIA_SEL_GAME, 3) == ngame + custom);
    CHECK(count_album(&p, -1) == ngame);
    media_playlist_free(&p);

    CHECK(media_playlist_build(&p, lib, ngame, MEDIA_ALL_ORDER, mix, 3) == ngame + custom);
    CHECK(p.items[0].album == -1 && p.items[0].track == 0 && p.items[ngame - 1].track == ngame - 1);
    CHECK(p.items[ngame].album == 0 && p.items[ngame + custom - 1].album == lib->nalbums - 1);
    media_playlist_free(&p);

    CHECK(media_playlist_build(&p, lib, ngame, MEDIA_CUSTOM_ORDER, MEDIA_SEL_GAME, 3) == custom);
    CHECK(p.items[0].album == 0 && p.items[custom - 1].album == lib->nalbums - 1);
    {
        MediaSong s = { mix, 2 };
        CHECK(media_playlist_seek(&p, s)->album == mix && media_playlist_current(&p)->track == 2);
        CHECK(media_playlist_next(&p)->track == 3);
        s.album = -1;                                        /* not in the list: stays */
        CHECK(media_playlist_seek(&p, s)->track == 3);
    }
    media_playlist_free(&p);

    /* no custom albums: every mode falls back to game music */
    {
        MediaLibrary none = { NULL, 0 };
        for (k = 1; k < MEDIA_MODE_COUNT; k++) {
            CHECK(media_playlist_build(&p, &none, ngame, (MediaPlayMode)k, 0, 5) == ngame);
            CHECK(count_album(&p, -1) == ngame);
            media_playlist_free(&p);
        }
    }
}

int main(int argc, char **argv)
{
    MediaLibrary lib;
    if (argc < 2) {
        printf("usage: media_test <empty scratch dir>\n");
        return 2;
    }
    snprintf(s_root, sizeof s_root, "%s", argv[1]);
    scan_cases(&lib);
    decode_cases(&lib);
    playlist_cases(&lib);
    media_free(&lib);
    CHECK(media_natural_cmp("a2", "a10") < 0 && media_natural_cmp("A10", "a2") > 0 && !media_natural_cmp("x01", "X1"));
    printf(s_fail ? "media: %d check(s) FAILED\n" : "media: all checks passed\n", s_fail);
    return s_fail != 0;
}
