/* shaderpack_test -- src/shaderpack: the zip reader (paths, CRC, inflate,
 * broken archives), the zip-slip guard with a crafted malicious zip, pack
 * extraction (root folder stripped, re-extract only when the zip changes, a
 * cut extraction redone, nothing written outside data/shaders) and the preset
 * index (pass counts through #reference, bezel / border hidden, folder packs).
 * argv[1] = a scratch folder (created; the test works below it only). */
#include "shader_pack.h"
#include "zip_read.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

static int s_fail, s_checks;
#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* raw deflate of 40 lines "#version 450\n// line %d of a test shader\n" (zlib -15, level 9) */
static const unsigned char s_defl[] = {
    0x8d,0xd4,0x41,0x0a,0x83,0x40,0x10,0x44,0xd1,0xbd,0xa7,0x68,0xf0,0x00,0xda,0x55,0x9a,0xc4,0xe3,0x08,0x8e,0x44,
    0x08,0x0a,0x8e,0xe4,0xfc,0x5e,0xc0,0xc5,0xdf,0xff,0x55,0x3f,0xaa,0xdb,0x7f,0x39,0xeb,0x76,0xec,0x31,0x8c,0x7d,
    0xd3,0x75,0xf1,0xdb,0xf6,0x12,0x7d,0x1c,0x6b,0xcc,0x71,0x95,0x7a,0x45,0xfd,0xce,0x4b,0x39,0x9b,0xf6,0xa9,0x4b,
    0xd8,0x09,0x76,0x86,0xdd,0x00,0xbb,0x11,0x76,0x2f,0xd8,0xbd,0x61,0xf7,0x81,0xdd,0x44,0xef,0x8c,0x41,0xa8,0x48,
    0x52,0x92,0xa4,0x26,0x49,0x51,0x92,0xaa,0x24,0x65,0x49,0xea,0x92,0x14,0x26,0xa9,0x8c,0xa8,0x8c,0xf0,0x56,0xa8,
    0x8c,0xa8,0x8c,0xa8,0x8c,0xa8,0x8c,0xa8,0x8c,0xa8,0x8c,0xa8,0x8c,0xa8,0x8c,0xa9,0x8c,0xa9,0x8c,0xf1,0x1b,0xa3,
    0x32,0xa6,0x32,0xa6,0x32,0xa6,0x32,0xa6,0x32,0xa6,0x32,0x7e,0x90,0xb9,0x01 };
static char s_plain[2048];
static size_t s_plain_len;

/* ── a small zip writer (stored, or the deflate stream above) ─────── */

typedef struct {
    const char *name;
    const char *data;          /* stored content (NULL + defl = the deflate entry) */
    int defl;
    int bad_crc;
} Ent;

static void put16(FILE *f, unsigned v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void put32(FILE *f, unsigned long v) { put16(f, v & 0xFFFF); put16(f, (v >> 16) & 0xFFFF); }

static int zip_write(const char *path, const Ent *e, int n)
{
    FILE *f = fopen(path, "wb");
    unsigned long ofs[64], cd, cdlen;
    int i;
    if (!f || n > 64)
        return 0;
    for (i = 0; i < n; i++) {
        const void *d = e[i].defl ? (const void *)s_defl : e[i].data;
        unsigned long cl = e[i].defl ? sizeof s_defl : (unsigned long)strlen(e[i].data);
        unsigned long ul = e[i].defl ? (unsigned long)s_plain_len : cl;
        unsigned long crc = e[i].defl ? zip_crc32(0, s_plain, s_plain_len) : zip_crc32(0, d, cl);
        if (e[i].bad_crc)
            crc ^= 1;
        ofs[i] = (unsigned long)ftell(f);
        put32(f, 0x04034b50); put16(f, 20); put16(f, 0); put16(f, e[i].defl ? 8 : 0); put16(f, 0); put16(f, 0);
        put32(f, crc); put32(f, cl); put32(f, ul); put16(f, (unsigned)strlen(e[i].name)); put16(f, 0);
        fwrite(e[i].name, 1, strlen(e[i].name), f);
        fwrite(d, 1, cl, f);
    }
    cd = (unsigned long)ftell(f);
    for (i = 0; i < n; i++) {
        const void *d = e[i].defl ? (const void *)s_defl : e[i].data;
        unsigned long cl = e[i].defl ? sizeof s_defl : (unsigned long)strlen(e[i].data);
        unsigned long ul = e[i].defl ? (unsigned long)s_plain_len : cl;
        unsigned long crc = e[i].defl ? zip_crc32(0, s_plain, s_plain_len) : zip_crc32(0, d, cl);
        if (e[i].bad_crc)
            crc ^= 1;
        put32(f, 0x02014b50); put16(f, 20); put16(f, 20); put16(f, 0); put16(f, e[i].defl ? 8 : 0);
        put16(f, 0); put16(f, 0); put32(f, crc); put32(f, cl); put32(f, ul);
        put16(f, (unsigned)strlen(e[i].name)); put16(f, 0); put16(f, 0); put16(f, 0); put16(f, 0); put32(f, 0);
        put32(f, ofs[i]);
        fwrite(e[i].name, 1, strlen(e[i].name), f);
    }
    cdlen = (unsigned long)ftell(f) - cd;
    put32(f, 0x06054b50); put16(f, 0); put16(f, 0); put16(f, (unsigned)n); put16(f, (unsigned)n);
    put32(f, cdlen); put32(f, cd); put16(f, 0);
    return fclose(f) == 0;
}

static char *P(const char *a, const char *b)
{
    static char buf[8][1024];
    static int k;
    char *s = buf[k++ & 7];
    snprintf(s, 1024, "%s/%s", a, b);
    return s;
}

static int exists(const char *p)
{
    SDL_PathInfo pi;
    return SDL_GetPathInfo(p, &pi);
}

static char *slurp(const char *p, size_t *n)
{
    return SDL_LoadFile(p, n);
}

static const ShaderPreset *find(const ShaderIndex *x, const char *rel)
{
    int i;
    for (i = 0; i < x->n; i++)
        if (!strcmp(x->p[i].rel, rel))
            return &x->p[i];
    return NULL;
}

/* the test's own scratch folder only */
static SDL_EnumerationResult SDLCALL rm_cb(void *ud, const char *dir, const char *f)
{
    char p[1024];
    SDL_PathInfo pi;
    (void)ud;
    snprintf(p, sizeof p, "%s%s", dir, f);
    if (SDL_GetPathInfo(p, &pi) && pi.type == SDL_PATHTYPE_DIRECTORY)
        SDL_EnumerateDirectory(p, rm_cb, NULL);
    SDL_RemovePath(p);
    return SDL_ENUM_CONTINUE;
}

static void rm_tree(const char *p)
{
    SDL_EnumerateDirectory(p, rm_cb, NULL);
    SDL_RemovePath(p);
}

static void test_paths(void)
{
    char o[256];
    CHECK(zip_safe_path("a/b/c.slang", o, sizeof o) && !strcmp(o, "a/b/c.slang"));
    CHECK(zip_safe_path("a\\b//c.slang", o, sizeof o) && !strcmp(o, "a/b/c.slang"));
    CHECK(zip_safe_path("dir/", o, sizeof o) && !strcmp(o, "dir"));
    CHECK(!zip_safe_path("../evil", o, sizeof o));
    CHECK(!zip_safe_path("a/../../evil", o, sizeof o));
    CHECK(!zip_safe_path("a\\..\\..\\evil", o, sizeof o));
    CHECK(!zip_safe_path("/abs", o, sizeof o));
    CHECK(!zip_safe_path("\\abs", o, sizeof o));
    CHECK(!zip_safe_path("C:/x", o, sizeof o));
    CHECK(!zip_safe_path("x:stream", o, sizeof o));
    CHECK(!zip_safe_path("a/./b", o, sizeof o));
    CHECK(!zip_safe_path("con.txt", o, sizeof o));
    CHECK(!zip_safe_path("a/NUL", o, sizeof o));
    CHECK(!zip_safe_path("a/b. ", o, sizeof o));
    CHECK(!zip_safe_path("a/b.", o, sizeof o));
    CHECK(!zip_safe_path("tab\there", o, sizeof o));
    CHECK(!zip_safe_path("", o, sizeof o));
    CHECK(!zip_safe_path("//", o, sizeof o));
    CHECK(zip_safe_path("console.txt", o, sizeof o));        /* only the exact device names */
    CHECK(zip_crc32(0, "123456789", 9) == 0xCBF43926u);
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "scratch";
    char media[512], data[512], *t;
    ShaderIndex x;
    const ShaderPreset *p;
    size_t n;
    int i;

    for (i = 0; i < 40; i++)
        s_plain_len += (size_t)snprintf(s_plain + s_plain_len, sizeof s_plain - s_plain_len,
                                        "#version 450\n// line %d of a test shader\n", i);
    rm_tree(root);
    SDL_CreateDirectory(root);
    snprintf(media, sizeof media, "%s/media/shaders", root);
    snprintf(data, sizeof data, "%s/data/shaders", root);
    SDL_CreateDirectory(P(root, "media"));
    SDL_CreateDirectory(media);

    test_paths();

    /* 1 a normal pack: one root folder, presets, a deflate entry, bezel / border */
    {
        Ent e[] = {
            { "pack-master/", "", 0, 0 },
            { "pack-master/crt/", "", 0, 0 },
            { "pack-master/crt/a.slangp", "shaders = 2\nshader0 = a.slang\nshader1 = a.slang\n", 0, 0 },
            { "pack-master/crt/b.slangp", "#reference \"a.slangp\"\nparam = 1.0\n", 0, 0 },
            { "pack-master/crt/c.slangp", "#reference \"../missing.slangp\"\n", 0, 0 },
            { "pack-master/crt/q.slangp", "#reference \"a.slangp\"\"\n", 0, 0 },
            { "pack-master/crt/r.slangp", "shaders = \"3\"\n", 0, 0 },
            { "pack-master/deep/a.slang", NULL, 1, 0 },
            { "pack-master/bezel/x.slangp", "shaders = 1\n", 0, 0 },
            { "pack-master/misc/border/y.slangp", "shaders = 1\n", 0, 0 },
        };
        CHECK(zip_write(P(media, "pack.zip"), e, (int)(sizeof e / sizeof e[0])));
    }
    /* 2 a folder pack used in place */
    SDL_CreateDirectory(P(media, "Mine"));
    {
        FILE *f = fopen(P(P(media, "Mine"), "m.slangp"), "wb");
        if (f) { fputs("shaders = 4\n", f); fclose(f); }
    }
    /* 3 the malicious zip */
    {
        Ent e[] = {
            { "../evil1.txt", "x", 0, 0 }, { "/abs.txt", "x", 0, 0 }, { "C:/evil_c.txt", "x", 0, 0 },
            { "a/../../evil2.txt", "x", 0, 0 }, { "a\\..\\..\\evil3.txt", "x", 0, 0 }, { "con.txt", "x", 0, 0 },
            { "ok.txt", "fine", 0, 0 }, { "x:y.txt", "x", 0, 0 }, { "sub/../../../evil4.txt", "x", 0, 0 },
            { ".pack_source", "1 1", 0, 0 },
        };
        CHECK(zip_write(P(media, "evil.zip"), e, (int)(sizeof e / sizeof e[0])));
    }
    /* 4 a zip with a CRC error, 5 a truncated zip */
    {
        Ent e[] = { { "r/good.slangp", "shaders = 1\n", 0, 0 }, { "r/bad.slangp", "shaders = 1\n", 0, 1 } };
        CHECK(zip_write(P(media, "broken.zip"), e, 2));
    }
    {
        Ent e[] = { { "t/one.slangp", "shaders = 1\n", 0, 0 } };
        FILE *f;
        long len;
        char *buf;
        CHECK(zip_write(P(media, "trunc.zip"), e, 1));
        f = fopen(P(media, "trunc.zip"), "rb+");
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        buf = malloc((size_t)len);
        fseek(f, 0, SEEK_SET);
        CHECK(fread(buf, 1, (size_t)len, f) == (size_t)len);
        fclose(f);
        f = fopen(P(media, "trunc.zip"), "wb");
        fwrite(buf, 1, (size_t)len - 30, f);             /* the end record cut off */
        fclose(f);
        free(buf);
    }

    /* first sync */
    CHECK(shader_pack_sync(media, data, &x));
    CHECK(x.packs == 5);
    CHECK(x.extracted == 2);                             /* pack, evil */
    CHECK(x.failed == 2);                                /* broken, trunc */
    CHECK(exists(P(data, "pack/crt/a.slangp")));         /* root folder stripped */
    CHECK(!exists(P(data, "pack/pack-master")));
    CHECK(exists(P(data, "pack/.pack_source")));
    t = slurp(P(data, "pack/deep/a.slang"), &n);
    CHECK(t && n == s_plain_len && !memcmp(t, s_plain, n));   /* inflated right */
    SDL_free(t);
    CHECK((p = find(&x, "pack/crt/a.slangp")) && p->passes == 2);
    CHECK((p = find(&x, "pack/crt/b.slangp")) && p->passes == 2);    /* via #reference */
    CHECK((p = find(&x, "pack/crt/q.slangp")) && p->passes == 2);    /* trailing quotes */
    CHECK((p = find(&x, "pack/crt/r.slangp")) && p->passes == 3);    /* quoted count */
    CHECK((p = find(&x, "pack/crt/c.slangp")) && p->passes == -1);   /* broken reference */
    CHECK((p = find(&x, "Mine/m.slangp")) && p->passes == 4);        /* folder pack */
    CHECK(!find(&x, "pack/bezel/x.slangp"));
    CHECK(!find(&x, "pack/misc/border/y.slangp"));
    CHECK(!find(&x, "broken/r/good.slangp") && !exists(P(data, "broken")) && !exists(P(data, "broken.partial")));
    CHECK(!exists(P(data, "trunc")) && !exists(P(data, "trunc.partial")));
    /* the malicious zip: only ok.txt, nothing outside data/shaders/evil */
    CHECK(exists(P(data, "evil/ok.txt")));
    CHECK(!exists(P(data, "evil1.txt")) && !exists(P(data, "evil2.txt")) && !exists(P(data, "evil3.txt")));
    CHECK(!exists(P(root, "data/evil1.txt")) && !exists(P(root, "data/evil2.txt")) && !exists(P(root, "evil4.txt")));
    CHECK(!exists(P(root, "data/evil4.txt")) && !exists(P(data, "evil4.txt")) && !exists("evil1.txt"));
    CHECK(!exists(P(data, "evil/con.txt")) && !exists("C:/evil_c.txt"));
    t = slurp(P(data, "evil/.pack_source"), &n);
    CHECK(t && strcmp(t, "1 1"));                        /* our marker, not the zip's */
    SDL_free(t);
    for (i = 0; i < x.n; i++)                            /* every path below data/ or media/ */
        CHECK(!strncmp(x.p[i].path, data, strlen(data)) || !strncmp(x.p[i].path, media, strlen(media)));
    shader_pack_free(&x);

    /* second sync: nothing extracted, the index read back */
    CHECK(shader_pack_sync(media, data, &x));
    CHECK(x.extracted == 0);
    CHECK((p = find(&x, "pack/crt/b.slangp")) && p->passes == 2);
    CHECK(find(&x, "Mine/m.slangp") != NULL);
    shader_pack_free(&x);

    /* the zip changed: re-extracted, a file it no longer has is gone */
    {
        Ent e[] = {
            { "pack-master/crt/a.slangp", "shaders = 5\n", 0, 0 },
            { "pack-master/new/n.slangp", "shaders = 1\n# a longer file so the size changes\n", 0, 0 },
        };
        CHECK(zip_write(P(media, "pack.zip"), e, 2));
    }
    CHECK(shader_pack_sync(media, data, &x));
    CHECK(x.extracted == 1);
    CHECK((p = find(&x, "pack/crt/a.slangp")) && p->passes == 5);
    CHECK(find(&x, "pack/new/n.slangp") != NULL);
    CHECK(!exists(P(data, "pack/crt/b.slangp")) && !exists(P(data, "pack/deep/a.slang")));
    CHECK(!exists(P(data, "pack/deep")) && !exists(P(data, "pack.partial")));
    shader_pack_free(&x);

    /* a cut extraction (a .partial left over) and a deleted data/: both redone */
    SDL_CreateDirectory(P(data, "evil.partial"));
    {
        FILE *f = fopen(P(P(data, "evil.partial"), ".pack_files"), "wb");
        if (f) { fputs("ok.txt\n", f); fclose(f); }
        f = fopen(P(P(data, "evil.partial"), "ok.txt"), "wb");
        if (f) { fputs("half", f); fclose(f); }
    }
    SDL_RemovePath(P(data, "evil/.pack_source"));       /* as if the marker never got written */
    CHECK(shader_pack_sync(media, data, &x));
    CHECK(x.extracted == 1 && exists(P(data, "evil/ok.txt")) && !exists(P(data, "evil.partial")));
    shader_pack_free(&x);
    rm_tree(P(root, "data"));
    CHECK(shader_pack_sync(media, data, &x));
    CHECK(x.extracted == 2 && find(&x, "pack/crt/a.slangp") != NULL);
    shader_pack_free(&x);

    /* "save as preset" (phase 4): a simple preset with a relative #reference, numbered, listed */
    {
        char saves[512], out[1100], abs_ref[1024];
        const char *names[2] = { "SCAN", "GLOW" };
        const float vals[2] = { 0.5f, 2.0f };
        ShaderIndex u;
        SDL_PathInfo pi;
        snprintf(saves, sizeof saves, "%s/saves/shaders/My Presets", root);
        /* the reference as an absolute path (as the port hands it over) */
        {
            const char *base = SDL_GetCurrentDirectory();
            if (root[0] && root[1] == ':')
                snprintf(abs_ref, sizeof abs_ref, "%s", P(data, "pack/crt/a.slangp"));
            else
                snprintf(abs_ref, sizeof abs_ref, "%s%s", base ? base : "", P(data, "pack/crt/a.slangp"));
        }
        CHECK(shader_pack_save_preset(saves, "MY PRESET", abs_ref, names, vals, 2, out, sizeof out));
        CHECK(strstr(out, "MY PRESET 1.slangp") != NULL);
        t = slurp(out, &n);
        CHECK(t && strstr(t, "#reference \"../../../data/shaders/pack/crt/a.slangp\"") && strstr(t, "SCAN = \"0.5\"")
              && strstr(t, "GLOW = \"2\""));
        if (t && !strstr(t, "#reference \"../../../data"))
            fprintf(stderr, "saved preset:\n%s\n", t);
        SDL_free(t);
        CHECK(shader_pack_passes(out) == 5);               /* through the relative reference */
        CHECK(shader_pack_save_preset(saves, "MY PRESET", abs_ref, names, vals, 0, out, sizeof out));
        CHECK(strstr(out, "MY PRESET 2.slangp") != NULL);  /* the next free number */
        shader_pack_list_dir(saves, "MY PRESETS", &u);
        CHECK(u.n == 2 && !strcmp(u.p[0].rel, "MY PRESETS/MY PRESET 1.slangp") && u.p[0].passes == 5);
        shader_pack_free(&u);
        CHECK(SDL_GetPathInfo(P(saves, "MY PRESET 1.slangp"), &pi));
    }

    /* media/ was never written: only the files the test put there */
    CHECK(!exists(P(media, "pack")) && !exists(P(media, "index.txt")));

    printf("shaderpack_test: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
