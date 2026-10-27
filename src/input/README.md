# xbox_input — host controllers as Xbox gamepads

One input layer for every host and both controller paths of a title:

```
SDL3 gamepads (hot-plug, rumble) ─┐
keyboard (window key edges)  ─────┼─▶ xbox_InputGetState(port) ─┬─▶ src/xdk  XInput at API level (HLE)
pad script / live input / PAD_PRESS┘                            └─▶ src/usb  USB pad report (LLE, title's own XPP)
```

## Files

| File | Purpose |
|------|---------|
| `xinput_xbox.h` | types, button constants, API |
| `xinput_device.c` | SDL3 host pads, keyboard, port assignment, packet numbers |
| `xinput_script.c` | `RECOMP_PAD_SCRIPT`, `RECOMP_PAD_LIVE`, `RECOMP_PAD_PRESS` (syntax in the file header) |

SDL3 on every OS (Windows, Linux, Android): it reads XInput, WGI, DirectInput, HIDAPI and evdev pads behind one
API, so PlayStation, Switch and generic pads work too. The thread that owns the window initialises
`SDL_INIT_GAMEPAD` and pumps events (`nv2a_vk`); without a window there are no host pads, only keyboard and script.

## Ports

- Host pads take free ports in the order SDL reports them and keep them while attached; re-enumerated at most every
  500 ms.
- The keyboard is merged on top of port 0 (`RECOMP_KEYBOARD=1` or `xbox_InputEnableKeyboard(1)`).
  `RECOMP_KEYBOARD_PORT=<0-3>` makes it a controller of its own on that port, which host pads skip: keyboard + one
  pad = two players.
- `RECOMP_PADS=<n>` reports ports 0..n-1 as connected (tests); a script step for `p2-` connects port 1.
- `xbox_InputConnectedMask()`: bit n = port n has a controller.

## API

```c
void     xbox_InputInit(void);
DWORD    xbox_InputGetState(DWORD port, XBOX_INPUT_STATE *s);   /* filled always; 0 = connected */
unsigned xbox_InputConnectedMask(void);
DWORD    xbox_InputSetState(DWORD port, const XBOX_VIBRATION *v); /* SDL rumble */
BOOL     xbox_InputIsConnected(DWORD port);
DWORD    xbox_InputGetCapabilities(DWORD port, DWORD flags, XBOX_INPUT_CAPABILITIES *caps);
void     xbox_InputKeyEvent(int scancode, int down);              /* from the window */
void     xbox_InputSetKeymap(const XBOX_KEY_BINDING *map, int count);
```

The packet number moves exactly when the state changes (titles look for edges by it).

## Mapping (positional)

| Xbox | SDL3 gamepad |
|------|--------------|
| A B X Y | south, east, west, north (Cross/Circle/Square/Triangle on a PlayStation pad) |
| Black / White | left / right shoulder |
| L / R trigger | left / right trigger axis (0..255) |
| Start / Back | start / back |
| D-pad, sticks, L3/R3 | same (Y axes flipped: up is positive on the Xbox) |
