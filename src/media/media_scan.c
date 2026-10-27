/* media_scan.c -- custom soundtracks below media\ (see media_scan.h). */

#include "media_scan.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#define MAX_DEPTH 16

static char *dupstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static char *join(const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b);
    char *d = (char *)malloc(na + nb + 2);
    if (!d)
        return NULL;
    memcpy(d, a, na);
    d[na] = '/';
    memcpy(d + na + 1, b, nb + 1);
    return d;
}

static const char *ext_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && dot != name ? dot + 1 : "";
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

int media_is_playable(const char *name)
{
    const char *e = ext_of(name);
    return ieq(e, "mp3") || ieq(e, "flac") || ieq(e, "wav") || ieq(e, "ogg") || ieq(e, "oga");
}

static int is_aac(const char *name)
{
    const char *e = ext_of(name);
    return ieq(e, "m4a") || ieq(e, "aac");
}

static int is_m3u(const char *name)
{
    const char *e = ext_of(name);
    return ieq(e, "m3u") || ieq(e, "m3u8");
}

int media_natural_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            const char *sa, *sb;
            size_t la, lb;
            while (*a == '0' && isdigit((unsigned char)a[1])) a++;
            while (*b == '0' && isdigit((unsigned char)b[1])) b++;
            for (sa = a; isdigit((unsigned char)*a); a++) ;
            for (sb = b; isdigit((unsigned char)*b); b++) ;
            la = (size_t)(a - sa); lb = (size_t)(b - sb);
            if (la != lb)
                return la < lb ? -1 : 1;
            {
                int c = strncmp(sa, sb, la);
                if (c)
                    return c;
            }
        } else {
            int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
            if (ca != cb)
                return ca < cb ? -1 : 1;
            a++; b++;
        }
    }
    return *a ? 1 : *b ? -1 : 0;
}

/* ── Directory listing ─────────────────────────────────────────────── */

typedef struct {
    char *name;
    int   dir;
} Entry;

static int entry_cmp(const void *x, const void *y)
{
    return media_natural_cmp(((const Entry *)x)->name, ((const Entry *)y)->name);
}

#ifdef _WIN32
static wchar_t *widen(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? (wchar_t *)malloc((size_t)n * sizeof *w) : NULL;
    if (w)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char *narrow(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = n > 0 ? (char *)malloc((size_t)n) : NULL;
    if (s)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}
#endif

/* Entries of dir, sorted naturally; NULL with *n = 0 when it cannot be read. */
static Entry *list_dir(const char *dir, int *n)
{
    Entry *e = NULL;
    int cap = 0;
    *n = 0;
#ifdef _WIN32
    {
        char *pat = join(dir, "*");
        wchar_t *w = pat ? widen(pat) : NULL;
        WIN32_FIND_DATAW fd;
        HANDLE h = w ? FindFirstFileW(w, &fd) : INVALID_HANDLE_VALUE;
        free(pat);
        free(w);
        if (h == INVALID_HANDLE_VALUE)
            return NULL;
        do {
            char *name;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
                continue;
            name = narrow(fd.cFileName);
            if (!name)
                continue;
            if (*n == cap) {
                Entry *ne = (Entry *)realloc(e, (size_t)(cap = cap ? cap * 2 : 32) * sizeof *e);
                if (!ne) { free(name); break; }
                e = ne;
            }
            e[*n].name = name;
            e[*n].dir = !!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
            (*n)++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR *d = opendir(dir);
        struct dirent *de;
        if (!d)
            return NULL;
        while ((de = readdir(d)) != NULL) {
            struct stat st;
            char *full;
            if (de->d_name[0] == '.')
                continue;                       /* ".", "..", hidden */
            full = join(dir, de->d_name);
            if (!full || stat(full, &st) != 0) { free(full); continue; }
            free(full);
            if (*n == cap) {
                Entry *ne = (Entry *)realloc(e, (size_t)(cap = cap ? cap * 2 : 32) * sizeof *e);
                if (!ne) break;
                e = ne;
            }
            e[*n].name = dupstr(de->d_name);
            if (!e[*n].name)
                break;
            e[*n].dir = S_ISDIR(st.st_mode);
            (*n)++;
        }
        closedir(d);
    }
#endif
    if (*n > 1)
        qsort(e, (size_t)*n, sizeof *e, entry_cmp);
    return e;
}

static void free_entries(Entry *e, int n)
{
    int i;
    for (i = 0; i < n; i++)
        free(e[i].name);
    free(e);
}

static int is_file(const char *path)
{
#ifdef _WIN32
    wchar_t *w = widen(path);
    DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static char *read_text(const char *path)
{
    FILE *f;
    long n;
    char *buf;
#ifdef _WIN32
    wchar_t *w = widen(path);
    f = w ? _wfopen(w, L"rb") : NULL;
    free(w);
#else
    f = fopen(path, "rb");
#endif
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || n > (4 << 20) || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf)
        buf[n] = 0;
    return buf;
}

/* Latin-1 text (an old .m3u) as UTF-8; valid UTF-8 is kept. */
static char *as_utf8(char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t extra = 0, i;
    char *d, *o;
    while (*p) {
        int k = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0)
            break;
        for (i = 1; i <= (size_t)k; i++)
            if ((p[i] & 0xC0) != 0x80)
                break;
        if (i <= (size_t)k)
            break;
        p += k + 1;
    }
    if (!*p)
        return s;                               /* valid UTF-8 */
    for (p = (const unsigned char *)s; *p; p++)
        extra += *p >= 0x80;
    d = (char *)malloc(strlen(s) + extra + 1);
    if (!d)
        return s;
    for (p = (const unsigned char *)s, o = d; *p; p++) {
        if (*p < 0x80) {
            *o++ = (char)*p;
        } else {
            *o++ = (char)(0xC0 | *p >> 6);
            *o++ = (char)(0x80 | (*p & 0x3F));
        }
    }
    *o = 0;
    free(s);
    return d;
}

/* ── Albums ─────────────────────────────────────────────────────────── */

typedef struct {
    MediaAlbum *a;
    int n, cap;
} AlbumList;

static void free_album(MediaAlbum *a)
{
    int j;
    for (j = 0; j < a->ntracks; j++) {
        free(a->tracks[j].path);
        free(a->tracks[j].name);
    }
    free(a->tracks);
    free(a->name);
    free(a->rel);
    memset(a, 0, sizeof *a);
}

static int add_track(MediaAlbum *al, int *cap, const char *path)
{
    const char *base = path + strlen(path);
    const char *dot;
    MediaTrack *t;
    size_t len;
    int i;
    for (i = 0; i < al->ntracks; i++)
        if (ieq(al->tracks[i].path, path))
            return 1;                           /* listed twice: once */
    if (al->ntracks >= MEDIA_MAX_TRACKS)
        return 0;
    if (al->ntracks == *cap) {
        MediaTrack *nt = (MediaTrack *)realloc(al->tracks, (size_t)(*cap = *cap ? *cap * 2 : 16) * sizeof *nt);
        if (!nt)
            return 0;
        al->tracks = nt;
    }
    while (base > path && base[-1] != '/' && base[-1] != '\\')
        base--;
    dot = strrchr(base, '.');
    len = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    t = &al->tracks[al->ntracks];
    t->path = dupstr(path);
    t->name = (char *)malloc(len + 1);
    if (!t->path || !t->name) {
        free(t->path);
        free(t->name);
        return 0;
    }
    memcpy(t->name, base, len);
    t->name[len] = 0;
    al->ntracks++;
    return 1;
}

static int is_abs(const char *p)
{
    return p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':');
}

/* The .m3u's entries, in order, then the folder's other music files. */
static void fill_album(MediaAlbum *al, const char *dir, Entry *e, int n, const char *m3u)
{
    int cap = 0, i, full = 0;
    if (m3u) {
        char *mp = join(dir, m3u);
        char *text = mp ? read_text(mp) : NULL;
        char *line, *next;
        free(mp);
        if (text) {
            text = as_utf8(text);
            line = text;
            if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
                line += 3;
            for (; line && *line; line = next) {
                char *end, *p, *path;
                next = strchr(line, '\n');
                if (next)
                    *next++ = 0;
                end = line + strlen(line);
                while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
                    *--end = 0;
                while (*line == ' ' || *line == '\t')
                    line++;
                if (!*line || *line == '#')
                    continue;
                for (p = line; *p; p++)
                    if (*p == '\\')
                        *p = '/';
                if (!media_is_playable(line)) {
                    fprintf(stderr, "[MEDIA] %s/%s: entry \"%s\" skipped (%s)\n", al->rel, m3u, line,
                            is_aac(line) ? "AAC is not supported yet" : "not a supported music file");
                    continue;
                }
                path = is_abs(line) ? dupstr(line) : join(dir, line);
                if (!path)
                    continue;
                if (!is_file(path))
                    fprintf(stderr, "[MEDIA] %s/%s: entry \"%s\" not found, skipped\n", al->rel, m3u, line);
                else if (!add_track(al, &cap, path))
                    full = 1;
                free(path);
            }
            free(text);
        }
    }
    for (i = 0; i < n && !full; i++) {
        char *path;
        if (e[i].dir || !media_is_playable(e[i].name))
            continue;
        path = join(dir, e[i].name);
        if (path && !add_track(al, &cap, path))
            full = 1;
        free(path);
    }
    if (full)
        fprintf(stderr, "[MEDIA] %s: more than %d tracks, the rest is skipped\n", al->rel, MEDIA_MAX_TRACKS);
}

static void scan_dir(AlbumList *out, const char *dir, const char *rel, int depth)
{
    int n, i, music = 0, aac = 0;
    const char *m3u = NULL;
    Entry *e = list_dir(dir, &n);
    if (!e)
        return;
    for (i = 0; i < n; i++) {
        if (e[i].dir)
            continue;
        if (media_is_playable(e[i].name))
            music++;
        else if (is_aac(e[i].name))
            aac++;
        else if (is_m3u(e[i].name)) {
            if (!m3u)
                m3u = e[i].name;
            else if (*rel)
                fprintf(stderr, "[MEDIA] %s: several playlists, \"%s\" is used\n", rel, m3u);
        }
    }
    if (!*rel) {
        if (music || aac)
            fprintf(stderr, "[MEDIA] music directly in media\\ is not used (only in subfolders)\n");
    } else {
        if (aac)
            fprintf(stderr, "[MEDIA] %s: %d AAC file(s) skipped (.m4a/.aac: not supported yet)\n", rel, aac);
        if (music) {
            MediaAlbum al;
            memset(&al, 0, sizeof al);
            al.rel = dupstr(rel);
            if (al.rel) {
                fill_album(&al, dir, e, n, m3u);
                if (al.ntracks && out->n == out->cap) {
                    MediaAlbum *na = (MediaAlbum *)realloc(out->a, (size_t)(out->cap ? out->cap * 2 : 16) * sizeof *na);
                    if (na) {
                        out->a = na;
                        out->cap = out->cap ? out->cap * 2 : 16;
                    }
                }
                if (al.ntracks && out->n < out->cap)
                    out->a[out->n++] = al;
                else
                    free_album(&al);
            }
        }
    }
    if (depth < MAX_DEPTH) {
        for (i = 0; i < n; i++) {
            char *sub, *subrel;
            if (!e[i].dir)
                continue;
            sub = join(dir, e[i].name);
            subrel = *rel ? join(rel, e[i].name) : dupstr(e[i].name);
            if (sub && subrel)
                scan_dir(out, sub, subrel, depth + 1);
            free(sub);
            free(subrel);
        }
    }
    free_entries(e, n);
}

static int album_cmp(const void *x, const void *y)
{
    return media_natural_cmp(((const MediaAlbum *)x)->rel, ((const MediaAlbum *)y)->rel);
}

/* The last component of rel (k = 0) or the one before it (k = 1); "" if none. */
static const char *component(const char *rel, int k, size_t *len)
{
    const char *end = rel + strlen(rel), *s;
    for (;;) {
        s = end;
        while (s > rel && s[-1] != '/')
            s--;
        if (!k--)
            break;
        if (s == rel) {
            *len = 0;
            return "";
        }
        end = s - 1;
    }
    *len = (size_t)(end - s);
    return s;
}

static void name_albums(MediaLibrary *lib)
{
    int i, j;
    for (i = 0; i < lib->nalbums; i++) {
        size_t l;
        const char *c = component(lib->albums[i].rel, 0, &l);
        lib->albums[i].name = (char *)malloc(l + 1);
        if (lib->albums[i].name) {
            memcpy(lib->albums[i].name, c, l);
            lib->albums[i].name[l] = 0;
        }
    }
    char *dup = lib->nalbums ? (char *)calloc((size_t)lib->nalbums, 1) : NULL;
    for (i = 0; dup && i < lib->nalbums; i++)
        for (j = 0; j < lib->nalbums; j++)
            if (j != i && lib->albums[i].name && lib->albums[j].name && ieq(lib->albums[i].name, lib->albums[j].name))
                dup[i] = 1;
    for (i = 0; dup && i < lib->nalbums; i++) {
        if (dup[i]) {
            size_t lp, ln;
            const char *p = component(lib->albums[i].rel, 1, &lp), *nm = component(lib->albums[i].rel, 0, &ln);
            char *s = (char *)malloc(lp + ln + 4);
            if (s && lp) {
                memcpy(s, p, lp);
                memcpy(s + lp, " - ", 3);
                memcpy(s + lp + 3, nm, ln);
                s[lp + 3 + ln] = 0;
                free(lib->albums[i].name);
                lib->albums[i].name = s;
            } else {
                free(s);
            }
        }
    }
    free(dup);
    /* still equal (same parent name too): a number in brackets */
    for (i = 0; i < lib->nalbums; i++) {
        int k = 1;
        for (j = 0; j < i; j++)
            if (lib->albums[i].name && lib->albums[j].name && ieq(lib->albums[i].name, lib->albums[j].name))
                k++;
        if (k > 1) {
            size_t l = strlen(lib->albums[i].name);
            char *s = (char *)malloc(l + 16);
            if (s) {
                snprintf(s, l + 16, "%s (%d)", lib->albums[i].name, k);
                free(lib->albums[i].name);
                lib->albums[i].name = s;
            }
        }
    }
}

int media_scan(const char *media_dir, MediaLibrary *lib)
{
    AlbumList out = { NULL, 0, 0 };
    char *dir = dupstr(media_dir);
    size_t n;
    memset(lib, 0, sizeof *lib);
    if (!dir)
        return 0;
    for (n = strlen(dir); n && (dir[n - 1] == '/' || dir[n - 1] == '\\'); )
        dir[--n] = 0;
    scan_dir(&out, dir, "", 0);
    free(dir);
    lib->albums = out.a;
    lib->nalbums = out.n;
    if (lib->nalbums > 1)
        qsort(lib->albums, (size_t)lib->nalbums, sizeof *lib->albums, album_cmp);
    name_albums(lib);
    return lib->nalbums;
}

void media_free(MediaLibrary *lib)
{
    int i;
    for (i = 0; i < lib->nalbums; i++)
        free_album(&lib->albums[i]);
    free(lib->albums);
    memset(lib, 0, sizeof *lib);
}

int media_find_album(const MediaLibrary *lib, const char *rel)
{
    int i;
    for (i = 0; rel && i < lib->nalbums; i++)
        if (ieq(lib->albums[i].rel, rel))
            return i;
    return -1;
}
