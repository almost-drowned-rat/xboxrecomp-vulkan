/* shader_pack.c -- see shader_pack.h. Written for this toolkit. */
#include "shader_pack.h"
#include "zip_read.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#define SP_LOG "  [SHADERS] "
#define SP_MAX_PACK_BYTES (1ull << 30)            /* one pack unpacked: the stock pack is 71 MB */
#define SP_MAX_REF_DEPTH 16

/* ── small helpers ────────────────────────────────────────────────── */

static char *cat3(const char *a, const char *b, const char *c)
{
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    char *s = malloc(la + lb + lc + 1);
    if (s) {
        memcpy(s, a, la);
        memcpy(s + la, b, lb);
        memcpy(s + la + lb, c, lc + 1);
    }
    return s;
}

static char *join(const char *dir, const char *name)
{
    size_t n = strlen(dir);
    return cat3(dir, n && (dir[n - 1] == '/' || dir[n - 1] == '\\') ? "" : "/", name);
}

static int is_dir(const char *p)
{
    SDL_PathInfo pi;
    return SDL_GetPathInfo(p, &pi) && pi.type == SDL_PATHTYPE_DIRECTORY;
}

static int exists(const char *p)
{
    SDL_PathInfo pi;
    return SDL_GetPathInfo(p, &pi);
}

/* every folder up to and including path */
static int mkdirs(const char *path)
{
    char *p = SDL_strdup(path), *c;
    int ok = 1;
    if (!p)
        return 0;
    for (c = p + 1; *c && ok; c++)
        if (*c == '/' || *c == '\\') {
            char keep = *c;
            *c = 0;
            if (c[-1] != ':' && !is_dir(p))       /* not "C:" */
                ok = SDL_CreateDirectory(p);
            *c = keep;
        }
    if (ok && !is_dir(p))
        ok = SDL_CreateDirectory(p);
    SDL_free(p);
    return ok;
}

static int write_file(const char *path, const void *data, size_t len)
{
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    int ok;
    if (!io)
        return 0;
    ok = (len == 0 || SDL_WriteIO(io, data, len) == len);
    return SDL_CloseIO(io) && ok;
}

static int ends_with_ci(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && SDL_strcasecmp(s + a - b, suf) == 0;
}

/* A directory's entry names (no "." / ".."), malloc'd; *n of them. */
typedef struct { char **v; int n, cap; } Names;

static SDL_EnumerationResult SDLCALL names_cb(void *ud, const char *dirname, const char *fname)
{
    Names *l = ud;
    (void)dirname;
    if (!strcmp(fname, ".") || !strcmp(fname, ".."))
        return SDL_ENUM_CONTINUE;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 32;
        char **v = realloc(l->v, (size_t)cap * sizeof *v);
        if (!v)
            return SDL_ENUM_FAILURE;
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n] = SDL_strdup(fname);
    if (!l->v[l->n])
        return SDL_ENUM_FAILURE;
    l->n++;
    return SDL_ENUM_CONTINUE;
}

static int name_cmp(const void *a, const void *b)
{
    return SDL_strcasecmp(*(char *const *)a, *(char *const *)b);
}

static Names list_dir(const char *dir)
{
    Names l = { NULL, 0, 0 };
    SDL_EnumerateDirectory(dir, names_cb, &l);
    if (l.n > 1)
        qsort(l.v, (size_t)l.n, sizeof *l.v, name_cmp);
    return l;
}

static void names_free(Names *l)
{
    int i;
    for (i = 0; i < l->n; i++)
        SDL_free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

/* "<size> <mtime>" of a file or folder */
static void stamp_of(const char *p, char *out, size_t n)
{
    SDL_PathInfo pi;
    if (SDL_GetPathInfo(p, &pi))
        snprintf(out, n, "%llu %lld", (unsigned long long)pi.size, (long long)pi.modify_time);
    else
        snprintf(out, n, "?");
}

/* ── removing exactly what an extraction wrote ────────────────────── */

/* The files listed in <dir>/.pack_files, then the folders they were in
 * (deepest first, only if empty), the two marker files and dir itself. */
static void remove_listed(const char *dir)
{
    char *lf = join(dir, ".pack_files"), *ms = join(dir, ".pack_source");
    size_t len = 0;
    char *text = lf ? SDL_LoadFile(lf, &len) : NULL;
    if (text) {
        char *line = text, *nl;
        int pass;
        for (pass = 0; pass < 2; pass++) {        /* 0: files, 1: their folders bottom-up */
            for (line = text; line && *line; line = nl) {
                nl = strchr(line, '\n');
                if (nl)
                    *nl = 0;
                if (*line) {
                    char safe[1024];
                    if (zip_safe_path(line, safe, sizeof safe)) {   /* never leave dir, even from a doctored list */
                        char *p = join(dir, safe), *sl;
                        if (p && pass == 0)
                            SDL_RemovePath(p);
                        while (p && pass == 1 && (sl = strrchr(p, '/')) != NULL && sl > p + strlen(dir)) {
                            *sl = 0;
                            SDL_RemovePath(p);    /* fails while not empty: fine */
                        }
                        free(p);
                    }
                }
                if (nl)
                    *nl++ = '\n';
            }
        }
        SDL_free(text);
    }
    if (lf)
        SDL_RemovePath(lf);
    if (ms)
        SDL_RemovePath(ms);
    SDL_RemovePath(dir);
    free(lf);
    free(ms);
}

/* ── extraction ───────────────────────────────────────────────────── */

/* The first component every safe entry path shares, if every entry is below
 * it: stripped (one root folder of the zip). "" if none. */
static void common_root(ZipFile *z, char *root, size_t n)
{
    int i, have = 0;
    char safe[1024];
    root[0] = 0;
    for (i = 0; i < zip_count(z); i++) {
        const char *sl;
        size_t len;
        if (!zip_safe_path(zip_entry(z, i)->name, safe, sizeof safe))
            continue;
        sl = strchr(safe, '/');
        len = sl ? (size_t)(sl - safe) : strlen(safe);
        if (!sl && !zip_entry(z, i)->dir) {       /* a file at the top: no common root */
            root[0] = 0;
            return;
        }
        if (!have) {
            if (len + 1 > n)
                return;
            memcpy(root, safe, len);
            root[len] = 0;
            have = 1;
        } else if (strlen(root) != len || strncmp(root, safe, len)) {
            root[0] = 0;
            return;
        }
    }
}

/* zip -> dst (a fresh folder). 1 = complete. */
static int extract(const char *zip, const char *dst, const char *pack)
{
    char err[160], root[256], safe[1024];
    ZipFile *z = zip_open(zip, err, sizeof err);
    SDL_IOStream *list = NULL;
    unsigned long long total = 0;
    int i, files = 0, refused = 0, ok = 1;
    size_t rl;
    char *lp;

    if (!z) {
        fprintf(stderr, SP_LOG "%s: %s -- pack skipped\n", zip, err);
        return 0;
    }
    if (!mkdirs(dst) || !(lp = join(dst, ".pack_files"))) {
        zip_close(z);
        return 0;
    }
    list = SDL_IOFromFile(lp, "wb");
    free(lp);
    if (!list) {
        zip_close(z);
        return 0;
    }
    common_root(z, root, sizeof root);
    rl = strlen(root);
    for (i = 0; i < zip_count(z) && ok; i++) {
        const ZipEntry *e = zip_entry(z, i);
        const char *rel;
        char *out, *sl;
        void *buf;
        if (!zip_safe_path(e->name, safe, sizeof safe)) {
            if (refused++ < 5)
                fprintf(stderr, SP_LOG "%s: entry \"%s\" refused (leaves the folder or is not a plain path)\n",
                        pack, e->name);
            continue;
        }
        rel = safe;
        if (rl) {
            if (strlen(safe) == rl)
                continue;                         /* the root folder itself */
            rel = safe + rl + 1;
        }
        if (!strcmp(rel, ".pack_files") || !strcmp(rel, ".pack_source"))
            continue;                             /* our own markers */
        out = join(dst, rel);
        if (!out) {
            ok = 0;
            break;
        }
        if (e->dir) {
            ok = mkdirs(out);
            free(out);
            continue;
        }
        total += e->usize;
        if (total > SP_MAX_PACK_BYTES) {
            fprintf(stderr, SP_LOG "%s: more than %llu MB unpacked -- pack skipped\n", pack, SP_MAX_PACK_BYTES >> 20);
            free(out);
            ok = 0;
            break;
        }
        if ((sl = strrchr(out, '/')) != NULL) {
            *sl = 0;
            ok = mkdirs(out);
            *sl = '/';
        }
        /* listed before it is written: a cut extraction can be removed */
        SDL_WriteIO(list, rel, strlen(rel));
        SDL_WriteIO(list, "\n", 1);
        buf = malloc(e->usize ? e->usize : 1);
        if (!ok || !buf || !zip_extract(z, i, buf, err, sizeof err)) {
            fprintf(stderr, SP_LOG "%s: %s: %s -- pack skipped\n", pack, e->name, buf ? err : "out of memory");
            ok = 0;
        } else if (!write_file(out, buf, e->usize)) {
            fprintf(stderr, SP_LOG "%s: cannot write %s -- pack skipped\n", pack, out);
            ok = 0;
        } else {
            files++;
        }
        free(buf);
        free(out);
    }
    SDL_CloseIO(list);
    zip_close(z);
    if (refused)
        fprintf(stderr, SP_LOG "%s: %d entries refused by the path guard\n", pack, refused);
    if (ok)
        fprintf(stderr, SP_LOG "%s: %d files extracted (%.1f MB)%s%s%s\n", pack, files, total / 1048576.0,
                rl ? ", root folder " : "", rl ? root : "", rl ? " stripped" : "");
    return ok;
}

/* One zip pack: extracted unless its marker matches. 1 = ready, 2 = extracted now, 0 = failed. */
static int sync_zip(const char *zip, const char *data, const char *pack)
{
    char want[64], *have = NULL, *dst = join(data, pack), *part = cat3(data, "/", pack), *mk = NULL;
    size_t len = 0;
    int r = 0;
    char *p2;
    stamp_of(zip, want, sizeof want);
    if (!dst || !part)
        goto out;
    p2 = cat3(part, ".partial", "");
    free(part);
    part = p2;
    mk = join(dst, ".pack_source");
    if (!part || !mk)
        goto out;
    have = SDL_LoadFile(mk, &len);
    if (have && len == strlen(want) && !memcmp(have, want, len)) {
        r = 1;
        goto out;
    }
    if (exists(part))
        remove_listed(part);                      /* a cut extraction of an earlier start */
    if (!extract(zip, part, pack)) {
        remove_listed(part);
        goto out;
    }
    if (exists(dst))
        remove_listed(dst);                       /* the old contents of a changed zip */
    if (exists(dst) || !SDL_RenamePath(part, dst)) {
        fprintf(stderr, SP_LOG "%s: cannot replace %s (%s) -- the new files stay in %s\n", pack, dst,
                SDL_GetError(), part);
        goto out;
    }
    r = write_file(mk, want, strlen(want)) ? 2 : 0;   /* last: the pack is complete */
out:
    SDL_free(have);
    free(dst);
    free(part);
    free(mk);
    return r;
}

/* ── pass counts ──────────────────────────────────────────────────── */

static int passes_depth(const char *path, int depth)
{
    size_t len = 0;
    char *t = SDL_LoadFile(path, &len), *line, *nl, ref[1024];
    int n = -1;
    ref[0] = 0;
    if (!t)
        return -1;
    for (line = t; line && *line && n < 0; line = nl) {
        char *s = line, *e;
        nl = strpbrk(line, "\r\n");
        if (nl)
            *nl++ = 0;
        while (*s == ' ' || *s == '\t')
            s++;
        if (!SDL_strncasecmp(s, "shaders", 7) && (s[7] == ' ' || s[7] == '\t' || s[7] == '=')) {
            s += 7;
            while (*s == ' ' || *s == '\t' || *s == '=' || *s == '"')
                s++;
            if (*s >= '0' && *s <= '9')
                n = (int)strtol(s, NULL, 10);
        } else if (!ref[0] && !SDL_strncasecmp(s, "#reference", 10)) {
            s += 10;
            while (*s == ' ' || *s == '\t' || *s == '"')
                s++;
            for (e = s + strlen(s); e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '"'); e--)
                ;                                 /* trailing quotes too (librashader 0.12.1) */
            if ((size_t)(e - s) < sizeof ref) {
                memcpy(ref, s, (size_t)(e - s));
                ref[e - s] = 0;
            }
        }
    }
    if (n < 0 && ref[0] && depth < SP_MAX_REF_DEPTH) {
        const char *sl = strrchr(path, '/'), *bs = strrchr(path, '\\');
        size_t dl;
        char *full;
        if (bs > sl)
            sl = bs;
        dl = sl ? (size_t)(sl - path) : 0;
        full = malloc(dl + strlen(ref) + 2);
        if (full) {
            if (ref[0] == '/' || ref[0] == '\\' || (ref[0] && ref[1] == ':'))
                strcpy(full, ref);
            else if (dl)
                snprintf(full, dl + strlen(ref) + 2, "%.*s/%s", (int)dl, path, ref);
            else
                strcpy(full, ref);
            n = passes_depth(full, depth + 1);
            free(full);
        }
    }
    SDL_free(t);
    return n;
}

int shader_pack_passes(const char *slangp)
{
    return passes_depth(slangp, 0);
}

/* ── the index ────────────────────────────────────────────────────── */

typedef struct { ShaderIndex *idx; int cap; int hidden; } Walk;

static int add(Walk *w, const char *path, const char *rel, int passes)
{
    ShaderIndex *x = w->idx;
    if (x->n == w->cap) {
        int cap = w->cap ? w->cap * 2 : 256;
        ShaderPreset *p = realloc(x->p, (size_t)cap * sizeof *p);
        if (!p)
            return 0;
        x->p = p;
        w->cap = cap;
    }
    x->p[x->n].path = SDL_strdup(path);
    x->p[x->n].rel = SDL_strdup(rel);
    x->p[x->n].passes = passes;
    if (!x->p[x->n].path || !x->p[x->n].rel)
        return 0;
    x->n++;
    return 1;
}

/* every .slangp below dir; rel = its key so far ("<pack>/..."); folders named
 * bezel / border are skipped (counted as hidden) */
static void walk(Walk *w, const char *dir, const char *rel, int depth)
{
    Names l = list_dir(dir);
    int i;
    for (i = 0; i < l.n; i++) {
        const char *f = l.v[i];
        char *p, *r;
        if (f[0] == '.')
            continue;
        p = join(dir, f);
        r = cat3(rel, "/", f);
        if (p && r) {
            if (is_dir(p)) {
                if (!SDL_strcasecmp(f, "bezel") || !SDL_strcasecmp(f, "border"))
                    w->hidden++;
                else if (depth < 24)
                    walk(w, p, r, depth + 1);
            } else if (ends_with_ci(f, ".slangp")) {
                add(w, p, r, shader_pack_passes(p));
            }
        }
        free(p);
        free(r);
    }
    names_free(&l);
}

static int preset_cmp(const void *a, const void *b)
{
    return SDL_strcasecmp(((const ShaderPreset *)a)->rel, ((const ShaderPreset *)b)->rel);
}

/* a user folder: nothing hidden (walk() skips bezel / border; these are the user's own) */
static void walk_all(Walk *w, const char *dir, const char *rel, int depth)
{
    Names l = list_dir(dir);
    int i;
    for (i = 0; i < l.n; i++) {
        const char *f = l.v[i];
        char *p, *r;
        if (f[0] == '.')
            continue;
        p = join(dir, f);
        r = cat3(rel, "/", f);
        if (p && r) {
            if (is_dir(p)) {
                if (depth < 8)
                    walk_all(w, p, r, depth + 1);
            } else if (ends_with_ci(f, ".slangp")) {
                add(w, p, r, shader_pack_passes(p));
            }
        }
        free(p);
        free(r);
    }
    names_free(&l);
}

/* a full path as '/'-separated components in buf; *ncomp of them in comp */
static int split_path(const char *p, char *buf, size_t n, char **comp, int max)
{
    int k = 0;
    char *c, *save = NULL;
    snprintf(buf, n, "%s", p);
    for (c = buf; *c; c++)
        if (*c == '\\')
            *c = '/';
    for (c = SDL_strtok_r(buf, "/", &save); c && k < max; c = SDL_strtok_r(NULL, "/", &save))
        if (strcmp(c, "."))
            comp[k++] = c;
    return k;
}

/* target relative to dir ("../../x/y.slangp"), or target itself (another drive, or a relative target) */
static void rel_path(const char *dir, const char *target, char *out, size_t n)
{
    char a[1024], b[1024], *ca[64], *cb[64];
    int na = split_path(dir, a, sizeof a, ca, 64), nb = split_path(target, b, sizeof b, cb, 64), i = 0, k;
    size_t len = 0;
    int abs_a = dir[0] && dir[1] == ':', abs_b = target[0] && target[1] == ':';
    if (!abs_a || !abs_b || SDL_toupper((unsigned char)dir[0]) != SDL_toupper((unsigned char)target[0])) {
        snprintf(out, n, "%s", target);
        for (k = 0; out[k]; k++)
            if (out[k] == '\\')
                out[k] = '/';
        return;
    }
    while (i < na && i < nb - 1 && !SDL_strcasecmp(ca[i], cb[i]))
        i++;
    out[0] = 0;
    for (k = i; k < na && len + 4 < n; k++)
        len += (size_t)snprintf(out + len, n - len, "../");
    for (k = i; k < nb && len < n; k++)
        len += (size_t)snprintf(out + len, n - len, "%s%s", cb[k], k + 1 < nb ? "/" : "");
}

int shader_pack_save_preset(const char *dir, const char *base, const char *reference, const char *const *names,
                            const float *values, int n, char *out, size_t out_n)
{
    char ref[1024], file[1100];
    SDL_IOStream *io;
    int k, i, ok = 1;
    if (!mkdirs(dir))
        return 0;
    for (k = 1; k < 1000; k++) {
        snprintf(file, sizeof file, "%s/%s %d.slangp", dir, base, k);
        if (!exists(file))
            break;
    }
    if (k == 1000)
        return 0;
    rel_path(dir, reference, ref, sizeof ref);
    io = SDL_IOFromFile(file, "wb");
    if (!io)
        return 0;
    ok &= SDL_IOprintf(io, "#reference \"%s\"\n", ref) > 0;
    for (i = 0; i < n && ok; i++)
        ok &= SDL_IOprintf(io, "%s = \"%g\"\n", names[i], (double)values[i]) > 0;
    ok &= SDL_CloseIO(io);
    if (!ok) {
        SDL_RemovePath(file);
        return 0;
    }
    snprintf(out, out_n, "%s", file);
    return 1;
}

void shader_pack_list_dir(const char *dir, const char *rel, ShaderIndex *x)
{
    Walk w = { x, 0, 0 };
    memset(x, 0, sizeof *x);
    if (!is_dir(dir))
        return;
    walk_all(&w, dir, rel, 0);
    if (x->n > 1)
        qsort(x->p, (size_t)x->n, sizeof *x->p, preset_cmp);
}

/* the saved index, if its signature is sig */
static int index_read(const char *file, const char *sig, const char *media, const char *data, ShaderIndex *x)
{
    size_t len = 0, sl = strlen(sig);
    char *t = SDL_LoadFile(file, &len), *line, *nl;
    Walk w = { x, 0, 0 };
    if (!t)
        return 0;
    if (len < sl + 16 || strncmp(t, "#shader-index 1\n", 16) || strncmp(t + 16, sig, sl)) {
        SDL_free(t);
        return 0;
    }
    for (line = t + 16 + sl; line && *line; line = nl) {
        char *tab1, *tab2, *full;
        nl = strchr(line, '\n');
        if (nl)
            *nl++ = 0;
        if (!(tab1 = strchr(line, '\t')) || !(tab2 = strchr(tab1 + 1, '\t')))
            continue;
        *tab1 = *tab2 = 0;
        full = join(tab1[1] == 'z' ? data : media, tab2 + 1);
        if (full)
            add(&w, full, tab2 + 1, atoi(line));
        free(full);
    }
    SDL_free(t);
    return 1;
}

static void index_write(const char *file, const char *sig, const ShaderIndex *x, const char *data)
{
    char *tmp = cat3(file, ".tmp", "");
    SDL_IOStream *io = tmp ? SDL_IOFromFile(tmp, "wb") : NULL;
    size_t dl = strlen(data);
    int i, ok = 1;
    if (!io) {
        free(tmp);
        return;
    }
    ok &= SDL_WriteIO(io, "#shader-index 1\n", 16) == 16;
    ok &= SDL_WriteIO(io, sig, strlen(sig)) == strlen(sig);
    for (i = 0; i < x->n && ok; i++) {
        char line[1400];
        int z = !strncmp(x->p[i].path, data, dl);  /* below data/: an extracted zip */
        int k = snprintf(line, sizeof line, "%d\t%c\t%s\n", x->p[i].passes, z ? 'z' : 'f', x->p[i].rel);
        ok &= k > 0 && (size_t)k < sizeof line && SDL_WriteIO(io, line, (size_t)k) == (size_t)k;
    }
    ok &= SDL_CloseIO(io);
    if (!ok || !SDL_RenamePath(tmp, file))
        SDL_RemovePath(tmp);
    free(tmp);
}

int shader_pack_sync(const char *media, const char *data, ShaderIndex *x)
{
    uint64_t t0 = SDL_GetTicksNS();
    Names l;
    char *sig = SDL_strdup(""), *ifile;
    int i;
    Walk w;

    memset(x, 0, sizeof *x);
    if (!sig)
        return 0;
    if (!is_dir(media)) {
        fprintf(stderr, SP_LOG "no %s -- no shader packs\n", media);
        SDL_free(sig);
        return 1;
    }
    if (!mkdirs(data)) {
        fprintf(stderr, SP_LOG "cannot create %s -- no shader packs\n", data);
        SDL_free(sig);
        return 0;
    }
    l = list_dir(media);
    for (i = 0; i < l.n; i++) {
        char *p = join(media, l.v[i]), st[64], *line = NULL, *ns;
        if (!p || l.v[i][0] == '.') {
            free(p);
            continue;
        }
        if (is_dir(p)) {
            x->packs++;
            stamp_of(p, st, sizeof st);
            line = cat3("#f ", l.v[i], " ");
        } else if (ends_with_ci(l.v[i], ".zip")) {
            char *pack = SDL_strdup(l.v[i]);
            int r;
            if (pack) {
                pack[strlen(pack) - 4] = 0;
                x->packs++;
                r = sync_zip(p, data, pack);
                x->extracted += r == 2;
                x->failed += r == 0;
                stamp_of(p, st, sizeof st);
                line = r ? cat3("#z ", pack, " ") : NULL;
                SDL_free(pack);
            }
        }
        if (line) {
            ns = cat3(sig, line, st);
            SDL_free(sig);
            free(line);
            sig = ns ? cat3(ns, "\n", "") : NULL;
            free(ns);
            if (!sig) {
                free(p);
                names_free(&l);
                return 0;
            }
        }
        free(p);
    }
    ifile = join(data, "index.txt");
    if (ifile && index_read(ifile, sig, media, data, x)) {
        fprintf(stderr, SP_LOG "%d packs, %d presets (index unchanged) in %.0f ms\n", x->packs, x->n,
                (SDL_GetTicksNS() - t0) / 1e6);
    } else {
        w.idx = x;
        w.cap = 0;
        w.hidden = 0;
        for (i = 0; i < l.n; i++) {
            const char *nm = l.v[i];
            char *root = NULL, *pack = SDL_strdup(nm);
            if (!pack || nm[0] == '.') {
                SDL_free(pack);
                continue;
            }
            if (ends_with_ci(nm, ".zip")) {
                pack[strlen(pack) - 4] = 0;
                root = join(data, pack);
                if (root && !exists(root)) {
                    free(root);
                    root = NULL;
                }
            } else {
                char *mp = join(media, nm);
                if (mp && is_dir(mp))
                    root = mp;
                else
                    free(mp);
            }
            if (root)
                walk(&w, root, pack, 0);
            free(root);
            SDL_free(pack);
        }
        if (x->n > 1)
            qsort(x->p, (size_t)x->n, sizeof *x->p, preset_cmp);
        if (ifile)
            index_write(ifile, sig, x, data);
        fprintf(stderr, SP_LOG "%d packs (%d extracted now, %d failed), %d presets, %d bezel/border folders"
                " hidden, index built in %.0f ms\n", x->packs, x->extracted, x->failed, x->n, w.hidden,
                (SDL_GetTicksNS() - t0) / 1e6);
    }
    free(ifile);
    SDL_free(sig);
    names_free(&l);
    return 1;
}

void shader_pack_free(ShaderIndex *x)
{
    int i;
    for (i = 0; i < x->n; i++) {
        SDL_free(x->p[i].path);
        SDL_free(x->p[i].rel);
    }
    free(x->p);
    memset(x, 0, sizeof *x);
}

/* ── on a thread ──────────────────────────────────────────────────── */

static SDL_AtomicInt s_state;
static ShaderIndex s_idx;
static char *s_media, *s_data;

static int SDLCALL pack_thread(void *arg)
{
    (void)arg;
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);   /* behind the game and render threads */
    SDL_SetAtomicInt(&s_state, shader_pack_sync(s_media, s_data, &s_idx) ? SHADER_PACK_READY : SHADER_PACK_FAILED);
    return 0;
}

void shader_pack_start(const char *media, const char *data)
{
    SDL_Thread *t;
    if (!SDL_CompareAndSwapAtomicInt(&s_state, SHADER_PACK_IDLE, SHADER_PACK_WORKING))
        return;
    s_media = SDL_strdup(media);
    s_data = SDL_strdup(data);
    t = s_media && s_data ? SDL_CreateThread(pack_thread, "shader packs", NULL) : NULL;
    if (t)
        SDL_DetachThread(t);
    else
        SDL_SetAtomicInt(&s_state, SHADER_PACK_FAILED);
}

int shader_pack_state(void)
{
    return SDL_GetAtomicInt(&s_state);
}

const ShaderIndex *shader_pack_index(void)
{
    return SDL_GetAtomicInt(&s_state) == SHADER_PACK_READY ? &s_idx : NULL;
}
