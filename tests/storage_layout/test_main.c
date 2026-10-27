/* storage_layout -- the storage layout for ports (docs/storage.md):
 * layout migration, path rules, atomic save writes, console identity.
 * Win32 only (the atomic writes and the migration are Win32 so far). */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *recomp_lookup(ULONG a) { (void)a; abort(); }
void *recomp_lookup_manual(ULONG a) { (void)a; abort(); }

static int failures;
#define CHECK(c, what) do { if (c) printf("  ok    %s\n", what); \
    else { printf("  FAIL  %s\n", what); failures++; } } while (0)

static char T[MAX_PATH], GAME[MAX_PATH], SAVES[MAX_PATH], DATA[MAX_PATH];

static void put(const char *path, const char *text)
{
    char dir[MAX_PATH], *s;
    FILE *f;
    snprintf(dir, sizeof dir, "%s", path);
    for (s = dir + 3; *s; s++)
        if (*s == '\\') { *s = 0; CreateDirectoryA(dir, NULL); *s = '\\'; }
    f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static int has(const char *path, const char *text)
{
    char buf[256] = "";
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return text ? strcmp(buf, text) == 0 : 1;
}

static int exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

static int pending_count(void)
{
    char pat[MAX_PATH];
    WCHAR wpat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int n = 0;
    snprintf(pat, sizeof pat, "%s\\.pending\\*", SAVES);    /* UTF-8: wide API */
    MultiByteToWideChar(CP_UTF8, 0, pat, -1, wpat, MAX_PATH);
    f = FindFirstFileW(wpat, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) n++; } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}

static void init(XboxTdataPlace place)
{
    XboxPathConfig c;
    xbox_path_config_default(&c);
    c.game_dir = GAME; c.save_dir = SAVES; c.data_dir = DATA;
    c.tdata = place; c.title_id = 0xABCD0001u;
    xbox_path_init_cfg(&c);
}

static int translates(const char *xbox, const char *want_suffix)
{
    WCHAR out[XBOX_HOST_PATH];
    char got[XBOX_HOST_PATH * 2];
    size_t gl, wl;
    if (!xbox_translate_path(xbox, out, XBOX_HOST_PATH))
        return want_suffix == NULL;
    if (!want_suffix) return 0;
    WideCharToMultiByte(CP_UTF8, 0, out, -1, got, sizeof got, NULL, NULL);
    gl = strlen(got); wl = strlen(want_suffix);
    return gl >= wl && _stricmp(got + gl - wl, want_suffix) == 0;
}

static HANDLE open_file(const char *xbox, ACCESS_MASK access, ULONG disposition)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK io;
    HANDLE h = NULL;
    name.Length = (USHORT)strlen(xbox);
    name.MaximumLength = name.Length + 1;
    name.Buffer = (PCHAR)xbox;
    memset(&oa, 0, sizeof oa);
    oa.ObjectName = &name;
    if (xbox_NtCreateFile(&h, access, &oa, &io, NULL, 0, 3, disposition, 0) != 0)
        return NULL;
    return h;
}

static void write_at(HANDLE h, const char *text, LONGLONG at)
{
    XBOX_IO_STATUS_BLOCK io;
    LARGE_INTEGER off;
    off.QuadPart = at;
    xbox_NtWriteFile(h, NULL, NULL, NULL, &io, (PVOID)text, (ULONG)strlen(text), &off);
}

int main(void)
{
    char p[MAX_PATH], q[MAX_PATH];
    const char *save = "\\Device\\Harddisk0\\Partition1\\UDATA\\ABCD0001\\slot\\a.sav";

    GetTempPathA(MAX_PATH, T);
    snprintf(p, sizeof p, "%sxr-storage-%lu", T, GetCurrentProcessId());
    snprintf(T, sizeof T, "%s", p);
    snprintf(GAME, sizeof GAME, "%s\\game", T);
    snprintf(SAVES, sizeof SAVES, "%s\\sa ves \xC3\xBC", T);     /* space + umlaut (UTF-8) */
    snprintf(DATA, sizeof DATA, "%s\\data", T);
    CreateDirectoryA(T, NULL);
    CreateDirectoryA(GAME, NULL);

    printf("migration from layout 0\n");
    snprintf(p, sizeof p, "%s\\UDATA\\ABCD0001\\slot\\a.sav", GAME); put(p, "old");
    snprintf(p, sizeof p, "%s\\TDATA\\ABCD0001\\c.bin", GAME); put(p, "cache");
    snprintf(p, sizeof p, "%s\\data\\TitleData\\t.bin", T); put(p, "tdata");
    {
        /* save_dir holds non-ASCII: create it through the wide API */
        WCHAR w[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, SAVES, -1, w, MAX_PATH);
        CreateDirectoryW(w, NULL);
    }
    init(XBOX_TDATA_DATA);
    CHECK(translates(save, "\\UDATA\\ABCD0001\\slot\\a.sav"), "UDATA moved into saves and translated there");
    {
        WCHAR w[XBOX_HOST_PATH]; FILE *f;
        xbox_translate_path(save, w, XBOX_HOST_PATH);
        f = _wfopen(w, L"rb");
        CHECK(f != NULL, "migrated save readable under a non-ASCII save folder");
        if (f) fclose(f);
    }
    snprintf(p, sizeof p, "%s\\UDATA", GAME);
    CHECK(!exists(p), "game\\UDATA gone");
    snprintf(p, sizeof p, "%s\\TDATA\\ABCD0001\\c.bin", DATA);
    CHECK(has(p, "cache"), "game\\TDATA moved to data\\TDATA (title keeps caches there)");
    snprintf(p, sizeof p, "%s\\TDATA\\ABCD0001\\t.bin", DATA);
    CHECK(has(p, "tdata"), "old data\\TitleData (T:) moved to data\\TDATA\\<title id>");

    printf("path rules\n");
    CHECK(translates("T:\\x.bin", "\\data\\TDATA\\ABCD0001\\x.bin"), "T: -> data\\TDATA\\<title id>");
    CHECK(translates("U:\\y.bin", "\\UDATA\\ABCD0001\\y.bin"), "U: -> saves\\UDATA\\<title id>");
    CHECK(translates("\\??\\U:\\y.bin", "\\UDATA\\ABCD0001\\y.bin"), "\\??\\U: same as U:");
    CHECK(translates("Z:\\z.bin", "\\data\\cache\\ABCD0001\\z.bin"), "Z: -> data\\cache\\<title id>");
    CHECK(translates("\\Device\\Harddisk0\\Partition5\\f.bin", "\\data\\cache\\ABCD0001\\f.bin"), "cache partition -> data\\cache");
    CHECK(translates("D:\\default.xbe", "\\game\\default.xbe"), "D: -> game");
    CHECK(translates("F:\\save.bin", NULL), "F: (memory unit) not inserted");
    CHECK(translates("\\Device\\MU_0\\save.bin", NULL), "\\Device\\MU_0 not inserted");
    CHECK(translates("Q:\\x", NULL), "unknown drive fails (not passed to the host)");
    {
        char lp[700]; WCHAR w[XBOX_HOST_PATH];
        int i, n = snprintf(lp, sizeof lp, "U:\\");
        for (i = 0; i < 30; i++) n += snprintf(lp + n, sizeof lp - n, "long_folder_%02d\\", i);
        snprintf(lp + n, sizeof lp - n, "f.bin");
        CHECK(xbox_translate_path(lp, w, XBOX_HOST_PATH) && !wcsncmp(w, L"\\\\?\\", 4),
              "long path gets the \\\\?\\ prefix");
    }

    printf("atomic save writes\n");
    {
        HANDLE h = open_file(save, XBOX_GENERIC_WRITE, XBOX_FILE_OVERWRITE_IF);
        WCHAR w[XBOX_HOST_PATH]; char a8[XBOX_HOST_PATH * 2];
        xbox_translate_path(save, w, XBOX_HOST_PATH);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, a8, sizeof a8, NULL, NULL);
        CHECK(h != NULL, "save opened for overwrite");
        write_at(h, "new", 0);
        {
            FILE *f = _wfopen(w, L"rb"); char b[8] = ""; size_t n = 0;
            if (f) { n = fread(b, 1, 7, f); fclose(f); } b[n] = 0;
            CHECK(!strcmp(b, "old"), "while open: the save still holds the old data");
        }
        CHECK(pending_count() == 1, "one pending copy");
        xbox_NtClose(h);
        {
            FILE *f = _wfopen(w, L"rb"); char b[8] = ""; size_t n = 0;
            if (f) { n = fread(b, 1, 7, f); fclose(f); } b[n] = 0;
            CHECK(!strcmp(b, "new"), "after close: the new data is in place");
        }
        CHECK(pending_count() == 0, "pending copy committed");

        /* in place, contents kept */
        h = open_file(save, XBOX_GENERIC_WRITE | XBOX_GENERIC_READ, XBOX_FILE_OPEN);
        write_at(h, "X", 0);
        xbox_NtClose(h);
        {
            FILE *f = _wfopen(w, L"rb"); char b[8] = ""; size_t n = 0;
            if (f) { n = fread(b, 1, 7, f); fclose(f); } b[n] = 0;
            CHECK(!strcmp(b, "Xew"), "in-place update keeps the rest of the file");
        }

        /* killed mid-save: written, never closed */
        h = open_file(save, XBOX_GENERIC_WRITE, XBOX_FILE_OVERWRITE_IF);
        write_at(h, "half", 0);
        CloseHandle(h);                       /* the process dies: no commit */
        init(XBOX_TDATA_DATA);                /* next start */
        {
            FILE *f = _wfopen(w, L"rb"); char b[8] = ""; size_t n = 0;
            if (f) { n = fread(b, 1, 7, f); fclose(f); } b[n] = 0;
            CHECK(!strcmp(b, "Xew"), "interrupted save: the old save is intact");
        }
        CHECK(pending_count() == 0, "unfinished copy dropped at the next start");
        (void)a8;
    }

    printf("console identity\n");
    {
        static const unsigned char zero[16];
        CHECK(!memcmp(g_xbox_identity_hd_key, zero, 16) && !strcmp(g_xbox_identity_serial, "XR0000000000"),
              "fixed defaults without saves\\console.txt");
        snprintf(p, sizeof p, "%s\\console.txt", SAVES);
        {
            WCHAR w[MAX_PATH]; FILE *f;
            MultiByteToWideChar(CP_UTF8, 0, p, -1, w, MAX_PATH);
            f = _wfopen(w, L"w");
            if (f) { fputs("# test\nhd_key = 00112233445566778899AABBCCDDEEFF\nserial = 123456789012\n", f); fclose(f); }
        }
        init(XBOX_TDATA_DATA);
        CHECK(g_xbox_identity_hd_key[1] == 0x11 && g_xbox_identity_hd_key[15] == 0xFF
              && !strcmp(g_xbox_identity_serial, "123456789012"), "override from saves\\console.txt");
    }

    printf("TDATA follows the title's choice\n");
    init(XBOX_TDATA_SAVES);
    snprintf(q, sizeof q, "%s\\TDATA\\ABCD0001\\c.bin", SAVES);
    {
        WCHAR w[MAX_PATH]; MultiByteToWideChar(CP_UTF8, 0, q, -1, w, MAX_PATH);
        CHECK(GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES, "data\\TDATA moved to saves\\TDATA");
    }
    CHECK(translates("T:\\x.bin", "\\TDATA\\ABCD0001\\x.bin") && !translates("T:\\x.bin", "\\data\\TDATA\\ABCD0001\\x.bin"),
          "T: now in saves");

    printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
