/*
 * game_setup.c - a port's first start: the game's files into <layout>/game/.
 * See game_setup.h. The host file system part (UTF-8 paths, listing, free
 * space) is the first section, the flow follows.
 */
#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif

#include "game_setup.h"
#include "xdvdfs.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#else
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

#define SETUP_PATH   1024
#define SETUP_BLOCK  (1u << 20)
#define TMP_NAME     ".extracting"
#define SPACE_MARGIN (64ull << 20)          /* kept free beyond the files themselves */
#define MAX_IMAGES   8

/* ── Host file system (UTF-8 paths) ──────────────────────────────────── */

#if defined(_WIN32)
#define SEP '\\'
static int widen(const char *s, WCHAR *w, int n)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) > 0;
}
#else
#define SEP '/'
#endif

static FILE *fs_open(const char *p, const char *mode)
{
#if defined(_WIN32)
    WCHAR wp[SETUP_PATH], wm[8];
    if (!widen(p, wp, SETUP_PATH) || !widen(mode, wm, 8))
        return NULL;
    return _wfopen(wp, wm);
#else
    return fopen(p, mode);
#endif
}

/* 1 a file, 2 a directory, 0 neither. */
static int fs_kind(const char *p)
{
#if defined(_WIN32)
    WCHAR wp[SETUP_PATH];
    DWORD a;
    if (!widen(p, wp, SETUP_PATH) || (a = GetFileAttributesW(wp)) == INVALID_FILE_ATTRIBUTES)
        return 0;
    return a & FILE_ATTRIBUTE_DIRECTORY ? 2 : 1;
#else
    struct stat st;
    if (stat(p, &st) != 0)
        return 0;
    return S_ISDIR(st.st_mode) ? 2 : S_ISREG(st.st_mode) ? 1 : 0;
#endif
}

static int fs_mkdir(const char *p)
{
#if defined(_WIN32)
    WCHAR wp[SETUP_PATH];
    if (!widen(p, wp, SETUP_PATH))
        return 0;
    return CreateDirectoryW(wp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir(p, 0755) == 0 || errno == EEXIST;
#endif
}

static int fs_rename(const char *from, const char *to)
{
#if defined(_WIN32)
    WCHAR wf[SETUP_PATH], wt[SETUP_PATH];
    return widen(from, wf, SETUP_PATH) && widen(to, wt, SETUP_PATH) && MoveFileExW(wf, wt, 0);
#else
    return rename(from, to) == 0;
#endif
}

/* A file, read-only or not (an image the player copied may be). */
static int fs_remove_file(const char *p)
{
#if defined(_WIN32)
    WCHAR wp[SETUP_PATH];
    if (!widen(p, wp, SETUP_PATH))
        return 0;
    SetFileAttributesW(wp, FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(wp);
#else
    return unlink(p) == 0;
#endif
}

static int fs_remove_dir(const char *p)
{
#if defined(_WIN32)
    WCHAR wp[SETUP_PATH];
    return widen(p, wp, SETUP_PATH) && RemoveDirectoryW(wp);
#else
    return rmdir(p) == 0;
#endif
}

static int fs_file_size(const char *p, uint64_t *size)
{
#if defined(_WIN32)
    WCHAR wp[SETUP_PATH];
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!widen(p, wp, SETUP_PATH) || !GetFileAttributesExW(wp, GetFileExInfoStandard, &d))
        return 0;
    *size = (uint64_t)d.nFileSizeHigh << 32 | d.nFileSizeLow;
    return 1;
#else
    struct stat st;
    if (stat(p, &st) != 0)
        return 0;
    *size = (uint64_t)st.st_size;
    return 1;
#endif
}

/* A written file out of the OS cache onto the disk before it is closed, so
 * the read-back check sees what the disk holds (maintainer, 2026-10-24). */
static int fs_flush(FILE *f)
{
    if (fflush(f) != 0)
        return 0;
#if defined(_WIN32)
    {
        HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
        return h != INVALID_HANDLE_VALUE && FlushFileBuffers(h);
    }
#else
    return fsync(fileno(f)) == 0;
#endif
}

static int fs_free_bytes(const char *dir, uint64_t *free_bytes)
{
#if defined(_WIN32)
    WCHAR wp[SETUP_PATH];
    ULARGE_INTEGER avail;
    if (!widen(dir, wp, SETUP_PATH) || !GetDiskFreeSpaceExW(wp, &avail, NULL, NULL))
        return 0;
    *free_bytes = avail.QuadPart;
    return 1;
#else
    struct statvfs s;
    if (statvfs(dir, &s) != 0)
        return 0;
    *free_bytes = (uint64_t)s.f_bavail * s.f_frsize;
    return 1;
#endif
}

/* The names in a directory (not "." / ".."), as a NULL-terminated array the
 * caller frees with free_names; kinds[i] as fs_kind. NULL when it cannot be
 * read. */
typedef struct { char **name; int *kind; unsigned n, cap; } NameList;

static int names_add(NameList *l, const char *name, int kind)
{
    if (l->n == l->cap) {
        unsigned cap = l->cap ? l->cap * 2 : 32;
        char **nn = (char **)realloc(l->name, cap * sizeof *nn);
        int *nk;
        if (!nn)
            return 0;
        l->name = nn;
        if (!(nk = (int *)realloc(l->kind, cap * sizeof *nk)))
            return 0;
        l->kind = nk;
        l->cap = cap;
    }
    if (!(l->name[l->n] = (char *)malloc(strlen(name) + 1)))
        return 0;
    strcpy(l->name[l->n], name);
    l->kind[l->n++] = kind;
    return 1;
}

static void names_free(NameList *l)
{
    unsigned i;
    for (i = 0; i < l->n; i++)
        free(l->name[i]);
    free(l->name);
    free(l->kind);
    memset(l, 0, sizeof *l);
}

static int fs_list(const char *dir, NameList *l)
{
    memset(l, 0, sizeof *l);
#if defined(_WIN32)
    {
        WCHAR wp[SETUP_PATH];
        char pat[SETUP_PATH], name[SETUP_PATH];
        WIN32_FIND_DATAW fd;
        HANDLE h;
        snprintf(pat, sizeof pat, "%s\\*", dir);
        if (!widen(pat, wp, SETUP_PATH) || (h = FindFirstFileW(wp, &fd)) == INVALID_HANDLE_VALUE)
            return 0;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL)
                || !names_add(l, name, fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? 2 : 1)) {
                FindClose(h);
                names_free(l);
                return 0;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR *d = opendir(dir);
        struct dirent *e;
        char p[SETUP_PATH];
        if (!d)
            return 0;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            if (!names_add(l, e->d_name, fs_kind(p))) {
                closedir(d);
                names_free(l);
                return 0;
            }
        }
        closedir(d);
    }
#endif
    return 1;
}

static int join(char *out, size_t n, const char *a, const char *b)
{
    return snprintf(out, n, "%s%c%s", a, SEP, b) < (int)n;
}

static int fs_remove_tree(const char *p)
{
    NameList l;
    unsigned i;
    int ok = 1;

    if (fs_kind(p) != 2)
        return fs_kind(p) == 0 || fs_remove_file(p);
    if (!fs_list(p, &l))
        return 0;
    for (i = 0; i < l.n; i++) {
        char c[SETUP_PATH];
        if (!join(c, sizeof c, p, l.name[i]) || !fs_remove_tree(c))
            ok = 0;
    }
    names_free(&l);
    return ok && fs_remove_dir(p);
}

/* Every folder above `file` under `root` (root exists). */
static int fs_mkdirs_for(const char *root, const char *rel)
{
    char p[SETUP_PATH];
    size_t n = strlen(root), i;

    if (n + 1 + strlen(rel) + 1 > sizeof p)
        return 0;
    memcpy(p, root, n);
    p[n++] = SEP;
    for (i = 0; rel[i]; i++) {
        if (rel[i] == '/') {
            p[n] = '\0';
            if (!fs_mkdir(p))
                return 0;
            p[n++] = SEP;
        } else {
            p[n++] = rel[i];
        }
    }
    return 1;
}

/* ── CRC-32 (the zip polynomial) and SHA-1 ────────────────────────────── */

static void crc_table(uint32_t t[256])
{
    uint32_t i, k, c;
    for (i = 0; i < 256; i++) {
        for (c = i, k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
}

static uint32_t crc_update(const uint32_t t[256], uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--)
        crc = t[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; unsigned used; } Sha1;

static uint32_t rol(uint32_t x, int n) { return x << n | x >> (32 - n); }

static void sha1_block(Sha1 *s, const uint8_t *b)
{
    uint32_t w[80], a = s->h[0], bb = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f, k, t;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
    for (; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (i = 0; i < 80; i++) {
        if (i < 20)      f = (bb & c) | (~bb & d), k = 0x5A827999u;
        else if (i < 40) f = bb ^ c ^ d,           k = 0x6ED9EBA1u;
        else if (i < 60) f = (bb & c) | (bb & d) | (c & d), k = 0x8F1BBCDCu;
        else             f = bb ^ c ^ d,           k = 0xCA62C1D6u;
        t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(bb, 30); bb = a; a = t;
    }
    s->h[0] += a; s->h[1] += bb; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(Sha1 *s)
{
    static const uint32_t h0[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    memcpy(s->h, h0, sizeof h0);
    s->len = 0;
    s->used = 0;
}

static void sha1_update(Sha1 *s, const uint8_t *p, size_t n)
{
    s->len += n;
    while (n) {
        size_t k = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->buf + s->used, p, k);
        s->used += (unsigned)k; p += k; n -= k;
        if (s->used == 64) {
            sha1_block(s, s->buf);
            s->used = 0;
        }
    }
}

static void sha1_hex(Sha1 *s, char out[41])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, z = 0, l[8];
    int i;
    sha1_update(s, &pad, 1);
    while (s->used != 56)
        sha1_update(s, &z, 1);
    for (i = 0; i < 8; i++)
        l[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, l, 8);
    for (i = 0; i < 5; i++)
        snprintf(out + 8 * i, 9, "%08X", s->h[i]);
}

/* ── The flow ─────────────────────────────────────────────────────────── */

static int setup_fail(xr_game_setup_result *r, const xr_game_setup_opts *o, int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->message, sizeof r->message, fmt, ap);
    va_end(ap);
    r->code = code;
    if (o->log)
        o->log(o->ctx, r->message);
    return code;
}

static void say(const xr_game_setup_opts *o, const char *fmt, ...)
{
    char m[1024];
    va_list ap;
    if (!o->log)
        return;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    o->log(o->ctx, m);
}

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t n = strlen(s), k = strlen(suffix), i;
    if (n < k)
        return 0;
    for (i = 0; i < k; i++) {
        char a = s[n - k + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b)
            return 0;
    }
    return 1;
}

static int is_image_name(const char *n)
{
    return ends_with_ci(n, ".iso") || ends_with_ci(n, ".xiso");
}

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The version rules (game_setup.h): default.xbe's title ID -- its
 * certificate: "XBEH", base address at 0x104, certificate address at 0x118,
 * title ID at certificate + 8 -- against the port's, then its SHA-1 against
 * the supported versions. */
static int check_identity(const xr_game_setup_opts *o, xr_game_setup_result *r, const char *xbe)
{
    const char *game = o->game ? o->game : "game";
    FILE *f = fs_open(xbe, "rb");
    uint8_t buf[65536], head[0x120];
    size_t got, have = 0;
    unsigned i;
    Sha1 s;

    if (!f)
        return setup_fail(r, o, XR_SETUP_WRITE_FAILED, "default.xbe cannot be read: %s.", strerror(errno));
    sha1_init(&s);
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        if (have < sizeof head) {
            size_t k = sizeof head - have < got ? sizeof head - have : got;
            memcpy(head + have, buf, k);
            have += k;
        }
        sha1_update(&s, buf, got);
    }
    sha1_hex(&s, r->xbe_sha1);
    r->title_id = 0;
    if (have == sizeof head && !memcmp(head, "XBEH", 4)) {
        uint32_t base = rd32(head + 0x104), cert = rd32(head + 0x118);
        uint8_t c[12];
        if (cert >= base && fseek(f, (long)(cert - base), SEEK_SET) == 0 && fread(c, 1, sizeof c, f) == sizeof c)
            r->title_id = rd32(c + 8);
    }
    fclose(f);
    say(o, "default.xbe: title ID %08X, SHA-1 %s", r->title_id, r->xbe_sha1);
    if (o->title_id && r->title_id != o->title_id)
        return setup_fail(r, o, XR_SETUP_WRONG_GAME, "This is not the %s image. Please provide the correct game.", game);
    r->version = NULL;
    for (i = 0; i < o->version_count; i++) {
#if defined(_WIN32)
        if (o->versions[i].xbe_sha1 && !_stricmp(o->versions[i].xbe_sha1, r->xbe_sha1))
#else
        if (o->versions[i].xbe_sha1 && !strcasecmp(o->versions[i].xbe_sha1, r->xbe_sha1))
#endif
            r->version = o->versions[i].name;
    }
    if (o->version_count && !r->version) {
        char list[256] = "";
        for (i = 0; i < o->version_count; i++) {
            size_t n = strlen(list);
            snprintf(list + n, sizeof list - n, "%s%s", i ? ", " : "", o->versions[i].name);
        }
        return setup_fail(r, o, XR_SETUP_UNSUPPORTED, "This %s version is not supported. Supported version(s): %s. "
                          "Please provide a supported version.", game, list);
    }
    return XR_SETUP_READY;
}

static void progress(const xr_game_setup_opts *o, double f, const char *fmt, ...)
{
    char m[256];
    va_list ap;
    if (!o->progress)
        return;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    o->progress(o->ctx, f, m);
}

/* Write one file of the image to tmp/<path>; its CRC-32 of what was read. */
static int extract_one(const xr_game_setup_opts *o, xr_game_setup_result *r, xdvdfs_image *img,
                       const xdvdfs_file *f, const char *tmp, const uint32_t *t, uint8_t *buf,
                       uint32_t *crc, uint64_t *done, uint64_t total, unsigned index, unsigned count)
{
    char p[SETUP_PATH];
    size_t i, n = strlen(tmp);
    uint64_t off = 0;
    FILE *out;

    if (n + 1 + strlen(f->path) + 1 > sizeof p || !fs_mkdirs_for(tmp, f->path))
        return setup_fail(r, o, XR_SETUP_WRITE_FAILED, "Cannot create the folders for \"%s\" in %s.", f->path, tmp);
    memcpy(p, tmp, n);
    p[n] = SEP;
    for (i = 0; f->path[i]; i++)
        p[n + 1 + i] = f->path[i] == '/' ? SEP : f->path[i];
    p[n + 1 + i] = '\0';
    if (!(out = fs_open(p, "wb")))
        return setup_fail(r, o, errno == ENOSPC ? XR_SETUP_DISK_FULL : XR_SETUP_WRITE_FAILED,
                          "Cannot create \"%s\": %s.", f->path, strerror(errno));
    *crc = 0;
    while (off < f->size) {
        size_t k = f->size - off < SETUP_BLOCK ? (size_t)(f->size - off) : SETUP_BLOCK;
        if (o->cancelled && o->cancelled(o->ctx)) {
            fclose(out);
            return setup_fail(r, o, XR_SETUP_CANCELLED, "The install was cancelled; the image is kept.");
        }
        if (!xdvdfs_read(img, f, off, buf, k)) {
            fclose(out);
            return setup_fail(r, o, XR_SETUP_BAD_IMAGE, "The image cannot be read at \"%s\" (damaged?).", f->path);
        }
        *crc = crc_update(t, *crc, buf, k);
        if (fwrite(buf, 1, k, out) != k) {
            int e = errno;
            fclose(out);
            return setup_fail(r, o, e == ENOSPC ? XR_SETUP_DISK_FULL : XR_SETUP_WRITE_FAILED,
                              "Writing \"%s\" failed: %s.", f->path, strerror(e));
        }
        off += k;
        *done += k;
        progress(o, total ? 0.9 * (double)*done / (double)(2 * total) : 0.0,
                 "Extracting %u / %u files (%u of %u MB)", index + 1, count,
                 (unsigned)(*done >> 20), (unsigned)(total >> 20));
    }
    if (!fs_flush(out)) {
        int e = errno;
        fclose(out);
        return setup_fail(r, o, e == ENOSPC ? XR_SETUP_DISK_FULL : XR_SETUP_WRITE_FAILED,
                          "Writing \"%s\" to the disk failed: %s.", f->path, strerror(e));
    }
    if (fclose(out) != 0)
        return setup_fail(r, o, errno == ENOSPC ? XR_SETUP_DISK_FULL : XR_SETUP_WRITE_FAILED,
                          "Writing \"%s\" failed: %s.", f->path, strerror(errno));
    return XR_SETUP_READY;
}

/* Read tmp/<path> back: its size and CRC-32 against the entry's. */
static int verify_one(const xr_game_setup_opts *o, xr_game_setup_result *r, const xdvdfs_file *f,
                      const char *tmp, const uint32_t *t, uint8_t *buf, uint32_t want,
                      uint64_t *done, uint64_t total)
{
    char p[SETUP_PATH];
    size_t i, n = strlen(tmp), got;
    uint64_t size = 0, seen = 0;
    uint32_t crc = 0;
    FILE *in;

    memcpy(p, tmp, n);
    p[n] = SEP;
    for (i = 0; f->path[i]; i++)
        p[n + 1 + i] = f->path[i] == '/' ? SEP : f->path[i];
    p[n + 1 + i] = '\0';
    if (!fs_file_size(p, &size) || size != f->size)
        return setup_fail(r, o, XR_SETUP_VERIFY_FAILED, "\"%s\" was written with %llu bytes, the image has "
                          "%u - the image is kept.", f->path, (unsigned long long)size, f->size);
    if (!(in = fs_open(p, "rb")))
        return setup_fail(r, o, XR_SETUP_VERIFY_FAILED, "\"%s\" cannot be read back: %s - the image is kept.",
                          f->path, strerror(errno));
    while ((got = fread(buf, 1, SETUP_BLOCK, in)) > 0) {
        crc = crc_update(t, crc, buf, got);
        seen += got;
        *done += got;
        progress(o, total ? 0.45 + 0.45 * (double)*done / (double)total : 0.45, "Checking the files (%u of %u MB)",
                 (unsigned)(*done >> 20), (unsigned)(total >> 20));
        if (o->cancelled && o->cancelled(o->ctx)) {
            fclose(in);
            return setup_fail(r, o, XR_SETUP_CANCELLED, "The install was cancelled; the image is kept.");
        }
    }
    fclose(in);
    if (seen != f->size || crc != want)
        return setup_fail(r, o, XR_SETUP_VERIFY_FAILED, "\"%s\" differs from the image (CRC-32 %08X written, "
                          "%08X read back) - the image is kept.", f->path, want, crc);
    return XR_SETUP_READY;
}

/* Everything in tmp/ up into game/, default.xbe last. An entry already in game/ with the
 * same name (a run that stopped while moving) is replaced. */
static int move_up(const xr_game_setup_opts *o, xr_game_setup_result *r, const char *game, const char *tmp,
                   const char *image_path)
{
    NameList l;
    unsigned i;
    int pass;

    if (!fs_list(tmp, &l))
        return setup_fail(r, o, XR_SETUP_WRITE_FAILED, "Cannot list %s.", tmp);
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < l.n; i++) {
            char from[SETUP_PATH], to[SETUP_PATH];
            int xbe = !strcmp(l.name[i], "default.xbe");
            if (xbe != pass)
                continue;
            if (!join(from, sizeof from, tmp, l.name[i]) || !join(to, sizeof to, game, l.name[i])
                || !strcmp(to, image_path)) {
                names_free(&l);
                return setup_fail(r, o, XR_SETUP_INTERNAL, "Cannot move \"%s\" into %s.", l.name[i], game);
            }
            if (fs_kind(to) && !fs_remove_tree(to)) {
                names_free(&l);
                return setup_fail(r, o, XR_SETUP_WRITE_FAILED, "\"%s\" is already in %s and cannot be replaced.",
                                  l.name[i], game);
            }
            if (!fs_rename(from, to)) {
                names_free(&l);
                return setup_fail(r, o, XR_SETUP_WRITE_FAILED, "Cannot move \"%s\" into %s.", l.name[i], game);
            }
        }
    }
    names_free(&l);
    fs_remove_dir(tmp);
    return XR_SETUP_READY;
}

static int install_image(const xr_game_setup_opts *o, xr_game_setup_result *r, const char *game,
                         const char *image_path)
{
    char err[512], tmp[SETUP_PATH] = "", xbe[SETUP_PATH];
    xdvdfs_image *img = NULL;
    const xdvdfs_file *files;
    uint32_t t[256], *crc = NULL;
    uint8_t *buf = NULL;
    uint64_t total = 0, done = 0, free_bytes = 0;
    unsigned i, n, count, xi;
    int rc, fd;
    FILE *f;

    say(o, "installing from %s", r->image);
    if (!(f = fs_open(image_path, "rb")))
        return setup_fail(r, o, XR_SETUP_BAD_IMAGE, "The image \"%s\" cannot be opened: %s.", r->image, strerror(errno));
#if defined(_WIN32)
    fd = _fileno(f);
#else
    fd = fileno(f);
#endif
    if ((rc = xdvdfs_open_fd(fd, &img, err, sizeof err)) != XDVDFS_OK) {
        fclose(f);
        return setup_fail(r, o, rc == XDVDFS_E_NOMEM ? XR_SETUP_INTERNAL : XR_SETUP_BAD_IMAGE,
                          "\"%s\": %s.", r->image, err);
    }
    files = xdvdfs_files(img);
    count = xdvdfs_count(img);
    if (!xdvdfs_find(img, "default.xbe")) {
        xdvdfs_close(img);
        fclose(f);
        return setup_fail(r, o, XR_SETUP_NO_XBE, "\"%s\" is an Xbox disc image without default.xbe - not a game "
                          "disc?", r->image);
    }
    for (i = 0; i < count; i++)
        total += files[i].size;
    if (o->test_free_bytes)
        free_bytes = o->test_free_bytes;
    else if (!fs_free_bytes(game, &free_bytes))
        free_bytes = UINT64_MAX;            /* unknown: try, a write error stops it */
    if (free_bytes < total + SPACE_MARGIN) {
        xdvdfs_close(img);
        fclose(f);
        return setup_fail(r, o, XR_SETUP_DISK_FULL, "Not enough free space: the game needs %llu MB, the drive has "
                          "%llu MB free.", (unsigned long long)((total + SPACE_MARGIN) >> 20),
                          (unsigned long long)(free_bytes >> 20));
    }
    if (!join(tmp, sizeof tmp, game, TMP_NAME) || !fs_mkdir(tmp)
        || !(crc = (uint32_t *)malloc((count ? count : 1) * sizeof *crc))
        || !(buf = (uint8_t *)malloc(SETUP_BLOCK))) {
        rc = setup_fail(r, o, crc && buf ? XR_SETUP_WRITE_FAILED : XR_SETUP_INTERNAL,
                        "Cannot prepare the install folder %s.", tmp);
        goto out;
    }
    crc_table(t);
    /* default.xbe first, written, read back and identified before any other
     * file (fail fast: a wrong game or version costs one file). */
    xi = (unsigned)(xdvdfs_find(img, "default.xbe") - files);
    rc = extract_one(o, r, img, &files[xi], tmp, t, buf, &crc[xi], &done, total, 0, count);
    if (rc == XR_SETUP_READY) {
        uint64_t scratch = 0;
        rc = verify_one(o, r, &files[xi], tmp, t, buf, crc[xi], &scratch, total);
    }
    if (rc == XR_SETUP_READY && join(xbe, sizeof xbe, tmp, "default.xbe"))
        rc = check_identity(o, r, xbe);
    for (i = 0, n = 1; i < count && rc == XR_SETUP_READY; i++)
        if (i != xi)
            rc = extract_one(o, r, img, &files[i], tmp, t, buf, &crc[i], &done, total, n++, count);
    done = files[xi].size;
    for (i = 0; i < count && rc == XR_SETUP_READY; i++)
        if (i != xi)
            rc = verify_one(o, r, &files[i], tmp, t, buf, crc[i], &done, total);
    xdvdfs_close(img);
    img = NULL;
    fclose(f);
    f = NULL;
    if (rc == XR_SETUP_READY) {
        progress(o, 0.95, "Moving the files into place");
        rc = move_up(o, r, game, tmp, image_path);
    }
    if (rc != XR_SETUP_READY)
        goto out;
    r->files = count;
    r->bytes = total;
    if (o->image_path)
        r->image_kept = 1;                  /* picked from elsewhere: the player's file, never deleted */
    else if (!fs_remove_file(image_path)) {
        r->image_kept = 1;
        say(o, "the image %s could not be deleted - it can be removed by hand", r->image);
    }
    progress(o, 1.0, "Done");
    r->code = XR_SETUP_INSTALLED;
    snprintf(r->message, sizeof r->message, "%s installed from \"%s\": %u files, %llu MB%s.",
             o->game ? o->game : "The game", r->image, count, (unsigned long long)(total >> 20),
             o->image_path ? " (the image stays where it is)"
             : r->image_kept ? " (the image is still there)" : "; the image was deleted");
    say(o, "%s", r->message);
    rc = XR_SETUP_INSTALLED;
out:
    if (img)
        xdvdfs_close(img);
    if (f)
        fclose(f);
    if (rc != XR_SETUP_INSTALLED && tmp[0])
        fs_remove_tree(tmp);                /* restart = extract again */
    free(crc);
    free(buf);
    return rc;
}

int xr_game_setup(const xr_game_setup_opts *o, xr_game_setup_result *r)
{
    char xbe[SETUP_PATH], tmp[SETUP_PATH], image_path[SETUP_PATH];
    char *images[MAX_IMAGES];
    const char *nested = NULL;
    unsigned i, nimg = 0;
    NameList l;
    int rc;

    memset(r, 0, sizeof *r);
    if (!o->game_dir || !join(xbe, sizeof xbe, o->game_dir, "default.xbe"))
        return setup_fail(r, o, XR_SETUP_INTERNAL, "No game folder given.");
    if (fs_kind(xbe) == 1) {
        /* An extracted folder: the same version rules. */
        if ((rc = check_identity(o, r, xbe)) != XR_SETUP_READY)
            return rc;
        r->code = XR_SETUP_READY;
        snprintf(r->message, sizeof r->message, "game/default.xbe is there (%s).", r->version ? r->version : "any version");
        return XR_SETUP_READY;
    }
    if (o->image_path) {
        /* an image picked from elsewhere (the launcher): game_dir is made if needed, the image is not moved */
        const char *b = strrchr(o->image_path, '/'), *bs = strrchr(o->image_path, '\\');
        if (bs > b)
            b = bs;
        snprintf(r->image, sizeof r->image, "%s", b ? b + 1 : o->image_path);
        if (fs_kind(o->image_path) != 1)
            return setup_fail(r, o, XR_SETUP_NO_GAME, "The disc image %s cannot be read.", o->image_path);
        if (fs_kind(o->game_dir) != 2 && !fs_mkdir(o->game_dir))
            return setup_fail(r, o, XR_SETUP_WRITE_FAILED, "The game folder %s cannot be made.", o->game_dir);
        if (join(tmp, sizeof tmp, o->game_dir, TMP_NAME) && fs_kind(tmp)) {
            say(o, "removing an unfinished install (%s)", tmp);
            fs_remove_tree(tmp);
        }
        return install_image(o, r, o->game_dir, o->image_path);
    }
    if (fs_kind(o->game_dir) != 2)
        return setup_fail(r, o, XR_SETUP_NO_GAME, "The game folder %s is missing. Put the game's disc image (.iso) "
                          "or its extracted files into it.", o->game_dir);
    if (join(tmp, sizeof tmp, o->game_dir, TMP_NAME) && fs_kind(tmp)) {
        say(o, "removing an unfinished install (%s)", tmp);
        fs_remove_tree(tmp);
    }
    if (!fs_list(o->game_dir, &l))
        return setup_fail(r, o, XR_SETUP_NO_GAME, "The game folder %s cannot be read.", o->game_dir);
    for (i = 0; i < l.n; i++) {
        if (l.kind[i] == 1 && is_image_name(l.name[i])) {
            if (nimg < MAX_IMAGES)
                images[nimg] = l.name[i];
            nimg++;
        } else if (l.kind[i] == 2 && !nested && strcmp(l.name[i], TMP_NAME)) {
            char p[SETUP_PATH];
            if (snprintf(p, sizeof p, "%s%c%s%cdefault.xbe", o->game_dir, SEP, l.name[i], SEP) < (int)sizeof p
                && fs_kind(p) == 1)
                nested = l.name[i];
        }
    }
    if (nimg > 1) {
        char list[600] = "";
        for (i = 0; i < nimg && i < MAX_IMAGES; i++) {
            size_t n = strlen(list);
            snprintf(list + n, sizeof list - n, "%s\"%s\"", i ? ", " : "", images[i]);
        }
        rc = setup_fail(r, o, XR_SETUP_SEVERAL_IMAGES, "game/ holds %u disc images (%s). Leave exactly one of them "
                        "there.", nimg, list);
    } else if (nimg == 1) {
        snprintf(r->image, sizeof r->image, "%s", images[0]);
        if (!join(image_path, sizeof image_path, o->game_dir, images[0]))
            rc = setup_fail(r, o, XR_SETUP_INTERNAL, "The path of \"%s\" is too long.", images[0]);
        else
            rc = install_image(o, r, o->game_dir, image_path);
    } else if (nested) {
        rc = setup_fail(r, o, XR_SETUP_NESTED, "default.xbe is in game/%s/, one folder too deep. Move the contents "
                        "of that folder directly into game/.", nested);
    } else {
        rc = setup_fail(r, o, XR_SETUP_NO_GAME, "game/ holds neither default.xbe nor a disc image. Put the game's "
                        "disc image (.iso) or its extracted files into %s.", o->game_dir);
    }
    names_free(&l);
    return rc;
}

const char *xr_game_setup_code_name(int code)
{
    static const char *const names[] = {
        "ready", "installed", "no-game", "nested", "several-images", "bad-image", "no-xbe",
        "disk-full", "write-failed", "verify-failed", "cancelled", "internal", "wrong-game", "unsupported-version"
    };
    return code >= 0 && code < (int)(sizeof names / sizeof names[0]) ? names[code] : "?";
}
