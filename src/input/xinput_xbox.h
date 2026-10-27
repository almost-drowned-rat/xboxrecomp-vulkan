/**
 * Xbox Input Compatibility Layer - Type Definitions
 *
 * Defines the Xbox XPP (Xbox Peripheral Port) input types and
 * translates them to Windows XInput. The Xbox controller API
 * differs from XInput in structure layout and polling model.
 *
 * The game uses 9 XPP entry points with 12 total calls:
 * - Controller state polling
 * - Vibration/force feedback
 * - Device enumeration
 */

#ifndef BURNOUT3_XINPUT_XBOX_H
#define BURNOUT3_XINPUT_XBOX_H

#include <stdint.h>
#include "platform/xbox_winnt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Xbox input types
 * ================================================================ */

/* Xbox controller port count */
#define XBOX_MAX_CONTROLLERS 4

/* Xbox gamepad button flags */
#define XBOX_GAMEPAD_DPAD_UP        0x0001
#define XBOX_GAMEPAD_DPAD_DOWN      0x0002
#define XBOX_GAMEPAD_DPAD_LEFT      0x0004
#define XBOX_GAMEPAD_DPAD_RIGHT     0x0008
#define XBOX_GAMEPAD_START          0x0010
#define XBOX_GAMEPAD_BACK           0x0020
#define XBOX_GAMEPAD_LEFT_THUMB     0x0040
#define XBOX_GAMEPAD_RIGHT_THUMB    0x0080

/* Xbox analog button thresholds */
#define XBOX_ANALOG_BUTTON_THRESHOLD 30

typedef struct XBOX_GAMEPAD {
    WORD  wButtons;             /* Digital button bitmask */
    BYTE  bAnalogButtons[8];    /* A, B, X, Y, Black, White, LTrig, RTrig (0-255) */
    SHORT sThumbLX;             /* Left stick X (-32768 to 32767) */
    SHORT sThumbLY;             /* Left stick Y */
    SHORT sThumbRX;             /* Right stick X */
    SHORT sThumbRY;             /* Right stick Y */
} XBOX_GAMEPAD;

typedef struct XBOX_INPUT_STATE {
    DWORD dwPacketNumber;
    XBOX_GAMEPAD Gamepad;
} XBOX_INPUT_STATE;

typedef struct XBOX_VIBRATION {
    WORD wLeftMotorSpeed;       /* Low-frequency rumble (0-65535) */
    WORD wRightMotorSpeed;      /* High-frequency rumble (0-65535) */
} XBOX_VIBRATION;

typedef struct XBOX_INPUT_CAPABILITIES {
    BYTE Type;
    BYTE SubType;
    WORD Flags;
    XBOX_GAMEPAD Gamepad;
    XBOX_VIBRATION Vibration;
} XBOX_INPUT_CAPABILITIES;

/* Analog button indices */
#define XBOX_BUTTON_A           0
#define XBOX_BUTTON_B           1
#define XBOX_BUTTON_X           2
#define XBOX_BUTTON_Y           3
#define XBOX_BUTTON_BLACK       4
#define XBOX_BUTTON_WHITE       5
#define XBOX_BUTTON_LTRIGGER    6
#define XBOX_BUTTON_RTRIGGER    7

/* ================================================================
 * Public API
 * ================================================================ */

/**
 * Initialize the input system.
 * Maps Xbox controller ports to XInput slots.
 */
void xbox_InputInit(void);

/**
 * Get the state of a controller: host pad, keyboard and pad script merged.
 * Port: 0-3 (Xbox controller ports). The state is filled either way (a USB
 * pad model on the bus reports it); the result says whether the port has a
 * controller (ERROR_SUCCESS) or not (ERROR_DEVICE_NOT_CONNECTED). The packet
 * number moves exactly when the state changes.
 */
DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState);

/**
 * Ports with a controller, bit n = port n: host pads, the keyboard's port
 * (RECOMP_KEYBOARD_PORT, default 0 merged), RECOMP_PADS and ports the pad
 * script drives. Re-enumerates host pads at most every 500 ms (hot-plug).
 */
unsigned xbox_InputConnectedMask(void);

/* ---- pad script (xinput_script.c) ------------------------------------
 * RECOMP_PAD_SCRIPT=<steps>|@file, RECOMP_PAD_LIVE=<file>, RECOMP_PAD_PRESS;
 * syntax in xinput_script.c. Applied by xbox_InputGetState. */
void xbox_InputScriptApply(int port, XBOX_GAMEPAD *g);
unsigned xbox_InputScriptPorts(void);
/* A "key=<HID usage ID>" step holds that key now (read with the keyboard,
 * so it goes through the keyboard's binding). */
int xbox_InputScriptKeyDown(int scancode);

/**
 * Set controller vibration.
 */
DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration);

/**
 * Check if a controller is connected.
 */
BOOL xbox_InputIsConnected(DWORD dwPort);

/**
 * Get controller capabilities.
 */
DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps);

/* ================================================================
 * Keyboard as controller 0
 *
 * Keys are USB HID usage IDs (keyboard page 0x07) -- the same numbers as
 * SDL_Scancode, and independent of the host OS and of the keyboard layout:
 * 4 = A ... 29 = Z, 30 = 1 ... 39 = 0, 40 Enter, 42 Backspace, 44 Space,
 * 79..82 arrows right/left/down/up, 89..97 keypad 1..9, 224/225 left
 * Ctrl/Shift.
 *
 * The window that has the focus feeds key edges in (nv2a_vk:
 * nv2a_vk_set_key_handler(xbox_InputKeyEvent)); the D3D11 framebuffer
 * window is read directly. The keyboard is merged on top of host pad 0 and
 * only answers while RECOMP_KEYBOARD=1 or xbox_InputEnableKeyboard(1).
 * ================================================================ */

/* What a key drives. Sticks: a key holds that half-axis at full throw. */
enum {
    XBOX_KEY_DPAD_UP, XBOX_KEY_DPAD_DOWN, XBOX_KEY_DPAD_LEFT, XBOX_KEY_DPAD_RIGHT,
    XBOX_KEY_START, XBOX_KEY_BACK, XBOX_KEY_LEFT_THUMB, XBOX_KEY_RIGHT_THUMB,
    XBOX_KEY_A, XBOX_KEY_B, XBOX_KEY_X, XBOX_KEY_Y,
    XBOX_KEY_BLACK, XBOX_KEY_WHITE, XBOX_KEY_LTRIGGER, XBOX_KEY_RTRIGGER,
    XBOX_KEY_LX_NEG, XBOX_KEY_LX_POS, XBOX_KEY_LY_NEG, XBOX_KEY_LY_POS,
    XBOX_KEY_RX_NEG, XBOX_KEY_RX_POS, XBOX_KEY_RY_NEG, XBOX_KEY_RY_POS,
    XBOX_KEY_CONTROL_COUNT
};

typedef struct {
    uint16_t scancode;   /* HID usage ID, < 512 */
    uint8_t  control;    /* XBOX_KEY_* */
} XBOX_KEY_BINDING;

#define XBOX_KEYMAP_MAX 64

/* Key down/up edge from the host window. Thread-safe against the readers.
 * A negative scancode releases every key (the window lost the focus). */
void xbox_InputKeyEvent(int scancode, int down);

/* Release every key (the window lost the focus: no key-up will arrive). */
void xbox_InputKeyReleaseAll(void);

/* Replace the keymap (copied; at most XBOX_KEYMAP_MAX entries). Several keys
 * may drive one control. Without a call the toolkit default is used:
 * arrows d-pad, Z X A S = A B X Y, Q E white/black, 1 3 triggers,
 * Enter/Backspace Start/Back, keypad 8/2/4/6 left stick, I/K/J/L right
 * stick, Shift/Ctrl thumb clicks. */
void xbox_InputSetKeymap(const XBOX_KEY_BINDING *map, int count);

/* Turn the keyboard on or off, overriding RECOMP_KEYBOARD. */
void xbox_InputEnableKeyboard(int on);

#ifdef __cplusplus
}
#endif

#endif /* BURNOUT3_XINPUT_XBOX_H */
