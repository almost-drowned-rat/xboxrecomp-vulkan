/*
 * game_setup test: xr_game_setup on a layout in a temp folder, with small
 * images built here (no game data):
 *   default.xbe (300 KB: an XBE header + certificate with a title ID),
 *   data/a.bin (1.6 MB), data/empty.bin (0 bytes)
 * as an XISO and as a full redump (video partition in front, the game
 * partition at 0x18300000), a plain ISO 9660 image, another title's image
 * and a version the port does not support.
 * Cases: no game, nested, several images, a damaged image, a non-Xbox image,
 * the wrong game and an unsupported version (fail fast: only default.xbe is
 * extracted, the image kept), a corrupted written file, disk full, cancel, a
 * leftover .extracting, the install from an XISO and from a redump (files
 * byte-identical, image deleted, media/ and saves/ untouched), ready on the
 * next start, the version rules on an extracted folder.
 */
#include "game_setup.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#define SEP "\\"
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#define RMDIR(p) rmdir(p)
#define SEP "/"
#endif

static int s_fail;
#define CHECK(c, what) do { if (c) printf("  PASS  %s\n", what); else { printf("  FAIL  %s (%s:%d)\n", what, __FILE__, __LINE__); s_fail++; } } while (0)

#define SECTOR    2048
#define XBE_SIZE  (300u * 1024)
#define A_SIZE    1600000u                  /* more than one 1 MiB block: read back in two */
#define TITLE     0x41560001u
#define OTHER     0x4D530004u
#define REDUMP    0x18300000u

/* The XBE: "XBEH", base 0x10000 at 0x104, the certificate at base + 0x200
 * (0x118), the title ID at certificate + 8; `variant` changes one byte of the
 * body, so the SHA-1 differs (another version of the same title). */
static uint8_t xbe_byte(uint32_t title, int variant, uint32_t i)
{
    static const uint8_t magic[4] = { 'X', 'B', 'E', 'H' };
    if (i < 4) return magic[i];
    if (i >= 0x104 && i < 0x108) return (uint8_t)(0x00010000u >> (8 * (i - 0x104)));
    if (i >= 0x118 && i < 0x11C) return (uint8_t)(0x00010200u >> (8 * (i - 0x118)));
    if (i >= 0x208 && i < 0x20C) return (uint8_t)(title >> (8 * (i - 0x208)));
    if (i == 0x4000) return (uint8_t)(0x11 + variant);
    if (i < 0x300) return 0;
    return (uint8_t)(i * 31 + (i >> 9));
}
static uint8_t a_byte(uint32_t i) { return (uint8_t)(77 + i * 13); }

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static uint32_t entry(uint8_t *t, uint32_t off, uint16_t left, uint16_t right, uint32_t sector, uint32_t size,
                      uint8_t attr, const char *name)
{
    uint32_t n = (uint32_t)strlen(name), end;
    put16(t + off, left);
    put16(t + off + 2, right);
    put32(t + off + 4, sector);
    put32(t + off + 8, size);
    t[off + 12] = attr;
    t[off + 13] = (uint8_t)n;
    memcpy(t + off + 14, name, n);
    end = (off + 14 + n + 3) & ~3u;
    memset(t + off + 14 + n, 0xFF, end - (off + 14 + n));
    return end;
}

typedef struct { uint32_t title; int variant; int truncate; uint32_t base; } ImageSpec;

/* Sectors of the game partition: 32 descriptor, 33 root, 34 data/, 35..
 * default.xbe, then a.bin. `base` bytes of zeros in front (a redump's video
 * partition) are written with a seek. */
static int write_image(const char *path, ImageSpec s)
{
    uint32_t xbe_sec = 35, xbe_secs = (XBE_SIZE + SECTOR - 1) / SECTOR, a_sec = xbe_sec + xbe_secs;
    uint32_t total = (a_sec + (A_SIZE + SECTOR - 1) / SECTOR + 1) * SECTOR, i;
    uint8_t *img = (uint8_t *)calloc(1, total);
    uint8_t *root = img + 33 * SECTOR, *dir = img + 34 * SECTOR;
    uint32_t o;
    FILE *f;

    if (!img)
        return 0;
    memcpy(img + 32 * SECTOR, "MICROSOFT*XBOX*MEDIA", 20);
    put32(img + 32 * SECTOR + 0x14, 33);
    put32(img + 32 * SECTOR + 0x18, SECTOR);
    memcpy(img + 32 * SECTOR + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20);
    memset(root, 0xFF, SECTOR);
    memset(dir, 0xFF, SECTOR);
    o = entry(root, 0, 0, 0, 34, SECTOR, 0x10, "data");
    put16(root + 2, (uint16_t)(o / 4));
    entry(root, o, 0, 0, xbe_sec, XBE_SIZE, 0x20, "default.xbe");
    o = entry(dir, 0, 0, 0, a_sec, A_SIZE, 0x20, "a.bin");
    put16(dir + 2, (uint16_t)(o / 4));
    entry(dir, o, 0, 0, 0, 0, 0x20, "empty.bin");
    for (i = 0; i < XBE_SIZE; i++)
        img[xbe_sec * SECTOR + i] = xbe_byte(s.title, s.variant, i);
    for (i = 0; i < A_SIZE; i++)
        img[a_sec * SECTOR + i] = a_byte(i);
    if (!(f = fopen(path, "wb"))) {
        free(img);
        return 0;
    }
    if (s.base) {
        /* the video partition: zeros up to the game partition */
        static uint8_t zero[1 << 16];
        uint32_t left = s.base;
        while (left) {
            uint32_t k = left < sizeof zero ? left : (uint32_t)sizeof zero;
            fwrite(zero, 1, k, f);
            left -= k;
        }
    }
    fwrite(img, 1, s.truncate ? 40 * SECTOR : total, f);
    fclose(f);
    free(img);
    return 1;
}

/* A plain ISO 9660 image: "CD001" at sector 16, no Xbox descriptor. */
static int write_plain_iso(const char *path)
{
    uint8_t *img = (uint8_t *)calloc(1, 64 * SECTOR);
    FILE *f;
    if (!img)
        return 0;
    img[16 * SECTOR] = 1;
    memcpy(img + 16 * SECTOR + 1, "CD001", 5);
    if (!(f = fopen(path, "wb"))) {
        free(img);
        return 0;
    }
    fwrite(img, 1, 64 * SECTOR, f);
    fclose(f);
    free(img);
    return 1;
}

static int exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

static int same_as_xbe(const char *p, uint32_t title, int variant)
{
    FILE *f = fopen(p, "rb");
    uint32_t i;
    int c, ok = 1;
    if (!f)
        return 0;
    for (i = 0; i < XBE_SIZE; i++)
        if ((c = fgetc(f)) == EOF || (uint8_t)c != xbe_byte(title, variant, i))
            ok = 0;
    if (fgetc(f) != EOF)
        ok = 0;
    fclose(f);
    return ok;
}

static int same_as_a(const char *p)
{
    FILE *f = fopen(p, "rb");
    uint32_t i;
    int c, ok = 1;
    if (!f)
        return 0;
    for (i = 0; i < A_SIZE; i++)
        if ((c = fgetc(f)) == EOF || (uint8_t)c != a_byte(i))
            ok = 0;
    if (fgetc(f) != EOF)
        ok = 0;
    fclose(f);
    return ok;
}

static long file_len(const char *p)
{
    FILE *f = fopen(p, "rb");
    long n;
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n;
}

static void write_text(const char *p, const char *t)
{
    FILE *f = fopen(p, "wb");
    if (f) {
        fputs(t, f);
        fclose(f);
    }
}

static void quiet_log(void *ctx, const char *text) { (void)ctx; printf("        log: %s\n", text); }

static char s_root[512], s_game[600];

/* Fault injection through the callbacks: CORRUPT flips a byte of a written
 * data/a.bin when the check phase of the other files starts; CANCEL answers
 * "cancelled" from the start. s_max_extract: the highest "Extracting n" seen
 * (fail fast: 1 means only default.xbe was written). */
enum { FAULT_NONE, FAULT_CORRUPT, FAULT_CANCEL };
static int s_fault, s_corrupted;
static unsigned s_max_extract;

static void test_progress(void *ctx, double f, const char *text)
{
    unsigned n;
    (void)ctx; (void)f;
    if (sscanf(text, "Extracting %u", &n) == 1 && n > s_max_extract)
        s_max_extract = n;
    /* after all files are written (data/empty.bin reports nothing: max 2),
     * during a.bin's read-back -- a byte behind its first block */
    if (s_fault == FAULT_CORRUPT && !s_corrupted && !strncmp(text, "Checking", 8) && s_max_extract >= 2) {
        char p[700];
        FILE *x;
        snprintf(p, sizeof p, "%s" SEP ".extracting" SEP "data" SEP "a.bin", s_game);
        if ((x = fopen(p, "r+b")) != NULL) {
            int c;
            fseek(x, 1500000, SEEK_SET);
            c = fgetc(x);
            fseek(x, 1500000, SEEK_SET);
            fputc(c ^ 0x5A, x);
            fclose(x);
            s_corrupted = 1;
        }
    }
}

static int test_cancelled(void *ctx) { (void)ctx; return s_fault == FAULT_CANCEL; }

static char s_sha_v0[41];                   /* the supported version's SHA-1 (learned from a run) */

static int run(xr_game_setup_result *r, uint64_t free_bytes)
{
    xr_game_setup_opts o;
    xr_game_version v[2];
    memset(&o, 0, sizeof o);
    v[0].name = "USA";
    v[0].xbe_sha1 = s_sha_v0;
    v[1].name = "PAL";
    v[1].xbe_sha1 = "0000000000000000000000000000000000000000";
    o.game_dir = s_game;
    o.game = "Test Game";
    o.title_id = TITLE;
    o.versions = v;
    o.version_count = s_sha_v0[0] ? 2 : 0;
    o.log = quiet_log;
    o.progress = test_progress;
    o.cancelled = test_cancelled;
    o.test_free_bytes = free_bytes;
    s_max_extract = 0;
    return xr_game_setup(&o, r);
}

static void path_in(char *out, size_t n, const char *base, const char *rel)
{
    snprintf(out, n, "%s" SEP "%s", base, rel);
}

static void clear_game(void)
{
    static const char *const files[] = { "default.xbe", "data" SEP "a.bin", "data" SEP "empty.bin" };
    char p[700];
    unsigned i;
    for (i = 0; i < 3; i++) {
        path_in(p, sizeof p, s_game, files[i]);
        remove(p);
    }
    path_in(p, sizeof p, s_game, "data");
    RMDIR(p);
}

int main(int argc, char **argv)
{
    char p[700], q[700];
    xr_game_setup_result r;
    ImageSpec good = { TITLE, 0, 0, 0 };

    snprintf(s_root, sizeof s_root, "%s", argc > 1 ? argv[1] : "game_setup_tmp");
    MKDIR(s_root);
    path_in(s_game, sizeof s_game, s_root, "game");
    MKDIR(s_game);
    path_in(p, sizeof p, s_root, "media");
    MKDIR(p);
    path_in(p, sizeof p, s_root, "media" SEP "keep.txt");
    write_text(p, "media");
    path_in(p, sizeof p, s_root, "saves");
    MKDIR(p);
    path_in(p, sizeof p, s_root, "saves" SEP "keep.txt");
    write_text(p, "saves");
    path_in(p, sizeof p, s_game, "disc.iso");

    printf("[learn the supported version's SHA-1: install once without a version list]\n");
    CHECK(write_image(p, good), "image written");
    CHECK(run(&r, 0) == XR_SETUP_INSTALLED && r.title_id == TITLE, "installed, title ID read from the XBE");
    memcpy(s_sha_v0, r.xbe_sha1, sizeof s_sha_v0);
    clear_game();

    printf("[empty game/]\n");
    CHECK(run(&r, 0) == XR_SETUP_NO_GAME, "no image, no default.xbe -> no-game");

    printf("[nested]\n");
    path_in(q, sizeof q, s_game, "Disc");
    MKDIR(q);
    path_in(q, sizeof q, s_game, "Disc" SEP "default.xbe");
    write_text(q, "x");
    CHECK(run(&r, 0) == XR_SETUP_NESTED, "default.xbe one folder down -> nested");
    remove(q);
    path_in(q, sizeof q, s_game, "Disc");
    RMDIR(q);

    printf("[several images]\n");
    path_in(q, sizeof q, s_game, "two.ISO");
    CHECK(write_image(p, good) && write_image(q, good), "two images written");
    CHECK(run(&r, 0) == XR_SETUP_SEVERAL_IMAGES, "two images -> several-images");
    CHECK(exists(p) && exists(q), "both images kept");
    remove(q);

    printf("[damaged image]\n");
    {
        ImageSpec cut = good;
        cut.truncate = 1;
        CHECK(write_image(p, cut), "truncated image written");
    }
    CHECK(run(&r, 0) == XR_SETUP_BAD_IMAGE, "truncated image -> bad-image");
    CHECK(exists(p), "the damaged image is kept");

    printf("[plain ISO 9660, not an Xbox disc]\n");
    CHECK(write_plain_iso(p), "plain ISO written");
    CHECK(run(&r, 0) == XR_SETUP_BAD_IMAGE && strstr(r.message, "not an Xbox disc image"), "plain ISO -> bad-image, says why");
    CHECK(exists(p), "the image is kept");

    printf("[another title's image]\n");
    {
        ImageSpec other = good;
        other.title = OTHER;
        CHECK(write_image(p, other), "image of another title written");
    }
    CHECK(run(&r, 0) == XR_SETUP_WRONG_GAME, "title ID 4D530004 -> wrong-game");
    CHECK(!strcmp(r.message, "This is not the Test Game image. Please provide the correct game."), "the exact message");
    CHECK(s_max_extract == 1, "fail fast: only default.xbe extracted");
    CHECK(exists(p), "the image is kept");
    path_in(q, sizeof q, s_game, ".extracting" SEP "default.xbe");
    CHECK(!exists(q), ".extracting cleared");

    printf("[the right game, an unsupported version]\n");
    {
        ImageSpec v1 = good;
        v1.variant = 1;
        CHECK(write_image(p, v1), "image of another version written");
    }
    CHECK(run(&r, 0) == XR_SETUP_UNSUPPORTED, "same title ID, other SHA-1 -> unsupported-version");
    CHECK(!strcmp(r.message, "This Test Game version is not supported. Supported version(s): USA, PAL. Please provide "
                  "a supported version."), "the exact message");
    CHECK(s_max_extract == 1, "fail fast: only default.xbe extracted");
    CHECK(exists(p), "the image is kept");
    path_in(q, sizeof q, s_game, "default.xbe");
    CHECK(!exists(q), "nothing moved into game/");

    printf("[a written file differs from the image]\n");
    CHECK(write_image(p, good), "good image written");
    s_fault = FAULT_CORRUPT;
    CHECK(run(&r, 0) == XR_SETUP_VERIFY_FAILED && s_corrupted, "corrupted data/a.bin -> verify-failed");
    CHECK(strstr(r.message, "a.bin") != NULL, "the message names the file");
    CHECK(exists(p), "the image is kept");
    path_in(q, sizeof q, s_game, "default.xbe");
    CHECK(!exists(q), "nothing moved into game/");
    s_fault = FAULT_NONE;

    printf("[disk full]\n");
    CHECK(run(&r, 1u << 20) == XR_SETUP_DISK_FULL, "1 MB free -> disk-full");
    CHECK(strstr(r.message, "free space") != NULL, "the message says why");
    CHECK(exists(p), "the image is kept");

    printf("[cancelled]\n");
    s_fault = FAULT_CANCEL;
    CHECK(run(&r, 0) == XR_SETUP_CANCELLED, "cancel -> cancelled");
    CHECK(exists(p), "the image is kept");
    s_fault = FAULT_NONE;

    printf("[leftover from a stopped run]\n");
    path_in(q, sizeof q, s_game, ".extracting");
    MKDIR(q);
    path_in(q, sizeof q, s_game, ".extracting" SEP "default.xbe");
    write_text(q, "half");

    printf("[install from an XISO]\n");
    CHECK(run(&r, 0) == XR_SETUP_INSTALLED, "XISO -> installed");
    CHECK(r.files == 3 && r.bytes == XBE_SIZE + A_SIZE, "3 files, all bytes");
    CHECK(r.version && !strcmp(r.version, "USA"), "version USA recognised");
    path_in(q, sizeof q, s_game, "default.xbe");
    CHECK(same_as_xbe(q, TITLE, 0), "default.xbe byte-identical");
    path_in(q, sizeof q, s_game, "data" SEP "a.bin");
    CHECK(same_as_a(q), "data/a.bin byte-identical");
    path_in(q, sizeof q, s_game, "data" SEP "empty.bin");
    CHECK(file_len(q) == 0, "data/empty.bin there, empty");
    CHECK(!exists(p) && !r.image_kept, "the image is deleted");
    path_in(q, sizeof q, s_game, ".extracting" SEP "default.xbe");
    CHECK(!exists(q), "no .extracting left");
    path_in(q, sizeof q, s_root, "media" SEP "keep.txt");
    CHECK(exists(q), "media/ untouched");
    path_in(q, sizeof q, s_root, "saves" SEP "keep.txt");
    CHECK(exists(q), "saves/ untouched");

    printf("[next start]\n");
    CHECK(run(&r, 0) == XR_SETUP_READY, "default.xbe there -> ready");

    printf("[an extracted folder of the wrong version]\n");
    path_in(q, sizeof q, s_game, "default.xbe");
    {
        FILE *f = fopen(q, "r+b");
        if (f) {
            fseek(f, 0x4000, SEEK_SET);
            fputc(0x77, f);
            fclose(f);
        }
    }
    CHECK(run(&r, 0) == XR_SETUP_UNSUPPORTED, "game/default.xbe of another version -> unsupported-version");
    clear_game();

    printf("[install from a full redump (video partition, game partition at 0x18300000)]\n");
    {
        ImageSpec rd = good;
        rd.base = REDUMP;
        CHECK(write_image(p, rd), "redump image written (~387 MB)");
    }
    CHECK(run(&r, 0) == XR_SETUP_INSTALLED && r.version && !strcmp(r.version, "USA"), "redump -> installed, USA");
    path_in(q, sizeof q, s_game, "default.xbe");
    CHECK(same_as_xbe(q, TITLE, 0), "default.xbe byte-identical");
    path_in(q, sizeof q, s_game, "data" SEP "a.bin");
    CHECK(same_as_a(q), "data/a.bin byte-identical");
    CHECK(!exists(p), "the redump image is deleted");

    printf("%s\n", s_fail ? "FAILED" : "all checks passed");
    return s_fail ? 1 : 0;
}
