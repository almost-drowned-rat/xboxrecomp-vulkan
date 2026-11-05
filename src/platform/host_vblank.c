/*
 * host_vblank.c - wait for the display's vertical blank (see host_vblank.h)
 *
 * Windows: D3DKMTOpenAdapterFromHdc on the window's monitor, then
 * D3DKMTWaitForVerticalBlankEvent on its VidPn source -- both exported by
 * gdi32 (kernel thunks; no D3D runtime, no DXGI), loaded at run time so a
 * system without them simply has no display pacing. The structs are declared
 * here (d3dkmthk.h is a WDK header).
 */

#include "host_vblank.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>

typedef LONG kmt_status;
typedef UINT kmt_handle;

typedef struct {
    HDC        hDc;
    kmt_handle hAdapter;
    LUID       AdapterLuid;
    UINT       VidPnSourceId;
} KmtOpenAdapterFromHdc;

typedef struct {
    kmt_handle hAdapter;
    kmt_handle hDevice;
    UINT       VidPnSourceId;
} KmtWaitForVerticalBlankEvent;

typedef struct {
    kmt_handle hAdapter;
} KmtCloseAdapter;

typedef kmt_status (APIENTRY *PfnOpen)(KmtOpenAdapterFromHdc *);
typedef kmt_status (APIENTRY *PfnWait)(const KmtWaitForVerticalBlankEvent *);
typedef kmt_status (APIENTRY *PfnClose)(const KmtCloseAdapter *);

static PfnWait  s_wait;
static PfnClose s_close;
static KmtWaitForVerticalBlankEvent s_vb;
static int s_open;

int host_vblank_open(void *native_window)
{
    HMODULE gdi;
    PfnOpen open_fn;
    HMONITOR mon;
    MONITORINFOEXW mi;
    KmtOpenAdapterFromHdc oa;
    kmt_status st;

    host_vblank_close();
    gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi)
        gdi = LoadLibraryW(L"gdi32.dll");
    if (!gdi)
        return 0;
    open_fn = (PfnOpen)(void *)GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc");
    s_wait  = (PfnWait)(void *)GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent");
    s_close = (PfnClose)(void *)GetProcAddress(gdi, "D3DKMTCloseAdapter");
    if (!open_fn || !s_wait) {
        fprintf(stderr, "  display vblank: gdi32 has no D3DKMT vblank wait\n");
        return 0;
    }
    mon = MonitorFromWindow((HWND)native_window, MONITOR_DEFAULTTOPRIMARY);
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!mon || !GetMonitorInfoW(mon, (MONITORINFO *)&mi))
        return 0;
    memset(&oa, 0, sizeof oa);
    oa.hDc = CreateDCW(NULL, mi.szDevice, NULL, NULL);
    if (!oa.hDc)
        return 0;
    st = open_fn(&oa);
    DeleteDC(oa.hDc);
    if (st != 0) {
        fprintf(stderr, "  display vblank: D3DKMTOpenAdapterFromHdc failed (0x%08lX)\n", (unsigned long)st);
        return 0;
    }
    s_vb.hAdapter = oa.hAdapter;
    s_vb.hDevice = 0;
    s_vb.VidPnSourceId = oa.VidPnSourceId;
    s_open = 1;
    return 1;
}

int host_vblank_wait(void)
{
    return s_open && s_wait(&s_vb) == 0;
}

void host_vblank_close(void)
{
    if (s_open && s_close) {
        KmtCloseAdapter ca;
        ca.hAdapter = s_vb.hAdapter;
        s_close(&ca);
    }
    s_open = 0;
}

#else

int host_vblank_open(void *native_window)
{
    (void)native_window;
    return 0;
}

int host_vblank_wait(void)
{
    return 0;
}

void host_vblank_close(void)
{
}

#endif
