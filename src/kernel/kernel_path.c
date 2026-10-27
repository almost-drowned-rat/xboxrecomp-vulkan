/*
 * kernel_path.c - Xbox device-path translation
 *
 * Translates Xbox device-style paths to host filesystem paths. Three host
 * folders (docs/storage.md, "Storage layout for ports"):
 *
 *   \Device\CdRom0\, D:\             -> <game>\
 *   E:\UDATA, U:\                     -> <save>\UDATA\, <save>\UDATA\<title id>\
 *   E:\TDATA, T:\                     -> <tdata>\TDATA\, <tdata>\TDATA\<title id>\
 *                                        (<tdata> = save or data, per title)
 *   cache partitions (X:/Y:/Z:)       -> <data>\cache\<title id>\
 *   raw partitions, system data       -> <data>\PartitionN.img, <data>\SystemData\
 *   memory units F:..M:, \Device\MU_  -> not inserted: translation fails
 *   anything else                     -> translation fails (never the host as is)
 *
 * save holds what a player would miss and is never deleted by the runtime;
 * data holds what may be deleted at any time. The layout version in
 * <save>\storage_layout.txt lets a later layout move folders once, by moving,
 * never deleting.
 *
 * The Win32 build emits UTF-16 paths (for CreateFileW), long ones with the
 * \\?\ prefix; the POSIX build emits UTF-8 paths with '/' separators and
 * resolves letter case against the disk (FATX is case-insensitive).
 */

#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* A project may set this to watch every guest path the kernel translates (for
 * example to print who opened a file). NULL by default. */
void (*g_xbox_path_hook)(const char *xbox_path) = NULL;

uint32_t g_xbox_title_id;

/*
 * Helper: check if an ANSI string starts with a prefix (case-insensitive).
 * Returns the number of chars consumed from the prefix, or 0 if no match.
 * Platform-independent.
 */
static int match_prefix(const char* path, const char* prefix)
{
    int i = 0;
    while (prefix[i]) {
        if (tolower((unsigned char)path[i]) != tolower((unsigned char)prefix[i]))
            return 0;
        i++;
    }
    return i;
}

/* Where a rule puts its paths. */
enum {
    PATH_GAME,      /* the extracted disc                               */
    PATH_SAVE,      /* save_dir                                         */
    PATH_DATA,      /* data_dir                                         */
    PATH_TDATA,     /* save_dir or data_dir, per XboxPathConfig.tdata    */
    PATH_NONE       /* not reachable: memory units                      */
};

/* The sub-folder a rule adds. TITLE_* ones carry the title ID. */
enum {
    SUB_NONE, SUB_UDATA, SUB_TDATA, SUB_UDATA_TITLE, SUB_TDATA_TITLE,
    SUB_CACHE_TITLE, SUB_SYSTEMDATA
};

typedef struct {
    const char* prefix;     /* Xbox path prefix (backslash form)   */
    int         base;       /* PATH_*                               */
    int         sub;        /* SUB_*                                */
} path_rule;

static const path_rule s_rules[] = {
    { "\\Device\\CdRom0\\",                     PATH_GAME,  SUB_NONE        },
    /* E:\UDATA and E:\TDATA: the saved games and the title's persistent
     * data, which XAPI's save-game and title-data calls open by device path
     * (\Device\Harddisk0\Partition1\UDATA\<title id>\...). Under the
     * Partition1 rule below they landed in the game dir, mixed into the
     * extracted disc. No trailing separator: titles open the bare directory
     * too. */
    { "\\Device\\Harddisk0\\Partition1\\UDATA", PATH_SAVE,  SUB_UDATA       },
    { "\\Device\\Harddisk0\\Partition1\\TDATA", PATH_TDATA, SUB_TDATA       },
    { "\\Device\\Harddisk0\\Partition1\\",      PATH_GAME,  SUB_NONE        },
    /* Partition2 is C:, the system partition. On a console that is where the
     * dashboard and its own assets live, and the dashboard opens them by
     * device path as well as through Y:, so a path *under* partition2 is an
     * ordinary asset read and belongs in the game dir -- the same place Y:
     * already goes. Only the bare device keeps the system-data image, which
     * is why this rule carries the trailing separator and is listed first. */
    { "\\Device\\Harddisk0\\Partition2\\",      PATH_GAME,  SUB_NONE        },
    { "C:\\",                                   PATH_GAME,  SUB_NONE        },
    { "\\Device\\Harddisk0\\Partition2",        PATH_DATA,  SUB_SYSTEMDATA  },
    /* 3-5 are the cache partitions behind X:, Y: and Z: (XMountUtilityDrive
     * links Z: to one of them). A title only ever uses its own, so one folder
     * per title ID, in data: the title rebuilds it when it is gone. */
    { "\\Device\\Harddisk0\\Partition3",        PATH_DATA,  SUB_CACHE_TITLE },
    { "\\Device\\Harddisk0\\Partition4",        PATH_DATA,  SUB_CACHE_TITLE },
    { "\\Device\\Harddisk0\\Partition5",        PATH_DATA,  SUB_CACHE_TITLE },
    { "D:\\",                                   PATH_GAME,  SUB_NONE        },
    /* Y: is the Xbox dashboard partition; the dashboard opens its assets
     * (e.g. "Y:\default.xip") from there. Map it to the game dir. */
    { "Y:\\",                                   PATH_GAME,  SUB_NONE        },
    /* XAPI links T: and U: to E:\TDATA\<title id> and E:\UDATA\<title id>
     * itself (resolve_symlink follows those); these are for titles that do
     * not, and land in the same folders. */
    { "T:\\",                                   PATH_TDATA, SUB_TDATA_TITLE },
    { "U:\\",                                   PATH_SAVE,  SUB_UDATA_TITLE },
    { "Z:\\",                                   PATH_DATA,  SUB_CACHE_TITLE },
    /* Memory units: never inserted (ports save to the hard disk only).
     * Without these the paths fell through to the host as "F:\..." -- a
     * host drive. */
    { "\\Device\\MU_",                          PATH_NONE,  SUB_NONE        },
    { "F:\\", PATH_NONE, SUB_NONE }, { "G:\\", PATH_NONE, SUB_NONE },
    { "H:\\", PATH_NONE, SUB_NONE }, { "I:\\", PATH_NONE, SUB_NONE },
    { "J:\\", PATH_NONE, SUB_NONE }, { "K:\\", PATH_NONE, SUB_NONE },
    { "L:\\", PATH_NONE, SUB_NONE }, { "M:\\", PATH_NONE, SUB_NONE },
};
#define PATH_RULE_COUNT ((int)(sizeof(s_rules) / sizeof(s_rules[0])))

static XboxTdataPlace s_tdata_place = XBOX_TDATA_SAVES;
static uint32_t       s_title_id;

/* The sub-folder of a rule, backslash form, with the title ID filled in. */
static void rule_sub(int sub, char *out, size_t n)
{
    switch (sub) {
    case SUB_UDATA:       snprintf(out, n, "\\UDATA"); break;
    case SUB_TDATA:       snprintf(out, n, "\\TDATA"); break;
    case SUB_UDATA_TITLE: snprintf(out, n, "\\UDATA\\%08X", s_title_id); break;
    case SUB_TDATA_TITLE: snprintf(out, n, "\\TDATA\\%08X", s_title_id); break;
    case SUB_CACHE_TITLE: snprintf(out, n, "\\cache\\%08X", s_title_id); break;
    case SUB_SYSTEMDATA:  snprintf(out, n, "\\SystemData"); break;
    default:              out[0] = '\0'; break;
    }
}

/* A "\??\X:" path is the drive "X:". */
static const char *strip_dos_devices(const char *p)
{
    if (match_prefix(p, "\\??\\") && p[4] && p[5] == ':')
        return p + 4;
    return p;
}

/* The rule for a path, or -1. */
static int find_rule(const char *xbox_path, const char **remainder)
{
    const char *p = strip_dos_devices(xbox_path);
    int i, skip;
    for (i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_prefix(p, s_rules[i].prefix);
        if (skip) {
            *remainder = p + skip;
            /* A prefix without a trailing separator leaves it here. */
            while (**remainder == '\\' || **remainder == '/')
                (*remainder)++;
            return i;
        }
    }
    return -1;
}

/*
 * Rewrite a path through a drive letter the title mapped for itself.
 *
 * Titles do their own mounting. Wreckless calls IoCreateSymbolicLink at guest
 * 0x000E9B1D to link "\??\Z:" to "\Device\Harddisk0\Partition1\", then loads
 * every asset through z:\. kernel_io.c has recorded that link since it was
 * written, but nothing ever read the table back, so the static rule below --
 * Z: is the cache partition, which is true of the console in general and wrong
 * for this title -- sent every asset open to the save directory and it failed
 * with ERROR_FILE_NOT_FOUND.
 *
 * Accepts "Z:\rest" and "\??\Z:\rest"; the table is keyed on the "\??\Z:" form
 * the kernel actually registers.
 *
 * Deliberately conservative: the rewritten path is returned only when it
 * matches a static rule. A link whose target this layer has no rule for would
 * otherwise turn a path that translated adequately into one that does not
 * translate at all.
 *
 * Returns 1 and fills `out` on success, 0 to leave the path alone.
 */
static int resolve_symlink(const char* xbox_path, char* out, size_t out_size)
{
    char        link[8];
    const char* rest;
    const char* target;
    const char* unused;
    size_t      tlen, rlen;

    if (!xbox_path || !out || out_size == 0)
        return 0;

    xbox_path = strip_dos_devices(xbox_path);
    if (!xbox_path[0] || xbox_path[1] != ':')
        return 0;
    rest = xbox_path + 2;

    link[0] = '\\';
    link[1] = '?';
    link[2] = '?';
    link[3] = '\\';
    link[4] = (char)toupper((unsigned char)xbox_path[0]);
    link[5] = ':';
    link[6] = '\0';

    target = xbox_LookupSymbolicLink(link);
    if (!target || !target[0])
        return 0;

    while (*rest == '\\' || *rest == '/')
        rest++;

    tlen = strlen(target);
    rlen = strlen(rest);
    if (tlen + rlen + 2 > out_size)
        return 0;

    memcpy(out, target, tlen);
    if (tlen && target[tlen - 1] != '\\' && target[tlen - 1] != '/')
        out[tlen++] = '\\';
    memcpy(out + tlen, rest, rlen);
    out[tlen + rlen] = '\0';

    return find_rule(out, &unused) >= 0;   /* the target is somewhere we can place */
}

FILE *xbox_fopen_utf8(const char *path, const char *mode)
{
#if defined(_WIN32)
    WCHAR wp[XBOX_HOST_PATH], wm[8];
    if (!path || !MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, XBOX_HOST_PATH)
        || !MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 8))
        return NULL;
    return _wfopen(wp, wm);
#else
    return path ? fopen(path, mode) : NULL;
#endif
}

const char *xbox_getenv_utf8(const char *name)
{
#if defined(_WIN32)
    static struct { char name[40]; char *value; } cache[16];
    WCHAR wn[40];
    const WCHAR *wv;
    int i, n;
    for (i = 0; i < 16 && cache[i].name[0]; i++)
        if (!strcmp(cache[i].name, name))
            return cache[i].value;
    if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, wn, 40))
        return NULL;
    wv = _wgetenv(wn);
    if (!wv)
        return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, wv, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return NULL;
    {
        char *v = (char *)malloc((size_t)n);
        if (!v)
            return NULL;
        WideCharToMultiByte(CP_UTF8, 0, wv, -1, v, n, NULL, NULL);
        if (i < 16) {
            snprintf(cache[i].name, sizeof cache[i].name, "%s", name);
            cache[i].value = v;
        }
        return v;
    }
#else
    return getenv(name);
#endif
}

void xbox_path_config_default(XboxPathConfig *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->tdata = XBOX_TDATA_SAVES;
}

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    xbox_path_init_ex(game_dir, save_dir, NULL);
}

void xbox_path_init_ex(const char* game_dir, const char* save_dir,
                       const char* cache_dir)
{
    XboxPathConfig cfg;
    xbox_path_config_default(&cfg);
    cfg.game_dir = game_dir;
    cfg.save_dir = save_dir;
    cfg.data_dir = cache_dir;
    xbox_path_init_cfg(&cfg);
}

/* Shared by both backends, for the same reason as xbox_LastFileError: the
 * bridge calls it unconditionally, and a _WIN32-only definition breaks every
 * POSIX link. The Win32 backend fills this during translation; the POSIX one
 * does not yet, so it reads empty there. */
static XBOX_THREAD_LOCAL wchar_t s_last_host_path_shared[XBOX_HOST_PATH];

const wchar_t *xbox_LastHostPath(void)
{
    return s_last_host_path_shared;
}

/* ======================================================================== */
#if defined(_WIN32)
/* ======================================================================== */

#include <shlobj.h>
#include <winioctl.h>

static WCHAR s_game_dir[XBOX_HOST_PATH];
static WCHAR s_save_dir[XBOX_HOST_PATH];
static WCHAR s_cache_dir[XBOX_HOST_PATH];
static BOOL  s_initialized = FALSE;

/*
 * The raw disk device, \Device\Harddisk0\Partition0.
 *
 * A title opens it to read the Xbox partition table at sector 4 (offset 0x800)
 * and find the cache partitions behind X:, Y: and Z: -- XAPI's utility-drive
 * mount does exactly that, and DSTEAL_JP calls HalReturnToFirmware when the
 * read fails. A directory cannot answer a 512-byte read at a file offset, so
 * the device is backed by an image file instead.
 *
 * The table is the standard retail geometry. This is emulating a device that
 * has to be there, not fabricating anything the title owns.
 */
#define XBOX_DISK_IMAGE_NAME   L"Partition0.img"
#define XBOX_PART_TABLE_OFFSET 0x800
#define XBOX_PART_IN_USE       0x80000000u

static void xbox_write_partition_table(const WCHAR *path)
{
    /* name[16], flags, lba_start, lba_size, reserved -- 32 bytes each */
    static const struct { const char *name; ULONG start, size; } parts[] = {
        { "XBOX_PART_X",  0x00000400, 0x00177000 },  /* X: cache      */
        { "XBOX_PART_Y",  0x00177400, 0x00177000 },  /* Y: cache      */
        { "XBOX_PART_Z",  0x002EE400, 0x00177000 },  /* Z: cache      */
        { "XBOX_PART_C",  0x00465400, 0x000FA000 },  /* C: system     */
        { "XBOX_PART_E",  0x0055F400, 0x00465400 },  /* E: game/save  */
    };
    unsigned char sector[512];
    HANDLE h;
    DWORD written;
    LARGE_INTEGER off;
    size_t i;

    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;

    memset(sector, 0, sizeof(sector));
    memcpy(sector, "****PARTINFO****", 16);
    for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        unsigned char *e = sector + 48 + i * 32;   /* 16 magic + 32 reserved */
        size_t n = strlen(parts[i].name);
        memset(e, ' ', 16);
        memcpy(e, parts[i].name, n < 16 ? n : 16);
        *(ULONG *)(e + 16) = XBOX_PART_IN_USE;
        *(ULONG *)(e + 20) = parts[i].start;
        *(ULONG *)(e + 24) = parts[i].size;
        *(ULONG *)(e + 28) = 0;
    }

    off.QuadPart = XBOX_PART_TABLE_OFFSET;
    if (SetFilePointerEx(h, off, NULL, FILE_BEGIN))
        WriteFile(h, sector, sizeof(sector), &written, NULL);
    CloseHandle(h);
}

/*
 * Map a partition device onto an image file.
 *
 * Matches "\Device\Harddisk0\PartitionN" with nothing below it, which is how a
 * device is opened. Anything with a path under it -- Partition1\TDATA and the
 * like -- is an ordinary filesystem access and falls through to the rules
 * table, which puts it in a directory.
 *
 * Partition0 is the whole disk and carries the partition table written above.
 * The rest start empty, which is what an unformatted partition looks like, so
 * a title that wants a filesystem there formats one.
 */
static BOOL xbox_partition_device_path(const char *xbox_path, WCHAR *out, DWORD n)
{
    static const char *prefix = "\\Device\\Harddisk0\\Partition";
    int len = match_prefix(xbox_path, prefix);
    int digit;
    const char *rest;

    if (!len)
        return FALSE;
    digit = xbox_path[len];
    if (digit < '0' || digit > '9')
        return FALSE;

    rest = xbox_path + len + 1;
    /* Nothing below it, allowing for a single trailing separator. */
    if (*rest == '\\' || *rest == '/')
        rest++;
    if (*rest != '\0')
        return FALSE;

    swprintf_s(out, n, L"%s\\Partition%c.img", s_cache_dir, (WCHAR)digit);
    return TRUE;
}


/* The layout of save_dir. 1: UDATA\ and TDATA\ (or TDATA in data), the
 * identity file, .pending\ for unfinished save writes. 0 is everything before
 * this file existed. Raise it when a release moves anything in save_dir and
 * add the step to storage_migrate. */
#define XBOX_STORAGE_LAYOUT 1

static void utf8_to_wide(const char *s, WCHAR *out, int n)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n))
        out[0] = 0;
}

static void strip_trailing_sep(WCHAR *s)
{
    size_t len = wcslen(s);
    while (len > 3 && (s[len - 1] == L'\\' || s[len - 1] == L'/'))
        s[--len] = L'\0';
}

static int dir_exists(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Move src into dst, merging folders. Nothing is ever deleted or
 * overwritten: an entry whose name is taken at the destination is moved
 * next to it as "<name>.migrated", "<name>.migrated2", ... Returns the
 * number of entries moved. */
static int move_merge(const WCHAR *src, const WCHAR *dst)
{
    WIN32_FIND_DATAW fd;
    WCHAR pat[XBOX_HOST_PATH], s[XBOX_HOST_PATH], d[XBOX_HOST_PATH];
    HANDLE f;
    int moved = 0;

    if (!dir_exists(src))
        return 0;
    if (!dir_exists(dst)) {
        SHCreateDirectoryExW(NULL, dst, NULL);
        /* An empty destination: the whole folder in one move. */
    }
    swprintf_s(pat, XBOX_HOST_PATH, L"%s\\*", src);
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        swprintf_s(s, XBOX_HOST_PATH, L"%s\\%s", src, fd.cFileName);
        swprintf_s(d, XBOX_HOST_PATH, L"%s\\%s", dst, fd.cFileName);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && dir_exists(d)) {
            moved += move_merge(s, d);
            RemoveDirectoryW(s);            /* only if now empty */
            continue;
        }
        if (GetFileAttributesW(d) != INVALID_FILE_ATTRIBUTES) {
            int k;
            for (k = 1; k < 100; k++) {
                swprintf_s(d, XBOX_HOST_PATH, k == 1 ? L"%s\\%s.migrated" : L"%s\\%s.migrated%d",
                           dst, fd.cFileName, k);
                if (GetFileAttributesW(d) == INVALID_FILE_ATTRIBUTES)
                    break;
            }
        }
        if (MoveFileExW(s, d, MOVEFILE_WRITE_THROUGH)) {
            moved++;
            fprintf(stderr, "  [STORAGE] moved %S -> %S\n", s, d);
        } else {
            fprintf(stderr, "  [STORAGE] could not move %S (error %lu), left in place\n",
                    s, GetLastError());
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    RemoveDirectoryW(src);                  /* only if now empty */
    return moved;
}

/* Bring save_dir to XBOX_STORAGE_LAYOUT, and E:\TDATA to where this title
 * keeps it. Runs at every start; does nothing once everything is in place.
 * Moves only -- an old copy that cannot be moved stays where it is. */
static void storage_migrate(const WCHAR *game, const WCHAR *save, const WCHAR *data,
                            const WCHAR *tdata_base)
{
    WCHAR f[XBOX_HOST_PATH], a[XBOX_HOST_PATH], b[XBOX_HOST_PATH];
    int layout = 0, n = 0;
    char place[16] = "";
    FILE *fp;

    swprintf_s(f, XBOX_HOST_PATH, L"%s\\storage_layout.txt", save);
    fp = _wfopen(f, L"r");
    if (fp) {
        if (fscanf(fp, "layout %d tdata %15s", &layout, place) < 1)
            layout = 0;
        fclose(fp);
    }

    if (layout < 1) {
        /* Layout 0 (before this file): E:\UDATA and E:\TDATA in the game
         * folder (until fork 29de70a), U: in save\UserData, T: in
         * data\TitleData. */
        swprintf_s(a, XBOX_HOST_PATH, L"%s\\UDATA", game);
        swprintf_s(b, XBOX_HOST_PATH, L"%s\\UDATA", save);
        n += move_merge(a, b);
        swprintf_s(a, XBOX_HOST_PATH, L"%s\\TDATA", game);
        swprintf_s(b, XBOX_HOST_PATH, L"%s\\TDATA", tdata_base);
        n += move_merge(a, b);
        swprintf_s(a, XBOX_HOST_PATH, L"%s\\UserData", save);
        swprintf_s(b, XBOX_HOST_PATH, L"%s\\UDATA\\%08X", save, s_title_id);
        n += move_merge(a, b);
        swprintf_s(a, XBOX_HOST_PATH, L"%s\\TitleData", data);
        swprintf_s(b, XBOX_HOST_PATH, L"%s\\TDATA\\%08X", tdata_base, s_title_id);
        n += move_merge(a, b);
        /* fork 29de70a put E:\TDATA in data; layout 1 keeps it in saves
         * unless the title chose data. */
        if (_wcsicmp(tdata_base, data) != 0) {
            swprintf_s(a, XBOX_HOST_PATH, L"%s\\TDATA", data);
            swprintf_s(b, XBOX_HOST_PATH, L"%s\\TDATA", tdata_base);
            n += move_merge(a, b);
        }
    }

    /* E:\TDATA moves with the title's choice. */
    {
        const char *want = s_tdata_place == XBOX_TDATA_DATA ? "data" : "saves";
        if (layout >= 1 && place[0] && strcmp(place, want) != 0) {
            const WCHAR *from = s_tdata_place == XBOX_TDATA_DATA ? save : data;
            swprintf_s(a, XBOX_HOST_PATH, L"%s\\TDATA", from);
            swprintf_s(b, XBOX_HOST_PATH, L"%s\\TDATA", tdata_base);
            n += move_merge(a, b);
        }
        if (layout != XBOX_STORAGE_LAYOUT || strcmp(place, want) != 0) {
            fp = _wfopen(f, L"w");
            if (fp) {
                fprintf(fp, "layout %d tdata %s\n", XBOX_STORAGE_LAYOUT, want);
                fprintf(fp, "# Written by the runtime; do not edit. A newer build migrates once\n"
                            "# from the layout named here (moves, never deletes).\n");
                fclose(fp);
            }
            fprintf(stderr, "  [STORAGE] layout %d -> %d (tdata in %s), %d item(s) moved\n",
                    layout, XBOX_STORAGE_LAYOUT, want, n);
        }
    }
}

static WCHAR s_pending_dir[XBOX_HOST_PATH];

/* Unfinished atomic save writes from a run that died mid-save: the save they
 * were going to replace is intact, so these are incomplete copies. */
static void pending_cleanup(void)
{
    WIN32_FIND_DATAW fd;
    WCHAR pat[XBOX_HOST_PATH], p[XBOX_HOST_PATH];
    HANDLE f;

    swprintf_s(pat, XBOX_HOST_PATH, L"%s\\*", s_pending_dir);
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        swprintf_s(p, XBOX_HOST_PATH, L"%s\\%s", s_pending_dir, fd.cFileName);
        if (DeleteFileW(p))
            fprintf(stderr, "  [STORAGE] dropped unfinished save write %S "
                            "(the save it would have replaced is intact)\n", fd.cFileName);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

void xbox_path_init_cfg(const XboxPathConfig *cfg)
{
    WCHAR save_base[MAX_PATH], full[XBOX_HOST_PATH];

    s_tdata_place = cfg->tdata;
    s_title_id = cfg->title_id ? cfg->title_id : g_xbox_title_id;

    /* The fallbacks used to name Burnout 3 specifically, so any other title
     * that passed NULL silently pointed its game dir and its saves at another
     * game's folders. Generic now -- a caller that wants a title-specific
     * location should pass one. */
    if (cfg->game_dir)
        utf8_to_wide(cfg->game_dir, s_game_dir, XBOX_HOST_PATH);
    else
        GetCurrentDirectoryW(XBOX_HOST_PATH, s_game_dir);

    if (cfg->save_dir) {
        utf8_to_wide(cfg->save_dir, s_save_dir, XBOX_HOST_PATH);
    } else if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, save_base))) {
        swprintf_s(s_save_dir, XBOX_HOST_PATH, L"%s\\xboxrecomp", save_base);
    } else {
        GetCurrentDirectoryW(XBOX_HOST_PATH, s_save_dir);
        wcscat_s(s_save_dir, XBOX_HOST_PATH, L"\\SaveData");
    }
    if (cfg->data_dir)
        utf8_to_wide(cfg->data_dir, s_cache_dir, XBOX_HOST_PATH);
    else
        wcscpy_s(s_cache_dir, XBOX_HOST_PATH, s_save_dir);

    /* Absolute, because SHCreateDirectoryExW below only accepts a fully
     * qualified path: handed the relative "saves" a host passes, it fails with
     * ERROR_BAD_PATHNAME and creates nothing. The partition images then cannot
     * be created either, and a title that opens \Device\Harddisk0\Partition1\
     * at boot gets STATUS_OBJECT_PATH_NOT_FOUND and quits to the dashboard.
     * It only ever worked where someone had made the directory by hand. */
    if (GetFullPathNameW(s_game_dir, XBOX_HOST_PATH, full, NULL))
        wcscpy_s(s_game_dir, XBOX_HOST_PATH, full);
    if (GetFullPathNameW(s_save_dir, XBOX_HOST_PATH, full, NULL))
        wcscpy_s(s_save_dir, XBOX_HOST_PATH, full);
    if (GetFullPathNameW(s_cache_dir, XBOX_HOST_PATH, full, NULL))
        wcscpy_s(s_cache_dir, XBOX_HOST_PATH, full);
    strip_trailing_sep(s_game_dir);
    strip_trailing_sep(s_save_dir);
    strip_trailing_sep(s_cache_dir);
    swprintf_s(s_pending_dir, XBOX_HOST_PATH, L"%s\\.pending", s_save_dir);

    /* Create the directories titles expect. A title that opens a file there
     * with a create disposition fails if the parent does not exist -- which
     * reads as "cannot create save file" and sends the title down its
     * init-failure path. Halo asserts exactly that at saved
     * games/game_state_xbox.c:97 and then unwinds, clearing global_d3d_device
     * on the way out, so a missing directory surfaces as a graphics failure.
     *
     * Cheap and idempotent: SHCreateDirectoryExW builds intermediates and is
     * happy if they already exist. */
    {
        const WCHAR *tdata = s_tdata_place == XBOX_TDATA_DATA ? s_cache_dir : s_save_dir;
        WCHAR image[XBOX_HOST_PATH];
        WCHAR dir[XBOX_HOST_PATH];

        SHCreateDirectoryExW(NULL, s_save_dir, NULL);
        SHCreateDirectoryExW(NULL, s_cache_dir, NULL);
        storage_migrate(s_game_dir, s_save_dir, s_cache_dir, tdata);
        SHCreateDirectoryExW(NULL, s_pending_dir, NULL);
        SetFileAttributesW(s_pending_dir, FILE_ATTRIBUTE_HIDDEN);
        pending_cleanup();
        swprintf_s(dir, XBOX_HOST_PATH, L"%s\\UDATA\\%08X", s_save_dir, s_title_id);
        SHCreateDirectoryExW(NULL, dir, NULL);
        swprintf_s(dir, XBOX_HOST_PATH, L"%s\\TDATA\\%08X", tdata, s_title_id);
        SHCreateDirectoryExW(NULL, dir, NULL);
        swprintf_s(dir, XBOX_HOST_PATH, L"%s\\cache\\%08X", s_cache_dir, s_title_id);
        SHCreateDirectoryExW(NULL, dir, NULL);
        swprintf_s(dir, XBOX_HOST_PATH, L"%s\\SystemData", s_cache_dir);
        SHCreateDirectoryExW(NULL, dir, NULL);
        swprintf_s(image, XBOX_HOST_PATH, L"%s\\%s", s_cache_dir, XBOX_DISK_IMAGE_NAME);
        xbox_write_partition_table(image);
        /* The other partition devices, sized to the same geometry the table
         * above describes, so a title that asks the device how big it is gets
         * an answer consistent with the table it just read. Marked sparse
         * first: the cache partitions are 750 MB each and none of that is
         * touched until something writes to it. */
        {
            static const ULONGLONG part_sectors[6] = {
                0,             /* 0: whole disk, sized below      */
                0x00465400ull, /* 1: E: game and saves            */
                0x000FA000ull, /* 2: C: system                    */
                0x00177000ull, /* 3: X: cache                     */
                0x00177000ull, /* 4: Y: cache                     */
                0x00177000ull, /* 5: Z: cache                     */
            };
            for (int p = 1; p <= 5; p++) {
                HANDLE h;
                DWORD ret;
                LARGE_INTEGER end;

                swprintf_s(image, XBOX_HOST_PATH, L"%s\\Partition%d.img",
                           s_cache_dir, p);
                h = CreateFileW(image, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (h == INVALID_HANDLE_VALUE)
                    continue;
                DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0,
                                &ret, NULL);
                end.QuadPart = (LONGLONG)(part_sectors[p] * 512ull);
                if (SetFilePointerEx(h, end, NULL, FILE_BEGIN))
                    SetEndOfFile(h);
                CloseHandle(h);
            }
        }
    }

    /* The console identity saves are signed with (kernel_xbox.c). */
    {
        char path8[XBOX_HOST_PATH * 3];
        WCHAR idf[XBOX_HOST_PATH];
        swprintf_s(idf, XBOX_HOST_PATH, L"%s\\console.txt", s_save_dir);
        if (WideCharToMultiByte(CP_UTF8, 0, idf, -1, path8, (int)sizeof path8, NULL, NULL))
            xbox_identity_load(path8);
    }

    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%S, save=%S, data=%S, tdata in %s, title %08X",
             s_game_dir, s_save_dir, s_cache_dir,
             s_tdata_place == XBOX_TDATA_DATA ? "data" : "saves", s_title_id);
    fprintf(stderr, "  [STORAGE] game=%S\n  [STORAGE] saves=%S\n  [STORAGE] data=%S (TDATA in %s, title %08X)\n",
            s_game_dir, s_save_dir, s_cache_dir,
            s_tdata_place == XBOX_TDATA_DATA ? "data" : "saves", s_title_id);
}

int xbox_path_in_save_dir(const xbox_host_char *host_path)
{
    const WCHAR *p = host_path;
    size_t n = wcslen(s_save_dir);
    if (!p || !n)
        return 0;
    if (!wcsncmp(p, L"\\\\?\\", 4))
        p += 4;
    return _wcsnicmp(p, s_save_dir, n) == 0 && (p[n] == L'\\' || p[n] == 0);
}

const xbox_host_char *xbox_path_pending_dir(void)
{
    return s_pending_dir;
}

xbox_host_char *xbox_host_path_fix_case(xbox_host_char *path, size_t base_len)
{
    (void)base_len;            /* NTFS lookups are case-insensitive already */
    return path;
}

/* The host path the last translation produced.
 *
 * A caller that wants to act on the file a title just opened -- playing an FMV
 * the host can decode itself, say -- has the guest path but not the host one,
 * and re-deriving it would duplicate every rule above. */
/* Thread-local: several threads open files at once, and a plain static let one
 * thread's translation overwrite another's between the translate and the read.
 * That showed up as the FMV trigger firing on roughly two runs in three. */

static void xbox_remember_host_path(const wchar_t *p)
{
    if (p) wcsncpy_s(s_last_host_path_shared, XBOX_HOST_PATH, p, _TRUNCATE);
}

BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char*  remainder = NULL;
    const WCHAR* base_dir  = NULL;
    char         sub[64];
    int          r;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!s_initialized)
        xbox_path_init(NULL, NULL);

    {
        char linked[512];
        if (resolve_symlink(xbox_path, linked, sizeof(linked)))
            return xbox_translate_path(linked, host_path_buf, buf_size);
    }

    if (xbox_partition_device_path(xbox_path, host_path_buf, buf_size)) {
        fprintf(stderr, "  [PATH] %s -> partition image\n", xbox_path);
        fflush(stderr);
        return TRUE;
    }

    r = find_rule(xbox_path, &remainder);
    if (r < 0 || s_rules[r].base == PATH_NONE) {
        /* Never handed to the host as written: "F:\..." is a host drive. */
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "%s Xbox path: %s",
                 r < 0 ? "Unrecognized" : "No such device (memory unit)", xbox_path);
        host_path_buf[0] = 0;
        return FALSE;
    }
    switch (s_rules[r].base) {
    case PATH_SAVE:  base_dir = s_save_dir; break;
    case PATH_DATA:  base_dir = s_cache_dir; break;
    case PATH_TDATA: base_dir = s_tdata_place == XBOX_TDATA_DATA ? s_cache_dir : s_save_dir; break;
    default:         base_dir = s_game_dir; break;
    }
    rule_sub(s_rules[r].sub, sub, sizeof sub);

    if (g_xbox_path_hook)
        g_xbox_path_hook(xbox_path);
    fprintf(stderr, "  [PATH] %s\n", xbox_path);
    fflush(stderr);
    {
        WCHAR remainder_wide[XBOX_HOST_PATH], sub_wide[64];
        WCHAR built[XBOX_HOST_PATH];

        /* Xbox file names are single-byte, in the console's Latin-1 code
         * page -- not the host's ANSI page, which differs by locale. */
        if (!MultiByteToWideChar(1252, 0, remainder, -1, remainder_wide, XBOX_HOST_PATH))
            return FALSE;
        MultiByteToWideChar(1252, 0, sub, -1, sub_wide, 64);
        for (WCHAR* p = remainder_wide; *p; p++) {
            if (*p == L'/') *p = L'\\';
        }

        if (sub[0]) {
            WCHAR dir_path[XBOX_HOST_PATH];
            swprintf_s(built, XBOX_HOST_PATH, L"%s%s\\%s", base_dir, sub_wide, remainder_wide);
            swprintf_s(dir_path, XBOX_HOST_PATH, L"%s%s", base_dir, sub_wide);
            SHCreateDirectoryExW(NULL, dir_path, NULL);
        } else {
            swprintf_s(built, XBOX_HOST_PATH, L"%s\\%s", base_dir, remainder_wide);
        }

        /* An empty remainder means the title opened the device itself, so
         * the join above leaves a trailing separator -- which CreateFileW
         * rejects even with FILE_FLAG_BACKUP_SEMANTICS, turning an existing
         * directory into STATUS_OBJECT_PATH_NOT_FOUND. */
        {
            size_t n = wcslen(built);
            while (n > 1 && (built[n - 1] == L'\\' || built[n - 1] == L'/'))
                built[--n] = L'\0';
            /* Long paths: the \\?\ form lifts MAX_PATH for the W APIs. The
             * base folders are absolute and normalised, so it is safe. */
            if (n >= MAX_PATH - 12 && wcsncmp(built, L"\\\\?\\", 4) != 0)
                swprintf_s(host_path_buf, buf_size, L"\\\\?\\%s", built);
            else
                wcsncpy_s(host_path_buf, buf_size, built, _TRUNCATE);
        }

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %S", xbox_path, host_path_buf);
        xbox_remember_host_path(host_path_buf);
        return TRUE;
    }
}

/* ======================================================================== */
#else /* !_WIN32  -- POSIX / Linux */
/* ======================================================================== */

#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <strings.h>
#include <errno.h>

static char s_game_dir[XBOX_HOST_PATH];
static char s_save_dir[XBOX_HOST_PATH];
static char s_cache_dir[XBOX_HOST_PATH];
static char s_pending_dir[XBOX_HOST_PATH];
static BOOL s_initialized = FALSE;

/* Strip a single trailing '/' (but never the root '/'). */
static void strip_trailing_slash(char* s)
{
    size_t len = strlen(s);
    if (len > 1 && s[len - 1] == '/')
        s[len - 1] = '\0';
}

/* Recursively create a directory and all missing parents. */
static void mkdir_p(const char* path)
{
    char tmp[XBOX_HOST_PATH];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return;
    memcpy(tmp, path, len + 1);

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
}

void xbox_path_init_cfg(const XboxPathConfig *cfg)
{
    char dir[XBOX_HOST_PATH];
    const char *tdata;

    s_tdata_place = cfg->tdata;
    s_title_id = cfg->title_id ? cfg->title_id : g_xbox_title_id;

    if (cfg->game_dir) {
        snprintf(s_game_dir, sizeof(s_game_dir), "%s", cfg->game_dir);
    } else {
        char cwd[XBOX_HOST_PATH];
        if (!getcwd(cwd, sizeof(cwd)))
            snprintf(cwd, sizeof(cwd), ".");
        snprintf(s_game_dir, sizeof(s_game_dir), "%s", cwd);
    }

    if (cfg->save_dir) {
        snprintf(s_save_dir, sizeof(s_save_dir), "%s", cfg->save_dir);
    } else {
        /* XDG base-directory spec: $XDG_DATA_HOME or ~/.local/share */
        const char* xdg = getenv("XDG_DATA_HOME");
        if (xdg && xdg[0]) {
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/xboxrecomp", xdg);
        } else {
            const char* home = getenv("HOME");
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/.local/share/xboxrecomp",
                     (home && home[0]) ? home : ".");
        }
    }

    snprintf(s_cache_dir, sizeof(s_cache_dir), "%s",
             cfg->data_dir ? cfg->data_dir : s_save_dir);

    strip_trailing_slash(s_game_dir);
    strip_trailing_slash(s_save_dir);
    strip_trailing_slash(s_cache_dir);
    snprintf(s_pending_dir, sizeof(s_pending_dir), "%s/.pending", s_save_dir);

    /* ponytail: no layout migration and no atomic save writes on POSIX yet
     * (Win32 has both, kernel_path.c / kernel_file.c); there is no POSIX
     * release with an older layout to migrate from. */
    tdata = s_tdata_place == XBOX_TDATA_DATA ? s_cache_dir : s_save_dir;
    snprintf(dir, sizeof dir, "%s/UDATA/%08X", s_save_dir, s_title_id);
    mkdir_p(dir);
    snprintf(dir, sizeof dir, "%s/TDATA/%08X", tdata, s_title_id);
    mkdir_p(dir);
    snprintf(dir, sizeof dir, "%s/cache/%08X", s_cache_dir, s_title_id);
    mkdir_p(dir);
    mkdir_p(s_pending_dir);

    snprintf(dir, sizeof dir, "%s/console.txt", s_save_dir);
    xbox_identity_load(dir);

    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%s, save=%s, data=%s, tdata in %s",
             s_game_dir, s_save_dir, s_cache_dir,
             s_tdata_place == XBOX_TDATA_DATA ? "data" : "saves");
}

int xbox_path_in_save_dir(const xbox_host_char *host_path)
{
    size_t n = strlen(s_save_dir);
    return host_path && n && strncmp(host_path, s_save_dir, n) == 0
        && (host_path[n] == '/' || host_path[n] == 0);
}

const xbox_host_char *xbox_path_pending_dir(void)
{
    return s_pending_dir;
}

/* FATX is case-insensitive and titles rely on it: a file the disc has as
 * "Frontend/MainMenu.PANEL" is opened as "frontend\MainMenu.PANEL". On a
 * case-sensitive host each component from base_len on that does not exist as
 * written is looked up in its directory without regard to case, and the
 * first match replaces it. A component that does not exist at all is left as
 * written (a file about to be created). */
xbox_host_char *xbox_host_path_fix_case(xbox_host_char *path, size_t base_len)
{
    size_t pos = base_len;
    struct stat st;

    if (!path || stat(path, &st) == 0)
        return path;                         /* exists as written */
    while (path[pos] == '/')
        pos++;
    while (path[pos]) {
        size_t end = pos;
        char saved;
        while (path[end] && path[end] != '/')
            end++;
        saved = path[end];
        path[end] = '\0';
        if (stat(path, &st) != 0) {
            /* Look for this component in its parent. */
            char *slash = strrchr(path, '/');
            const char *name = slash ? slash + 1 : path;
            DIR *d;
            if (slash) *slash = '\0';
            d = opendir(slash ? (path[0] ? path : "/") : ".");
            if (slash) *slash = '/';
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)) != NULL) {
                    if (strlen(e->d_name) == strlen(name) && !strcasecmp(e->d_name, name)) {
                        memcpy((char *)name, e->d_name, strlen(name));
                        break;
                    }
                }
                closedir(d);
            }
        }
        path[end] = saved;
        if (!saved)
            break;
        pos = end + 1;
    }
    return path;
}

BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char* remainder = NULL;
    const char* base_dir  = NULL;
    char        sub[64];
    int         r;
    size_t      base_len;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!s_initialized)
        xbox_path_init(NULL, NULL);

    {
        char linked[512];
        if (resolve_symlink(xbox_path, linked, sizeof(linked)))
            return xbox_translate_path(linked, host_path_buf, buf_size);
    }

    r = find_rule(xbox_path, &remainder);
    if (r < 0 || s_rules[r].base == PATH_NONE) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "%s Xbox path: %s",
                 r < 0 ? "Unrecognized" : "No such device (memory unit)", xbox_path);
        host_path_buf[0] = 0;
        return FALSE;
    }
    switch (s_rules[r].base) {
    case PATH_SAVE:  base_dir = s_save_dir; break;
    case PATH_DATA:  base_dir = s_cache_dir; break;
    case PATH_TDATA: base_dir = s_tdata_place == XBOX_TDATA_DATA ? s_cache_dir : s_save_dir; break;
    default:         base_dir = s_game_dir; break;
    }
    rule_sub(s_rules[r].sub, sub, sizeof sub);
    for (char *p = sub; *p; p++)
        if (*p == '\\') *p = '/';
    base_len = strlen(base_dir) + strlen(sub);

    {
        char remainder_posix[XBOX_HOST_PATH];
        snprintf(remainder_posix, sizeof(remainder_posix), "%s", remainder);

        /* Xbox paths use backslashes -> POSIX slashes. */
        for (char* p = remainder_posix; *p; p++)
            if (*p == '\\') *p = '/';

        if (sub[0]) {
            char dir_path[XBOX_HOST_PATH];
            snprintf(host_path_buf, buf_size, "%s%s/%s", base_dir, sub, remainder_posix);
            snprintf(dir_path, sizeof(dir_path), "%s%s", base_dir, sub);
            mkdir_p(dir_path);
        } else {
            snprintf(host_path_buf, buf_size, "%s/%s", base_dir, remainder_posix);
        }

        {
            size_t n = strlen(host_path_buf);
            while (n > 1 && host_path_buf[n - 1] == '/')
                host_path_buf[--n] = '\0';
        }
        xbox_host_path_fix_case(host_path_buf, base_len);

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %s", xbox_path, host_path_buf);
        return TRUE;
    }
}

#endif /* _WIN32 */
