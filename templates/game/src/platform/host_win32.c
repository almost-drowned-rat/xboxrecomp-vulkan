/* host_win32.c - @TITLE@ host platform layer, Windows implementation.
 *
 * Crash handler origin: sp00nznet/xboxrecomp templates/new-game/src/main.c
 * (commit 193e299, MIT) -- veh_handler / print_guest_context, see
 * docs/sources.md. Moved here so main.c stays free of windows.h.
 */
#include <windows.h>
#include <shellapi.h>
#include <dbghelp.h>
#include <io.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime headers this file uses -- not the umbrella xbox/xboxrecomp.h,
 * which also pulls in the D3D8 and NV2A-state headers of libraries a
 * Vulkan-only build does not have. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "apu.h"

#include "host.h"
#include "recomp_icall_feedback.h"   /* no-op unless RECOMP_ICALL_FEEDBACK */

/* Guest register file (thread-local, defined in xbox_memory_layout.c). */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;

/* Emulated APU MMIO trap (see templates/new-game/src/main.c). */
typedef struct MCPXAPUState MCPXAPUState;
extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                 uint32_t fault_xbox_va, int is_write);
#define APU_TRAP_BASE 0xFE800000u
#define APU_TRAP_END  0xFE880000u

/* USB host controllers (toolkit src/usb/ohci.c, RECOMP_USB): the title's own
 * XPP driver finds its gamepads through their trapped registers. */
extern int xbox_OhciOwnsAddress(uint32_t xbox_va);
extern int xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va);

static void host_log_drain(void);

/* ── crash handler ───────────────────────────────────────────────── */

/* Name the guest function a fault happened in (the generated sub_XXXXXXXX
 * symbol, via the .pdb) and scan the guest stack for return addresses:
 * FPO frames, so it is a scan, not a walk, and over-reports. */
static void print_guest_context(void *rip)
{
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n", sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        if (!shown) {
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;
            uint32_t k;
            fprintf(stderr, "    (no return addresses in range)\n  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* ── write watchpoint (@PREFIX@_WATCH_WRITE=<hex va>[,...]) ──────────────
 *
 * Debugging aid: who writes this guest byte? Arms debug register DR0 (1 byte,
 * on write) on every thread of the process, re-arming once a second for
 * threads started later, and logs each hit with the host function (from the
 * .pdb), the thread, the byte's new value and the guest return address on
 * the hitting thread's guest stack. Found the second writer of XPP's pad
 * report buffer (M3 stage 2). Off unless set. */
#include <tlhelp32.h>

static uintptr_t s_watch_host[4];
static int s_watch_n;
static volatile LONG s_watch_hits;

static void watch_arm_all(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();

    if (snap == INVALID_HANDLE_VALUE)
        return;
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        HANDLE th;
        CONTEXT c;
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
            continue;
        th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                        FALSE, te.th32ThreadID);
        if (!th)
            continue;
        if (SuspendThread(th) != (DWORD)-1) {
            memset(&c, 0, sizeof c);
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &c) && (c.Dr0 != s_watch_host[0] || !(c.Dr7 & 1))) {
                DWORD64 *dr[4] = { &c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3 };
                int i;
                for (i = 0; i < s_watch_n; i++) {
                    *dr[i] = s_watch_host[i];
                    /* Ln enable, RWn = 01 (write), LENn = 00 (1 byte) */
                    c.Dr7 = (c.Dr7 & ~((3ull << (i * 2)) | (0xFull << (16 + i * 4))))
                          | (1ull << (i * 2)) | (1ull << (16 + i * 4));
                }
                SetThreadContext(th, &c);
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
}

static DWORD WINAPI watch_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        watch_arm_all();
        Sleep(1000);
    }
    return 0;
}

static LONG CALLBACK watch_handler(PEXCEPTION_POINTERS ep)
{
    int which;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 0xF))
        return EXCEPTION_CONTINUE_SEARCH;
    for (which = 0; which < 3 && !(ep->ContextRecord->Dr6 & (1ull << which)); which++)
        ;
    ep->ContextRecord->Dr6 = 0;
    {
        /* The legitimate writer hits hundreds of times a second: log each
         * distinct writer (RIP) a few times only. */
        static uintptr_t rips[32];
        static int counts[32], nrips;
        int k;
        /* Library routines (memcpy) are shared by every writer: the return
         * address on the host stack tells their callers apart. */
        uintptr_t key = ep->ContextRecord->Rip ^ ((uintptr_t)GetCurrentThreadId() << 48)
                      ^ ((uintptr_t)which << 62) ^ (*(uintptr_t *)ep->ContextRecord->Rsp << 7);
        for (k = 0; k < nrips && rips[k] != key; k++)
            ;
        if (k == nrips && nrips < 32)
            rips[nrips++] = key;
        static int cap = -1;
        if (cap < 0) {
            const char *e = getenv("@PREFIX@_WATCH_MAX");   /* hits logged per writer */
            cap = e ? atoi(e) : 4;
        }
        if (k < 32 && ++counts[k] > cap)
            return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (InterlockedIncrement(&s_watch_hits) <= 20000) {
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        DWORD64 disp = 0;
        uint32_t ret = 0;
        memset(buf, 0, sizeof buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        if (g_xbox_mem_offset && g_esp)
            ret = *(const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        char caller[128] = "?";
        {
            char cbuf[sizeof(SYMBOL_INFO) + 128];
            SYMBOL_INFO *cs = (SYMBOL_INFO *)cbuf;
            DWORD64 cdisp = 0;
            memset(cbuf, 0, sizeof cbuf);
            cs->SizeOfStruct = sizeof(SYMBOL_INFO);
            cs->MaxNameLen = 127;
            if (SymFromAddr(GetCurrentProcess(), *(DWORD64 *)ep->ContextRecord->Rsp, &cdisp, cs))
                snprintf(caller, sizeof caller, "%s+0x%llX", cs->Name, (unsigned long long)cdisp);
        }
        fprintf(stderr, "[WATCH] #%d (guest 0x%08X) write -> %02X by %s+0x%llX ([rsp] %s) thread %lu guest esp 0x%08X [esp] 0x%08X\n",
                which, (uint32_t)(s_watch_host[which] - (uintptr_t)g_xbox_mem_offset),
                *(volatile uint8_t *)s_watch_host[which],
                SymFromAddr(GetCurrentProcess(), ep->ContextRecord->Rip, &disp, sym) ? sym->Name : "?",
                (unsigned long long)disp, caller, GetCurrentThreadId(), g_esp, ret);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void watch_start(void)
{
    const char *e = getenv("@PREFIX@_WATCH_WRITE");
    if (!e || !*e || !g_xbox_mem_offset)
        return;
    /* Up to four addresses, comma separated (DR0..DR3). */
    while (*e && s_watch_n < 4) {
        char *end;
        s_watch_host[s_watch_n++] = (uintptr_t)g_xbox_mem_offset + (uint32_t)strtoul(e, &end, 16);
        e = (*end == ',') ? end + 1 : end;
    }
    e = getenv("@PREFIX@_WATCH_WRITE");
    AddVectoredExceptionHandler(1, watch_handler);
    CloseHandle(CreateThread(NULL, 0, watch_thread, NULL, 0, NULL));
    fprintf(stderr, "[WATCH] write watchpoint on guest 0x%s\n", e);
}

void host_debug_start(void)
{
    watch_start();
}

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    if (code == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];
        uint32_t xbox_va = (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);

        if (xbox_va >= APU_TRAP_BASE && xbox_va < APU_TRAP_END
                && apu_hook_handle_mmio(ep->ContextRecord, fault_addr, xbox_va,
                                        ep->ExceptionRecord->ExceptionInformation[0] ? 1 : 0))
            return EXCEPTION_CONTINUE_EXECUTION;
        if (xbox_OhciOwnsAddress(xbox_va) && xbox_OhciHandleMmio(ep->ContextRecord, xbox_va))
            return EXCEPTION_CONTINUE_EXECUTION;

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
                (unsigned long long)ep->ContextRecord->Rip, (unsigned long long)fault_addr,
                ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n", xbox_va);
    } else if (code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO
               || code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_PRIV_INSTRUCTION) {
        fprintf(stderr, "[CRASH] exception 0x%08lX at RIP=0x%llX\n",
                (unsigned long)code, (unsigned long long)ep->ContextRecord->Rip);
    } else {
        return EXCEPTION_CONTINUE_SEARCH;   /* C++/debug/SEH traffic: not ours */
    }

    fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
    fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n", g_ebx, g_esi, g_edi);
    print_guest_context((void *)ep->ContextRecord->Rip);
    RECOMP_ICALL_FEEDBACK_DUMP();   /* a title that dies mid-boot is the one whose targets we want */
    host_log_drain();               /* the process is about to die */
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── time limit (test runs, CLAUDE.md: --max-seconds) ───────────── */

static void host_terminate(int code)
{
    RECOMP_ICALL_FEEDBACK_DUMP();   /* TerminateProcess skips atexit */
    host_log_drain();
    TerminateProcess(GetCurrentProcess(), (UINT)code);
}

static DWORD WINAPI time_limit_thread(LPVOID arg)
{
    double secs = *(double *)arg;
    Sleep((DWORD)(secs * 1000.0));
    fprintf(stderr, "[HOST] --max-seconds %.0f reached, exiting\n", secs);
    host_terminate(0);
    return 0;
}

/* The window was closed. The guest threads never return on their own, so
 * this ends the process the way the time limit does. */
void host_quit(void)
{
    fprintf(stderr, "[HOST] window closed, exiting\n");
    host_terminate(0);
}

/* ── init ────────────────────────────────────────────────────────── */

static void join(char *out, const char *dir, const char *name)
{
    snprintf(out, HOST_PATH_MAX, "%s\\%s", dir, name);
}

/* Paths are UTF-8 inside the host layer and wide at every Win32 call, so an
 * install folder with spaces, umlauts or any other script works (the ANSI
 * APIs only know the locale's code page). */
static void widen(const char *s, WCHAR *out, int n)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n))
        out[0] = 0;
}

static void narrow(const WCHAR *s, char *out, int n)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL))
        out[0] = 0;
}

static FILE *fopen_utf8(const char *path, const WCHAR *mode)
{
    WCHAR w[HOST_PATH_MAX];
    widen(path, w, HOST_PATH_MAX);
    return _wfopen(w, mode);
}

/* ── log file with repeat collapsing ─────────────────────────────
 *
 * The runtime prints straight to stdout/stderr from many places, and some of
 * it repeats by the thousand ([READ] per 4 KB of a movie, the same DPC every
 * tick): 1 MB of log per two minutes, most of it one line in different
 * numbers. Both streams go through a pipe instead, and a reader thread writes
 * the log: a run of lines that differ only in their numbers keeps its first
 * HOST_LOG_KEEP lines and is then summarised as one line with the count and
 * the last line. Crash and watchdog reports are written verbatim -- their
 * register and stack lines all look alike and every one matters. */

#define HOST_LOG_KEEP      3
#define HOST_LOG_LINE_MAX  1024
#define HOST_LOG_VERBATIM  400   /* lines after a [CRASH]/[WATCHDOG] header */

static FILE  *s_log;
static int    s_log_pipe_w = -1;
static int    s_log_pipe_r = -1;
static HANDLE s_log_thread;
static char   s_prev_key[HOST_LOG_LINE_MAX];
static char   s_last_line[HOST_LOG_LINE_MAX];
static unsigned long s_repeats;   /* similar lines after the prev key's first */
static int    s_verbatim;

/* The line with every run of hex/decimal digits replaced by '#'. */
static void log_key(const char *line, char *key, size_t n)
{
    size_t k = 0;
    while (*line && k + 1 < n) {
        if (isxdigit((unsigned char)*line) && (isdigit((unsigned char)*line)
                || (line[1] && isxdigit((unsigned char)line[1])))) {
            while (*line && (isxdigit((unsigned char)*line) || *line == 'x'))
                line++;
            key[k++] = '#';
        } else {
            key[k++] = *line++;
        }
    }
    key[k] = '\0';
}

static void log_flush_run(void)
{
    if (s_repeats > HOST_LOG_KEEP - 1) {
        unsigned long hidden = s_repeats - (HOST_LOG_KEEP - 1);
        fprintf(s_log, "  [log] ... %lu more similar line(s), last: %s",
                hidden, s_last_line);
    }
    s_repeats = 0;
}

static void log_line(const char *line)
{
    char key[HOST_LOG_LINE_MAX];

    if (!strncmp(line, "[CRASH]", 7) || !strncmp(line, "[WATCHDOG]", 10)) {
        log_flush_run();
        s_prev_key[0] = '\0';
        s_verbatim = HOST_LOG_VERBATIM;
    }
    if (s_verbatim > 0) {
        s_verbatim--;
        fputs(line, s_log);
        return;
    }
    /* @PREFIX@_LOG_RAW=1: no collapsing at all (register traces, where every
     * line looks like the last). A frame trace (RECOMP_FRAME_TRACE) is one
     * line per draw, all alike: never collapsed. */
    {
        static int raw = -1;
        if (raw < 0) {
            const char *e = getenv("@PREFIX@_LOG_RAW");
            raw = e && *e && *e != '0';
        }
        if (raw) {
            fputs(line, s_log);
            return;
        }
    }
    if (!strncmp(line, "[FTRACE]", 8)) {
        log_flush_run();
        s_prev_key[0] = '\0';
        fputs(line, s_log);
        return;
    }
    log_key(line, key, sizeof(key));
    if (s_prev_key[0] && !strcmp(key, s_prev_key)) {
        s_repeats++;
        if (s_repeats < HOST_LOG_KEEP)
            fputs(line, s_log);
        else
            snprintf(s_last_line, sizeof(s_last_line), "%s", line);
        return;
    }
    log_flush_run();
    snprintf(s_prev_key, sizeof(s_prev_key), "%s", key);
    fputs(line, s_log);
}

static DWORD WINAPI log_thread(LPVOID unused)
{
    char buf[4096], line[HOST_LOG_LINE_MAX];
    size_t len = 0;
    int n, i;

    (void)unused;
    while ((n = _read(s_log_pipe_r, buf, sizeof(buf))) > 0) {
        for (i = 0; i < n; i++) {
            if (len + 2 < sizeof(line))
                line[len++] = buf[i];
            if (buf[i] == '\n' || len + 2 >= sizeof(line)) {
                if (line[len - 1] != '\n')
                    line[len++] = '\n';
                line[len] = '\0';
                log_line(line);
                len = 0;
            }
        }
        fflush(s_log);
    }
    if (len) {
        line[len] = '\0';
        log_line(line);
    }
    log_flush_run();
    fflush(s_log);
    return 0;
}

/* Close the write side and wait for the reader to empty the pipe: the run
 * is over (time limit, crash, exit) and nothing printed so far may be lost.
 * Output written after this is dropped. */
static void host_log_drain(void)
{
    if (s_log_pipe_w < 0)
        return;
    fflush(stdout);
    fflush(stderr);
    _close(_fileno(stdout));
    _close(_fileno(stderr));
    _close(s_log_pipe_w);
    s_log_pipe_w = -1;
    WaitForSingleObject(s_log_thread, 5000);
}

static void open_log(const char *data_dir)
{
    char log[HOST_PATH_MAX], prev[HOST_PATH_MAX];
    WCHAR wlog[HOST_PATH_MAX], wprev[HOST_PATH_MAX];
    int fds[2];

    join(log, data_dir, "@NAME@.log");
    join(prev, data_dir, "@NAME@.previous.log");
    widen(log, wlog, HOST_PATH_MAX);
    widen(prev, wprev, HOST_PATH_MAX);
    MoveFileExW(wlog, wprev, MOVEFILE_REPLACE_EXISTING);
    s_log = _wfopen(wlog, L"w");
    if (!s_log)
        return;
    /* A GUI-subsystem exe (the player's build) started without inherited
     * handles - double-clicked, not through the launcher - has no descriptor
     * behind stdout / stderr (_fileno -2): the _dup2 below would reach nothing
     * and the log stay empty (THPS 2X, 2026-10-29). Give both one first. */
    if (_fileno(stdout) < 0)
        freopen("NUL", "w", stdout);
    if (_fileno(stderr) < 0)
        freopen("NUL", "w", stderr);
    if (_pipe(fds, 1 << 16, _O_BINARY) != 0) {
        /* No filter then, but still a log. */
        fclose(s_log);
        if (_wfreopen(wlog, L"w", stdout))
            _dup2(_fileno(stdout), _fileno(stderr));
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        return;
    }
    s_log_pipe_r = fds[0];
    s_log_pipe_w = fds[1];
    _dup2(s_log_pipe_w, _fileno(stdout));
    _dup2(s_log_pipe_w, _fileno(stderr));
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    s_log_thread = CreateThread(NULL, 0, log_thread, NULL, 0, NULL);
    atexit(host_log_drain);
}

/* --input-script / --input-live: the toolkit's pad script (src/usb/
 * usb_gamepad.c, RECOMP_PAD_SCRIPT=@file / RECOMP_PAD_LIVE=file). The path is
 * made absolute here, before the working directory moves to data/. The
 * command line wins over the environment. */
static void set_input_file(const WCHAR *var, const WCHAR *prefix, const char *path)
{
    /* Wide in the environment as well: the toolkit reads these with
     * _wgetenv, so a path outside the locale's code page survives. */
    WCHAR wpath[HOST_PATH_MAX], full[HOST_PATH_MAX], val[HOST_PATH_MAX + 2];
    widen(path, wpath, HOST_PATH_MAX);
    if (!GetFullPathNameW(wpath, HOST_PATH_MAX, full, NULL))
        wcscpy_s(full, HOST_PATH_MAX, wpath);
    swprintf_s(val, HOST_PATH_MAX + 2, L"%s%s", prefix, full);
    _wputenv_s(var, val);
}

int host_init(int argc, char **argv, HostOptions *opt)
{
    static double s_limit;
    int console = 0, i, wargc = 0;
    char *slash;
    WCHAR wexe[HOST_PATH_MAX], wdir[HOST_PATH_MAX];
    WCHAR **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    static char argbuf[64][HOST_PATH_MAX];

    (void)argv;
    memset(opt, 0, sizeof(*opt));
    GetModuleFileNameW(NULL, wexe, HOST_PATH_MAX);
    narrow(wexe, opt->exe_dir, HOST_PATH_MAX);
    /* The command line as UTF-8 (argv is in the ANSI code page). */
    if (wargv) {
        argc = wargc < 64 ? wargc : 64;
        for (i = 0; i < argc; i++)
            narrow(wargv[i], argbuf[i], HOST_PATH_MAX);
        LocalFree(wargv);
    } else {
        argc = 1;
    }
    slash = strrchr(opt->exe_dir, '\\');
    if (slash) *slash = '\0';
    join(opt->game_dir, opt->exe_dir, "game");
    join(opt->save_dir, opt->exe_dir, "saves");
    join(opt->data_dir, opt->exe_dir, "data");

    for (i = 1; i < argc; i++) {
        const char *a = argbuf[i], *v = (i + 1 < argc) ? argbuf[i + 1] : NULL;
        if (!strcmp(a, "--console")) { console = 1; continue; }
        if (!v) break;
        if (!strcmp(a, "--max-seconds")) opt->max_seconds = atof(v);
        else if (!strcmp(a, "--game-dir")) snprintf(opt->game_dir, HOST_PATH_MAX, "%s", v);
        else if (!strcmp(a, "--save-dir")) snprintf(opt->save_dir, HOST_PATH_MAX, "%s", v);
        else if (!strcmp(a, "--data-dir")) snprintf(opt->data_dir, HOST_PATH_MAX, "%s", v);
        else if (!strcmp(a, "--input-script")) set_input_file(L"RECOMP_PAD_SCRIPT", L"@", v);
        else if (!strcmp(a, "--input-live")) set_input_file(L"RECOMP_PAD_LIVE", L"", v);
        else continue;
        i++;
    }

    widen(opt->save_dir, wdir, HOST_PATH_MAX);
    CreateDirectoryW(wdir, NULL);
    widen(opt->data_dir, wdir, HOST_PATH_MAX);
    CreateDirectoryW(wdir, NULL);
    if (!console) open_log(opt->data_dir);
    else { setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0); }
    /* Every game path is absolute; the working directory only catches what
     * the runtime writes relative to it (xbox_kernel.log, icall_targets.dump),
     * and that belongs in data/, not next to the exe. */
    SetCurrentDirectoryW(wdir);
    RECOMP_ICALL_FEEDBACK_INIT();

    /* Symbols up front, not inside the handler (it allocates). */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    widen(opt->exe_dir, wdir, HOST_PATH_MAX);
    SymInitializeW(GetCurrentProcess(), wdir, TRUE);          /* .pdb beside the exe */
    /* Last in the chain: the runtime's own handlers (MMIO traps and the like)
     * see a fault first, and only one nobody handled is reported -- the
     * report ends the log. */
    AddVectoredExceptionHandler(0, veh_handler);

    if (opt->max_seconds > 0) {
        s_limit = opt->max_seconds;
        CloseHandle(CreateThread(NULL, 0, time_limit_thread, &s_limit, 0, NULL));
    }
    return 0;
}

void host_setenv_default(const char *name, const char *value)
{
    if (!getenv(name))
        _putenv_s(name, value);
}

void *host_read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen_utf8(path, L"rb");
    long size;
    void *data;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = size > 0 ? malloc((size_t)size) : NULL;
    if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data) *out_size = (size_t)size;
    return data;
}

void host_fatal(const char *msg)
{
    fprintf(stderr, "[FATAL] %s\n", msg);
    fflush(stderr);
    {
        WCHAR wmsg[1024];
        widen(msg, wmsg, 1024);
        MessageBoxW(NULL, wmsg, L"@TITLE@ Recomp", MB_ICONERROR);
    }
}
