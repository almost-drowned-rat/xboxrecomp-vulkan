/* zip_read.c -- see zip_read.h. Written for this toolkit; the ZIP layout from
 * PKWARE's APPNOTE.TXT (local header, central directory, end record). */
#include "zip_read.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

/* MINIZ_NO_* (all but inflate) come from CMakeLists.txt */
#include "third_party/miniz/miniz_tinfl.h"

struct ZipFile {
    SDL_IOStream *io;
    ZipEntry *e;
    int n;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int fail(char *err, size_t n, const char *msg)
{
    if (err && n)
        snprintf(err, n, "%s", msg);
    return 0;
}

static int read_at(SDL_IOStream *io, uint64_t ofs, void *buf, size_t len)
{
    return SDL_SeekIO(io, (Sint64)ofs, SDL_IO_SEEK_SET) == (Sint64)ofs && SDL_ReadIO(io, buf, len) == len;
}

ZipFile *zip_open(const char *path, char *err, size_t n)
{
    ZipFile *z = NULL;
    uint8_t *tail = NULL, *cd = NULL;
    Sint64 size;
    size_t tlen, i;
    long eocd = -1;
    uint32_t cd_size, cd_ofs, count, pos;

    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) {
        fail(err, n, "cannot open the file");
        return NULL;
    }
    size = SDL_GetIOSize(io);
    if (size < 22 || size > 0xFFFFFFFFll) {           /* no end record, or ZIP64-sized */
        fail(err, n, size < 22 ? "not a zip (too short)" : "larger than 4 GB (ZIP64 is not supported)");
        goto bad;
    }
    /* the end record: the last 22 bytes plus a comment of up to 64 KB */
    tlen = (size_t)(size < 22 + 65535 ? size : 22 + 65535);
    tail = malloc(tlen);
    if (!tail || !read_at(io, (uint64_t)size - tlen, tail, tlen)) {
        fail(err, n, "cannot read the end of the file");
        goto bad;
    }
    for (i = tlen - 22 + 1; i-- > 0;)
        if (rd32(tail + i) == 0x06054b50u && i + 22 + rd16(tail + i + 20) <= tlen) {
            eocd = (long)i;
            break;
        }
    if (eocd < 0) {
        fail(err, n, "not a zip (no end of central directory)");
        goto bad;
    }
    if (eocd >= 20 && rd32(tail + eocd - 20) == 0x07064b50u) {
        fail(err, n, "ZIP64 is not supported");
        goto bad;
    }
    if (rd16(tail + eocd + 4) || rd16(tail + eocd + 6) || rd16(tail + eocd + 8) != rd16(tail + eocd + 10)) {
        fail(err, n, "multi-disk zips are not supported");
        goto bad;
    }
    count = rd16(tail + eocd + 10);
    cd_size = rd32(tail + eocd + 12);
    cd_ofs = rd32(tail + eocd + 16);
    if (count == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_ofs == 0xFFFFFFFFu) {
        fail(err, n, "ZIP64 is not supported");
        goto bad;
    }
    if ((uint64_t)cd_ofs + cd_size > (uint64_t)size - (tlen - (size_t)eocd)) {
        fail(err, n, "central directory outside the file (truncated?)");
        goto bad;
    }
    cd = malloc(cd_size ? cd_size : 1);
    z = calloc(1, sizeof *z);
    if (!cd || !z || !(z->e = calloc(count ? count : 1, sizeof *z->e)) || !read_at(io, cd_ofs, cd, cd_size)) {
        fail(err, n, "cannot read the central directory");
        goto bad;
    }
    for (pos = 0; z->n < (int)count; z->n++) {
        ZipEntry *e = &z->e[z->n];
        uint32_t nl, xl, cl, lofs;
        if (pos + 46 > cd_size || rd32(cd + pos) != 0x02014b50u) {
            fail(err, n, "broken central directory");
            goto bad;
        }
        nl = rd16(cd + pos + 28); xl = rd16(cd + pos + 30); cl = rd16(cd + pos + 32);
        if (pos + 46 + nl + xl + cl > cd_size || nl == 0) {
            fail(err, n, "broken central directory entry");
            goto bad;
        }
        e->flags = rd16(cd + pos + 8);
        e->method = rd16(cd + pos + 10);
        e->crc = rd32(cd + pos + 16);
        e->csize = rd32(cd + pos + 20);
        e->usize = rd32(cd + pos + 24);
        lofs = rd32(cd + pos + 42);
        if (e->csize == 0xFFFFFFFFu || e->usize == 0xFFFFFFFFu || lofs == 0xFFFFFFFFu) {
            fail(err, n, "ZIP64 entries are not supported");
            goto bad;
        }
        e->local_ofs = lofs;
        e->name = malloc(nl + 1);
        if (!e->name)
            goto bad;
        memcpy(e->name, cd + pos + 46, nl);
        e->name[nl] = 0;
        e->dir = e->name[nl - 1] == '/' || e->name[nl - 1] == '\\';
        pos += 46 + nl + xl + cl;
    }
    free(tail);
    free(cd);
    z->io = io;
    return z;
bad:
    free(tail);
    free(cd);
    if (z) {
        int k;
        for (k = 0; k < z->n; k++)
            free(z->e[k].name);
        free(z->e);
        free(z);
    }
    SDL_CloseIO(io);
    return NULL;
}

int zip_count(const ZipFile *z)
{
    return z ? z->n : 0;
}

const ZipEntry *zip_entry(const ZipFile *z, int i)
{
    return z && i >= 0 && i < z->n ? &z->e[i] : NULL;
}

int zip_extract(ZipFile *z, int i, void *out, char *err, size_t n)
{
    const ZipEntry *e = zip_entry(z, i);
    uint8_t lh[30], *in = NULL;
    uint64_t data;
    int ok = 0;

    if (!e)
        return fail(err, n, "no such entry");
    if (e->flags & 1)
        return fail(err, n, "encrypted entry");
    if (e->method != 0 && e->method != 8)
        return fail(err, n, "compression method not supported (only stored and deflate)");
    if (e->usize > ZIP_MAX_ENTRY_BYTES || e->csize > ZIP_MAX_ENTRY_BYTES)
        return fail(err, n, "entry too large");
    if (e->method == 0 && e->csize != e->usize)
        return fail(err, n, "stored entry with two sizes");
    if (!read_at(z->io, e->local_ofs, lh, sizeof lh) || rd32(lh) != 0x04034b50u)
        return fail(err, n, "broken local header");
    data = (uint64_t)e->local_ofs + 30 + rd16(lh + 26) + rd16(lh + 28);
    if (data + e->csize > (uint64_t)SDL_GetIOSize(z->io))
        return fail(err, n, "entry data outside the file (truncated?)");
    if (e->method == 0) {
        if (e->usize && !read_at(z->io, data, out, e->usize))
            return fail(err, n, "cannot read the entry");
    } else {
        in = malloc(e->csize ? e->csize : 1);
        if (!in || (e->csize && !read_at(z->io, data, in, e->csize))) {
            free(in);
            return fail(err, n, "cannot read the entry");
        }
        /* raw deflate (no zlib header); must give exactly usize bytes */
        if (tinfl_decompress_mem_to_mem(out, e->usize, in, e->csize, 0) != e->usize) {
            free(in);
            return fail(err, n, "inflate failed (broken data)");
        }
        free(in);
    }
    if (zip_crc32(0, out, e->usize) != e->crc)
        return fail(err, n, "CRC-32 mismatch (broken data)");
    ok = 1;
    return ok;
}

#define ZIP_STREAM_IN (256u << 10)

/* the stream of one entry: sequential reads from its data, inflate into the 32 KB window, every byte written + CRC'd */
static int stream_entry(ZipFile *z, const ZipEntry *e, uint64_t data, SDL_IOStream *out, char *err, size_t n)
{
    uint8_t *ib = malloc(ZIP_STREAM_IN), *dict = NULL;
    tinfl_decompressor *d = NULL;
    uint64_t left = e->csize, total = 0;
    uint32_t crc = 0;
    size_t ilen = 0, ipos = 0, dpos = 0;
    int ok = 0;

    if (!ib || SDL_SeekIO(z->io, (Sint64)data, SDL_IO_SEEK_SET) != (Sint64)data) {
        fail(err, n, ib ? "cannot read the entry" : "out of memory");
        goto done;
    }
    if (e->method == 0) {
        while (left) {
            size_t k = left > ZIP_STREAM_IN ? ZIP_STREAM_IN : (size_t)left;
            if (SDL_ReadIO(z->io, ib, k) != k) {
                fail(err, n, "cannot read the entry");
                goto done;
            }
            if (SDL_WriteIO(out, ib, k) != k) {
                fail(err, n, "cannot write the file");
                goto done;
            }
            crc = zip_crc32(crc, ib, k);
            left -= k;
            total += k;
        }
    } else {
        dict = malloc(TINFL_LZ_DICT_SIZE);
        d = malloc(sizeof *d);
        if (!dict || !d) {
            fail(err, n, "out of memory");
            goto done;
        }
        tinfl_init(d);
        for (;;) {
            size_t in_n, out_n;
            tinfl_status st;
            if (ipos == ilen && left) {
                ilen = left > ZIP_STREAM_IN ? ZIP_STREAM_IN : (size_t)left;
                if (SDL_ReadIO(z->io, ib, ilen) != ilen) {
                    fail(err, n, "cannot read the entry");
                    goto done;
                }
                ipos = 0;
                left -= ilen;
            }
            in_n = ilen - ipos;
            out_n = TINFL_LZ_DICT_SIZE - dpos;
            /* raw deflate (no zlib header) into the wrapping window */
            st = tinfl_decompress(d, ib + ipos, &in_n, dict, dict + dpos, &out_n, left ? TINFL_FLAG_HAS_MORE_INPUT : 0);
            ipos += in_n;
            if (out_n) {
                total += out_n;
                if (total > e->usize) {
                    fail(err, n, "inflate gives more than the entry's size (broken data)");
                    goto done;
                }
                if (SDL_WriteIO(out, dict + dpos, out_n) != out_n) {
                    fail(err, n, "cannot write the file");
                    goto done;
                }
                crc = zip_crc32(crc, dict + dpos, out_n);
                dpos = (dpos + out_n) & (TINFL_LZ_DICT_SIZE - 1);
            }
            if (st == TINFL_STATUS_DONE)
                break;
            if (st < 0 || (st == TINFL_STATUS_NEEDS_MORE_INPUT && ipos == ilen && !left)) {
                fail(err, n, "inflate failed (broken data)");
                goto done;
            }
        }
    }
    if (total != e->usize) {
        fail(err, n, "inflate gives less than the entry's size (broken data)");
        goto done;
    }
    if (crc != e->crc) {
        fail(err, n, "CRC-32 mismatch (broken data)");
        goto done;
    }
    ok = 1;
done:
    free(ib);
    free(dict);
    free(d);
    return ok;
}

int zip_extract_to_file(ZipFile *z, int i, const char *path, char *err, size_t n)
{
    const ZipEntry *e = zip_entry(z, i);
    uint8_t lh[30];
    uint64_t data;
    SDL_IOStream *out;
    int ok;

    if (!e)
        return fail(err, n, "no such entry");
    if (e->flags & 1)
        return fail(err, n, "encrypted entry");
    if (e->method != 0 && e->method != 8)
        return fail(err, n, "compression method not supported (only stored and deflate)");
    if (e->method == 0 && e->csize != e->usize)
        return fail(err, n, "stored entry with two sizes");
    if (!read_at(z->io, e->local_ofs, lh, sizeof lh) || rd32(lh) != 0x04034b50u)
        return fail(err, n, "broken local header");
    data = (uint64_t)e->local_ofs + 30 + rd16(lh + 26) + rd16(lh + 28);
    if (data + e->csize > (uint64_t)SDL_GetIOSize(z->io))
        return fail(err, n, "entry data outside the file (truncated?)");
    if (!(out = SDL_IOFromFile(path, "wb")))
        return fail(err, n, "cannot create the file");
    ok = stream_entry(z, e, data, out, err, n);
    if (!SDL_CloseIO(out) && ok)
        ok = fail(err, n, "cannot write the file");
    if (!ok)
        SDL_RemovePath(path);
    return ok;
}

void zip_close(ZipFile *z)
{
    int k;
    if (!z)
        return;
    for (k = 0; k < z->n; k++)
        free(z->e[k].name);
    free(z->e);
    SDL_CloseIO(z->io);
    free(z);
}

uint32_t zip_crc32(uint32_t crc, const void *data, size_t len)
{
    static uint32_t t[256];
    static int init;
    const uint8_t *p = data;
    if (!init) {                                  /* the table is the same on every thread: a benign race */
        uint32_t k, c;
        int j;
        for (k = 0; k < 256; k++) {
            for (c = k, j = 0; j < 8; j++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[k] = c;
        }
        init = 1;
    }
    crc = ~crc;
    while (len--)
        crc = t[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static int bad_component(const char *s, size_t len)
{
    static const char *const dev[] = { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
                                       "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
                                       "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
    size_t i, base;
    if ((len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.'))
        return 1;
    if (s[len - 1] == '.' || s[len - 1] == ' ')
        return 1;
    for (base = 0; base < len && s[base] != '.'; base++)
        ;
    for (i = 0; i < sizeof dev / sizeof dev[0]; i++)
        if (strlen(dev[i]) == base && SDL_strncasecmp(s, dev[i], base) == 0)
            return 1;
    return 0;
}

int zip_safe_path(const char *name, char *out, size_t n)
{
    const char *p = name, *c;
    size_t k = 0;
    if (!name || !*name || !out || n < 2)
        return 0;
    if (name[0] == '/' || name[0] == '\\')
        return 0;
    for (c = name; *c; c++)
        if (*c == ':' || (unsigned char)*c < 0x20 || *c == 0x7F)
            return 0;
    while (*p) {
        const char *s = p;
        size_t len;
        while (*p && *p != '/' && *p != '\\')
            p++;
        len = (size_t)(p - s);
        if (*p)
            p++;
        if (!len)
            continue;                             /* "a//b" */
        if (bad_component(s, len) || k + len + 2 > n)
            return 0;
        if (k)
            out[k++] = '/';
        memcpy(out + k, s, len);
        k += len;
    }
    out[k] = 0;
    return k > 0;
}
