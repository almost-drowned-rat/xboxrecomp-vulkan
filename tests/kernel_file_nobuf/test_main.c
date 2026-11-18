/* kernel_file_nobuf -- FILE_NO_INTERMEDIATE_BUFFERING opens read like any other (THPS 2X issue #22, 2026-11-18).
 *
 * Titles open their data files with CreateOptions 0x4C (FILE_NON_DIRECTORY_FILE | FILE_NO_INTERMEDIATE_BUFFERING |
 * FILE_WRITE_THROUGH) and read sizes that are multiples of the Xbox's 512-byte sector. The bridge used to pass the
 * option on as FILE_FLAG_NO_BUFFERING, and Win32 then wants offset, length and buffer address aligned to the HOST
 * volume's sector size: on a drive with 4096-byte sectors every 512 / 1024 / 3584-byte read failed
 * (STATUS_UNSUCCESSFUL), THPS 2X read no shaders / sounds and crashed after the intro movies. Only reads of exactly
 * 4096 bytes got through (the player's log).
 *
 * This host's drive may have 512-byte sectors, so the test reads what fails on ANY drive under that flag: a length
 * that is not a sector multiple, into a buffer at an odd address. Win32 only (the POSIX branch never mapped it). */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *recomp_lookup(ULONG a) { (void)a; abort(); }
void *recomp_lookup_manual(ULONG a) { (void)a; abort(); }

static int failures;
static void check(int ok, const char *what, const char *detail)
{
    printf("  %-64s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

static char T[MAX_PATH], GAME[MAX_PATH], SAVES[MAX_PATH], DATA[MAX_PATH];

static NTSTATUS open_file(const char *xbox, ULONG options, HANDLE *h)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK io;
    name.Length = (USHORT)strlen(xbox);
    name.MaximumLength = name.Length + 1;
    name.Buffer = (PCHAR)xbox;
    memset(&oa, 0, sizeof oa);
    oa.ObjectName = &name;
    *h = NULL;
    /* As THPS 2X opens shaders\geom.xvu: GENERIC_READ|SYNCHRONIZE|FILE_READ_ATTRIBUTES, share read, FILE_OPEN. */
    return xbox_NtCreateFile(h, 0x80100080u, &oa, &io, NULL, 0x80, 1, 1, options);
}

static void read_case(HANDLE h, ULONG len, LONGLONG at, size_t misalign, const uint8_t *want, const char *what)
{
    static uint8_t raw[8192 + 64];
    uint8_t *buf = raw + 16 + misalign;      /* raw is 16-aligned at least; +1 makes it odd */
    XBOX_IO_STATUS_BLOCK io;
    LARGE_INTEGER off;
    NTSTATUS st;
    char d[96];
    memset(&io, 0, sizeof io);
    off.QuadPart = at;
    st = xbox_NtReadFile(h, NULL, NULL, NULL, &io, buf, len, &off);
    snprintf(d, sizeof d, "status 0x%08X, %u bytes", (unsigned)st, (unsigned)io.Information);
    check(st == 0 && io.Information == len && !memcmp(buf, want + at, len), what, d);
}

int main(void)
{
    static uint8_t content[8192];
    char p[MAX_PATH];
    HANDLE h;
    NTSTATUS st;
    FILE *f;
    XboxPathConfig c;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    GetTempPathA(MAX_PATH, T);
    snprintf(p, sizeof p, "%sxr-nobuf-%lu", T, GetCurrentProcessId());
    snprintf(T, sizeof T, "%s", p);
    snprintf(GAME, sizeof GAME, "%s\\game", T);
    snprintf(SAVES, sizeof SAVES, "%s\\saves", T);
    snprintf(DATA, sizeof DATA, "%s\\data", T);
    CreateDirectoryA(T, NULL);
    CreateDirectoryA(GAME, NULL);
    snprintf(p, sizeof p, "%s\\shaders", GAME);
    CreateDirectoryA(p, NULL);
    for (i = 0; i < (int)sizeof content; i++)
        content[i] = (uint8_t)(i * 7 + (i >> 8));
    snprintf(p, sizeof p, "%s\\shaders\\geom.xvu", GAME);
    f = fopen(p, "wb");
    if (!f || fwrite(content, 1, sizeof content, f) != sizeof content) { puts("cannot write the test file"); return 2; }
    fclose(f);

    xbox_path_config_default(&c);
    c.game_dir = GAME; c.save_dir = SAVES; c.data_dir = DATA;
    c.title_id = 0x41560001u;
    xbox_path_init_cfg(&c);

    st = open_file("\\Device\\CdRom0\\shaders\\geom.xvu", 0x4Cu, &h);
    {
        char d[64];
        snprintf(d, sizeof d, "status 0x%08X", (unsigned)st);
        check(st == 0 && h, "open with CreateOptions 0x4C (no intermediate buffering)", d);
    }
    if (st == 0 && h) {
        read_case(h, 512, 0, 0, content, "512 bytes at 0 (THPS 2X geom.xvu)");
        read_case(h, 3584, 512, 0, content, "3584 bytes at 512");
        read_case(h, 100, 0, 1, content, "100 bytes into an odd buffer address");
        read_case(h, 512, 2048, 1, content, "512 bytes at 2048 into an odd buffer address");
        read_case(h, 4096, 4096, 0, content, "4096 bytes at 4096 (worked before too)");
        xbox_NtClose(h);
    }

    snprintf(p, sizeof p, "%s\\shaders\\geom.xvu", GAME); DeleteFileA(p);
    snprintf(p, sizeof p, "%s\\shaders", GAME); RemoveDirectoryA(p);
    RemoveDirectoryA(GAME); RemoveDirectoryA(SAVES); RemoveDirectoryA(DATA); RemoveDirectoryA(T);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
