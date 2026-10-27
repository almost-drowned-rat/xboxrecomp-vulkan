/* zip_read.h -- a small read-only ZIP reader for the user's shader packs
 * (shader module phase 3; THPS docs/shader_module.md). Stored and deflate
 * entries only (inflate: miniz's tinfl, MIT, third_party/miniz), no ZIP64, no
 * encryption, no multi-disk archives; sizes and CRC-32 from the central
 * directory, every entry checked against its CRC. Paths are UTF-8 (SDL I/O).
 * Any thread; a ZipFile is used by one thread at a time. */
#ifndef SHADERPACK_ZIP_READ_H
#define SHADERPACK_ZIP_READ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An entry larger than this is refused (a pack's largest file is a few MB;
 * a "zip bomb" entry claims gigabytes). */
#define ZIP_MAX_ENTRY_BYTES (64u << 20)

typedef struct {
    char    *name;            /* as stored, UTF-8, '/' or '\\' separators */
    uint32_t csize, usize;    /* compressed / uncompressed bytes */
    uint32_t crc;
    uint32_t local_ofs;       /* offset of its local header */
    uint16_t method;          /* 0 stored, 8 deflate */
    uint16_t flags;
    int      dir;             /* name ends with a separator */
} ZipEntry;

typedef struct ZipFile ZipFile;

/* Opens the archive and reads its central directory; NULL with a message in
 * err on any error (not a zip, ZIP64, multi-disk, truncated). */
ZipFile *zip_open(const char *path, char *err, size_t n);
int zip_count(const ZipFile *z);
const ZipEntry *zip_entry(const ZipFile *z, int i);
/* The entry's bytes into out (entry->usize of them): read, inflated and
 * checked against the size and CRC-32. 1 = done, 0 = error (err). */
int zip_extract(ZipFile *z, int i, void *out, char *err, size_t n);
/* The entry streamed into the file at path (created or replaced), with the
 * same checks but no ZIP_MAX_ENTRY_BYTES cap: inflated in 32 KB steps, so an
 * entry of any size up to 4 GB needs no buffer of its size (the launcher's
 * compiler zip has a 78 MB DLL). Stops as soon as the data runs past usize.
 * 1 = done, 0 = error (err; a partly written file is removed). */
int zip_extract_to_file(ZipFile *z, int i, const char *path, char *err, size_t n);
void zip_close(ZipFile *z);

/* CRC-32 (IEEE, as ZIP uses), continued from crc (0 to start). */
uint32_t zip_crc32(uint32_t crc, const void *data, size_t len);

/* The zip-slip guard: name made into a relative '/' path that cannot leave
 * the folder it is extracted into, in out. 0 = refused: absolute (/, \, a
 * drive letter), a ".." or "." component, an empty name, a ':' anywhere
 * (drive or NTFS stream), control characters, a component ending in a dot or
 * a space, or a Windows device name (CON, NUL, COM1 ...). Empty components
 * ("a//b") are dropped. */
int zip_safe_path(const char *name, char *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif
