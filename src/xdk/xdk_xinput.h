/*
 * xdk_xinput.h -- the XDK's controller API (XPP / XInput) at API level.
 *
 * The title's statically linked XAPI drives the console's USB host
 * controllers; the LLE path (src/usb) emulates those so the title's own
 * driver finds a pad. This layer replaces the gamepad entry points instead:
 * a title binds them by address (an override per function in its
 * recomp_manual.c), and they read the host input layer (src/input) directly.
 * No OHCI, no USB frames, no enumeration delay.
 *
 * Every entry point works on the guest registers like a recompiled function:
 * arguments on the guest stack at g_esp + 4, result in g_eax, and it pops its
 * stdcall arguments itself. XGetDevices and XGetDeviceChanges stay the
 * title's own code (they only read the device-type record); call
 * xdk_UpdateDevices() before them so the record is current.
 *
 * Reference design: XboxXBE-Recomp runtime/xdk/xapi.cpp (TGP482, MIT,
 * commit 65e959d4a5fd0c171dc7b17f3f9b44bab00928e6). Rewritten in C; no
 * poller thread (the record is updated when the title asks).
 */
#ifndef XDK_XINPUT_H
#define XDK_XINPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The title's XDEVICE_TYPE_GAMEPAD record (XPP_DEVICE_TYPE: CurrentConnected,
 * ChangeConnected, PreviousConnected), found per title/XDK build. */
void xdk_xinput_bind(uint32_t gamepad_type_va);

/* RECOMP_XINPUT_HLE (default on once bound): 0 runs the title's own XPP code
 * on the emulated USB pads instead. */
int  xdk_xinput_active(void);

void xdk_UpdateDevices(void);          /* refresh the gamepad record          */
void xdk_XInitDevices(void);           /* (count, types)          stdcall 8   */
void xdk_XInputOpen(void);             /* (type, port, slot, pp)  stdcall 16  */
void xdk_XInputClose(void);            /* (handle)                stdcall 4   */
void xdk_XInputGetState(void);         /* (handle, state)         stdcall 8   */
void xdk_XInputSetState(void);         /* (handle, feedback)      stdcall 8   */
void xdk_XInputPoll(void);             /* (handle)                stdcall 4   */
void xdk_XInputGetCapabilities(void);  /* (handle, caps)          stdcall 8   */

#ifdef __cplusplus
}
#endif

#endif /* XDK_XINPUT_H */
