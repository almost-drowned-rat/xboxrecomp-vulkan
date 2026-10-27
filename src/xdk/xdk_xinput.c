/*
 * xdk_xinput.c -- XPP gamepad entry points at API level (see xdk_xinput.h).
 *
 * Reference design: XboxXBE-Recomp runtime/xdk/xapi.cpp (TGP482, MIT,
 * commit 65e959d4a5fd0c171dc7b17f3f9b44bab00928e6): the device-type record
 * kept current from the host, handles per open port, packet number from the
 * state, SetState completing at once. Rewritten in C for this runtime.
 *
 * Layouts (XDK 3911..5849, the same in every build seen so far):
 *   XPP_DEVICE_TYPE      +0 CurrentConnected, +4 ChangeConnected,
 *                        +8 PreviousConnected (bit n = port n)
 *   XDEVICE_PREALLOC_TYPE { DeviceType, dwPreallocCount } (8 bytes)
 *   XINPUT_STATE         +0 dwPacketNumber, +4 wButtons, +6 bAnalogButtons[8],
 *                        +14 sThumbLX, LY, RX, RY
 *   XINPUT_FEEDBACK      +0 dwStatus, +4 hEvent, +0x42/+0x44 left/right motor
 *   XINPUT_CAPABILITIES  packed: +0 SubType, +1 Reserved (2), +3 In.Gamepad
 *                        (wButtons, analog[8], 4 thumbs), +0x15 Out.Rumble
 */
#include "xdk_xinput.h"

#include "kernel.h"
#include "frame_time.h"
#include "xbox_memory_layout.h"
#include "xinput_xbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;

static uint8_t *gp(uint32_t va) { return (uint8_t *)((uintptr_t)va + g_xbox_mem_offset); }
static uint32_t r32(uint32_t va)
{
    const uint8_t *p = gp(va);
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t r16(uint32_t va) { const uint8_t *p = gp(va); return (uint16_t)(p[0] | p[1] << 8); }
static void w32(uint32_t va, uint32_t v)
{
    uint8_t *p = gp(va);
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void w16(uint32_t va, uint16_t v) { uint8_t *p = gp(va); p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

#define ARG(n) r32(g_esp + 4u + 4u * (n))
/* stdcall return: result in eax, pop the return address and the arguments. */
#define RETURN(v, nargs) do { g_eax = (uint32_t)(v); g_esp += 4u + 4u * (nargs); return; } while (0)

#define ERR_INVALID_HANDLE  6u
#define ERR_IO_PENDING      997u
#define ERR_NOT_CONNECTED   1167u

/* A handle is a token, never guest memory: every function that takes one is
 * replaced here, so no title code dereferences it. */
#define HANDLE_TAG 0x58490000u            /* "XI" */
#define HANDLE_PORT(h) ((h) - HANDLE_TAG - 1u)

static uint32_t s_gamepad_type;
static unsigned s_open;                   /* bit n: port n open */
static int      s_trace = -1;

static int trace(void)
{
    if (s_trace < 0) {
        const char *e = getenv("RECOMP_PAD_TRACE");
        s_trace = e && *e && *e != '0';
    }
    return s_trace;
}

void xdk_xinput_bind(uint32_t gamepad_type_va)
{
    s_gamepad_type = gamepad_type_va;
    xbox_InputInit();
}

int xdk_xinput_active(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_XINPUT_HLE");
        on = s_gamepad_type && !(e && *e == '0');
    }
    return on;
}

static int handle_port(uint32_t h)
{
    uint32_t port = HANDLE_PORT(h);
    return (h > HANDLE_TAG && port < XBOX_MAX_CONTROLLERS && (s_open >> port) & 1u) ? (int)port : -1;
}

void xdk_UpdateDevices(void)
{
    uint32_t cur, old;

    if (!s_gamepad_type)
        return;
    cur = xbox_InputConnectedMask() & 0xFu;
    old = r32(s_gamepad_type + 0);
    if (cur != old) {
        w32(s_gamepad_type + 4, r32(s_gamepad_type + 4) | (cur ^ old));
        w32(s_gamepad_type + 0, cur);
        fprintf(stderr, "  XINPUT: controller ports now %X\n", cur);
        fflush(stderr);
    }
}

void xdk_XInitDevices(void)
{
    uint32_t n = ARG(0), types = ARG(1), i;

    /* Every listed type starts empty; only gamepads ever arrive (no memory
     * units, no voice, no keyboards on the bus). */
    for (i = 0; types && i < n && i < 16; i++) {
        uint32_t t = r32(types + 8u * i);
        if (t) {
            w32(t + 0, 0);
            w32(t + 4, 0);
            w32(t + 8, 0);
        }
    }
    s_open = 0;
    xdk_UpdateDevices();
    RETURN(0, 2);
}

void xdk_XInputOpen(void)
{
    uint32_t type = ARG(0), port = ARG(1);
    uint32_t h = 0;

    if (type == s_gamepad_type && port < XBOX_MAX_CONTROLLERS
        && (xbox_InputConnectedMask() >> port) & 1u) {
        s_open |= 1u << port;
        h = HANDLE_TAG + 1u + port;
    }
    if (trace())
        fprintf(stderr, "[PADTRACE] hle XInputOpen(type 0x%08X, port %u) -> 0x%08X\n", type, port, h);
    RETURN(h, 4);
}

void xdk_XInputClose(void)
{
    int port = handle_port(ARG(0));
    if (port >= 0)
        s_open &= ~(1u << port);
    RETURN(0, 1);
}

void xdk_XInputGetState(void)
{
    uint32_t s = ARG(1);
    int port = handle_port(ARG(0));
    XBOX_INPUT_STATE st;
    const XBOX_GAMEPAD *g = &st.Gamepad;
    int i;

    if (port < 0)
        RETURN(ERR_INVALID_HANDLE, 2);
    if (xbox_InputGetState((DWORD)port, &st) != 0)
        RETURN(ERR_NOT_CONNECTED, 2);
    if (port == 0)
        xbox_ft_input();
    w32(s + 0, st.dwPacketNumber);
    w16(s + 4, g->wButtons);
    for (i = 0; i < 8; i++)
        gp(s + 6)[i] = g->bAnalogButtons[i];
    w16(s + 14, (uint16_t)g->sThumbLX);
    w16(s + 16, (uint16_t)g->sThumbLY);
    w16(s + 18, (uint16_t)g->sThumbRX);
    w16(s + 20, (uint16_t)g->sThumbRY);
    RETURN(0, 2);
}

void xdk_XInputSetState(void)
{
    uint32_t fb = ARG(1), ev;
    int port = handle_port(ARG(0));
    XBOX_VIBRATION v;

    if (port < 0)
        RETURN(ERR_INVALID_HANDLE, 2);
    v.wLeftMotorSpeed = r16(fb + 0x42);
    v.wRightMotorSpeed = r16(fb + 0x44);
    xbox_InputSetState((DWORD)port, &v);
    /* Done at once: the title sees ERROR_IO_PENDING, then a completed status
     * (and its event set) the next time it looks. */
    w32(fb + 0, 0);
    ev = r32(fb + 4);
    if (ev)
        xbox_GuestSetEvent(ev);
    RETURN(ERR_IO_PENDING, 2);
}

void xdk_XInputPoll(void)
{
    RETURN(handle_port(ARG(0)) >= 0 ? 0 : ERR_INVALID_HANDLE, 1);
}

void xdk_XInputGetCapabilities(void)
{
    uint32_t c = ARG(1);
    int port = handle_port(ARG(0));

    if (port < 0)
        RETURN(ERR_INVALID_HANDLE, 2);
    if (!xbox_InputIsConnected((DWORD)port))
        RETURN(ERR_NOT_CONNECTED, 2);
    /* The standard Xbox gamepad: every control at full resolution, both
     * motors. */
    memset(gp(c), 0, 0x19);
    gp(c)[0] = 1;                       /* XINPUT_DEVSUBTYPE_GC_GAMEPAD */
    w16(c + 3, 0x00FF);                 /* wButtons */
    memset(gp(c + 5), 0xFF, 8);         /* analog buttons */
    memset(gp(c + 13), 0xFF, 8);        /* thumbs */
    w16(c + 0x15, 0xFFFF);
    w16(c + 0x17, 0xFFFF);
    RETURN(0, 2);
}
