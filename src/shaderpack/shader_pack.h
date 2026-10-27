/* shader_pack.h -- the user's RetroArch slang shader packs, made ready for the
 * shader module (THPS docs/shader_module.md, phase 3; every port).
 *
 * media/shaders/ is the user's folder and only read: each *.zip there is one
 * pack, extracted once to data/shaders/<zip name without .zip>/ (a cache,
 * rebuilt from media/ when data/ is deleted), each sub-folder is one pack used
 * in place. Extraction: a single root folder common to every entry is
 * stripped (slang-shaders-master/), every name passes the zip-slip guard
 * (zip_safe_path), stored + deflate only, CRC-checked; it goes to
 * data/shaders/<pack>.partial/ first and is renamed when complete, with a
 * marker (.pack_source: the zip's size and modification time) written last -
 * a cut extraction is redone at the next start, an unchanged zip is not
 * touched again. The files it wrote are listed in .pack_files; a changed zip
 * removes exactly those (never a recursive delete).
 *
 * The preset index (data/shaders/index.txt) lists every .slangp with its pass
 * count - "shaders = N", or the count of the preset a "#reference" points to
 * (followed up to 16 deep). Presets below a folder named "bezel" or "border"
 * are left out (they draw outside the picture; maintainer, 2026-10-27).
 * Rebuilt when the packs change (their markers / folder times), else read back.
 *
 * Nothing here writes below media/. Paths are UTF-8, '/' separators inside
 * the index. Any thread; shader_pack_start runs it on a thread of its own. */
#ifndef SHADERPACK_SHADER_PACK_H
#define SHADERPACK_SHADER_PACK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *path;               /* full path of the .slangp, UTF-8 */
    char *rel;                /* "<pack>/<path in the pack>", '/' separators: the stable key */
    int   passes;             /* pass count; -1 = broken (no shaders line, reference not found) */
} ShaderPreset;

typedef struct {
    ShaderPreset *p;
    int n;
    int packs;                /* packs found (zips + folders) */
    int extracted;            /* zips extracted this time */
    int failed;               /* zips that could not be extracted (logged) */
} ShaderIndex;

/* Extract new / changed zips and build or read the index. 1 = done (idx
 * filled, possibly empty), 0 = data/shaders could not be written. Messages go
 * to stderr with the prefix "  [SHADERS] ". */
int shader_pack_sync(const char *media_shaders, const char *data_shaders, ShaderIndex *idx);
void shader_pack_free(ShaderIndex *idx);

/* The same on a thread of its own, started once (port start-up): the state is
 * SHADER_PACK_WORKING until it is done. shader_pack_index() is NULL before. */
enum { SHADER_PACK_IDLE, SHADER_PACK_WORKING, SHADER_PACK_READY, SHADER_PACK_FAILED };
void shader_pack_start(const char *media_shaders, const char *data_shaders);
int shader_pack_state(void);
const ShaderIndex *shader_pack_index(void);

/* Pass count of a preset file, following "#reference" (exposed for tests). */
int shader_pack_passes(const char *slangp);

/* The user's own presets (THPS 2X: saves/shaders/My Presets): every .slangp
 * below dir, keyed "<rel>/<path below dir>", sorted, with pass counts; nothing
 * hidden. Fills a fresh idx (shader_pack_free it). Any thread. */
void shader_pack_list_dir(const char *dir, const char *rel, ShaderIndex *idx);

/* "Save as preset": a RetroArch simple preset in dir (created if needed) -
 * "#reference" to the preset in use (a path relative to dir when both are on
 * the same drive, else absolute; '/' separators) plus name = "value" for the
 * n changed parameters - under the first free name "<base> <k>.slangp"
 * (k = 1, 2, ...). The written path in out. 1 = written. Any thread. */
int shader_pack_save_preset(const char *dir, const char *base, const char *reference, const char *const *names,
                            const float *values, int n, char *out, size_t out_n);

#ifdef __cplusplus
}
#endif

#endif
