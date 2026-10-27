/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 * Host pads come from SDL3 on every OS; the keyboard and the pad script
 * (xinput_script.c) are merged on top.
 */

#include "xinput_xbox.h"
#include "xinput_bind.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- bindings per device (xinput_bind.h) ---------------------------------
 * The keyboard's default is the title's keymap, a pad's the positional
 * table; the own binding comes from <prefix>_KEYBOARD / <prefix>_PAD_<GUID> /
 * <prefix>_PAD, else the port's store (bind.keyboard, bind.pad.<GUID>), else
 * the default. <prefix>_RESET=1 ignores the store. */
static XboxBindGet    s_bind_get;
static XboxBindSet    s_bind_set;
static char           s_bind_env[48] = "RECOMP_BIND";
static volatile int   s_bind_menu;
static volatile int   s_bind_suppress;   /* a press being recorded */
static XboxBindMap    s_kbd_def, s_kbd_own;
static int            s_kbd_loaded;
static XboxBindMap    s_pad_def;

void xbox_InputBindSetMenu(int menu)
{
    s_bind_menu = menu ? 1 : 0;
}

static int bind_reset(void)
{
    char name[64];
    const char *e;
    snprintf(name, sizeof name, "%s_RESET", s_bind_env);
    e = getenv(name);
    return e && *e == '1';
}

/* The own binding of a device: env, store, default -- in that order. */
static void bind_load(XboxBindKind kind, const char *env_key, const char *env_key2, const char *store_key,
                      const XboxBindMap *def, XboxBindMap *own, const char *what)
{
    char name[96];
    const char *text = NULL, *from = "default";

    if (env_key) {
        snprintf(name, sizeof name, "%s_%s", s_bind_env, env_key);
        if ((text = getenv(name)) != NULL)
            from = "env";
    }
    if (!text && env_key2) {
        snprintf(name, sizeof name, "%s_%s", s_bind_env, env_key2);
        if ((text = getenv(name)) != NULL)
            from = "env";
    }
    if (!text && s_bind_get && !bind_reset() && *(text = s_bind_get(store_key, "")))
        from = "settings";
    if (xbox_bind_parse(kind, text, def, own) != 0) {
        fprintf(stderr, "  INPUT: %s binding \"%s\" (%s) not understood - default kept\n", what, text, from);
        *own = *def;
    } else if (text && *text) {
        fprintf(stderr, "  INPUT: %s binding from %s: %s\n", what, from, text);
    }
}

static void kbd_bind_load(void)
{
    bind_load(XBOX_BIND_KEYBOARD, "KEYBOARD", NULL, "bind.keyboard", &s_kbd_def, &s_kbd_own, "keyboard");
    s_kbd_loaded = 1;
}

void xbox_InputBindSetStore(XboxBindGet get, XboxBindSet set, const char *env_prefix)
{
    s_bind_get = get;
    s_bind_set = set;
    if (env_prefix && *env_prefix)
        snprintf(s_bind_env, sizeof s_bind_env, "%s", env_prefix);
    s_kbd_loaded = 0;   /* reread at the next poll; pads when they are opened */
}

/* ---- keyboard, as controller 0 -------------------------------------------
 *
 * A title that waits on PRESS START is unreachable on a machine with no
 * controller plugged in, which is most machines someone brings this up on.
 * The whole point of a recompilation is to be able to look at the thing
 * running, and a build nobody can press a button in cannot be looked at.
 *
 * Off by default, because a keyboard silently acting as player 1 is
 * surprising when a real pad is what you meant to use. RECOMP_KEYBOARD=1 or
 * xbox_InputEnableKeyboard(1) turns it on. It only ever answers for port 0,
 * and is merged on top of a pad connected there, so a real controller keeps
 * working.
 *
 * Keys are HID usage IDs (xinput_xbox.h), so the table below means the same
 * physical key on every host and every keyboard layout. A title sets its own
 * layout with xbox_InputSetKeymap; the default is the one this layer always
 * had, picked the way a Dreamcast or Saturn emulator would:
 *
 *   arrows        d-pad             Enter      START
 *   Z X A S       A B X Y           Backspace  BACK
 *   Q E           white black       1 3        triggers
 *   numpad 8/2/4/6 left thumb       I/K/J/L    right thumb
 *
 * Key state arrives as edges from whichever window has the focus
 * (xbox_InputKeyEvent; nv2a_vk forwards its SDL key events), so typing in
 * another application does not drive the game. The D3D11 framebuffer window
 * keeps its own table, read through xbox_FramebufferKeyDown.
 */
#define KEY_SLOTS 512

static volatile uint8_t s_key_down[KEY_SLOTS];
static volatile int     s_key_events;        /* a window feeds edges */
static int              s_kbd_forced = -1;   /* xbox_InputEnableKeyboard */

static XBOX_KEY_BINDING s_keymap[XBOX_KEYMAP_MAX] = {
    {82, XBOX_KEY_DPAD_UP}, {81, XBOX_KEY_DPAD_DOWN},
    {80, XBOX_KEY_DPAD_LEFT}, {79, XBOX_KEY_DPAD_RIGHT},
    {40, XBOX_KEY_START}, {42, XBOX_KEY_BACK},
    {225, XBOX_KEY_LEFT_THUMB}, {224, XBOX_KEY_RIGHT_THUMB},
    {29, XBOX_KEY_A}, {27, XBOX_KEY_B}, {4, XBOX_KEY_X}, {22, XBOX_KEY_Y},
    {20, XBOX_KEY_WHITE}, {8, XBOX_KEY_BLACK},
    {30, XBOX_KEY_LTRIGGER}, {32, XBOX_KEY_RTRIGGER},
    /* W/A/S/D would collide with the face buttons above, so the left thumb
     * shares the arrow keys' row on the numeric pad instead. */
    {92, XBOX_KEY_LX_NEG}, {94, XBOX_KEY_LX_POS},
    {90, XBOX_KEY_LY_NEG}, {96, XBOX_KEY_LY_POS},
    {13, XBOX_KEY_RX_NEG}, {15, XBOX_KEY_RX_POS},
    {14, XBOX_KEY_RY_NEG}, {12, XBOX_KEY_RY_POS},
};
static int s_keymap_len = 24;

void xbox_InputKeyEvent(int scancode, int down)
{
    if (scancode < 0) {             /* focus lost (nv2a_vk.h) */
        xbox_InputKeyReleaseAll();
        return;
    }
    if (scancode == 0 || scancode >= KEY_SLOTS)
        return;
    s_key_down[scancode] = down ? 1 : 0;
    s_key_events = 1;
}

void xbox_InputKeyReleaseAll(void)
{
    int i;
    for (i = 0; i < KEY_SLOTS; i++)
        s_key_down[i] = 0;
}

void xbox_InputSetKeymap(const XBOX_KEY_BINDING *map, int count)
{
    int i, n = 0;
    if (!map || count < 0)
        return;
    for (i = 0; i < count && n < XBOX_KEYMAP_MAX; i++)
        if (map[i].scancode < KEY_SLOTS && map[i].control < XBOX_KEY_CONTROL_COUNT)
            s_keymap[n++] = map[i];
    s_keymap_len = n;
    s_kbd_loaded = 0;   /* the default binding changed */
}

void xbox_InputEnableKeyboard(int on)
{
    s_kbd_forced = on ? 1 : 0;
}

static int keyboard_enabled(void)
{
    static int env = -1;
    if (s_kbd_forced >= 0)
        return s_kbd_forced;
    if (env < 0) {
        const char *v = getenv("RECOMP_KEYBOARD");
        env = (v && *v && *v != '0') ? 1 : 0;
    }
    return env;
}

#if defined(_WIN32)
/* The D3D11 path's framebuffer window (src/video/fb_present.c) records
 * Windows virtual-key codes. Declared rather than included so src/input does
 * not depend on the video module's include path. */
extern int xbox_FramebufferKeyDown(int vk);

static int hid_to_vk(int hid)
{
    if (hid >= 4 && hid <= 29)  return 'A' + (hid - 4);
    if (hid >= 30 && hid <= 38) return '1' + (hid - 30);
    if (hid == 39) return '0';
    if (hid >= 89 && hid <= 97) return 0x61 + (hid - 89);   /* VK_NUMPAD1.. */
    switch (hid) {
    case 40:  return 0x0D;   /* VK_RETURN */
    case 42:  return 0x08;   /* VK_BACK */
    case 44:  return 0x20;   /* VK_SPACE */
    case 79:  return 0x27;   /* VK_RIGHT */
    case 80:  return 0x25;   /* VK_LEFT */
    case 81:  return 0x28;   /* VK_DOWN */
    case 82:  return 0x26;   /* VK_UP */
    case 224: return 0x11;   /* VK_CONTROL */
    case 225: return 0x10;   /* VK_SHIFT */
    }
    return 0;
}
#endif

static int key_down(int hid)
{
    if (s_key_down[hid] || xbox_InputScriptKeyDown(hid))
        return 1;
#if defined(_WIN32)
    /* Only while no window feeds edges: then the D3D11 window is the one. */
    if (!s_key_events) {
        int vk = hid_to_vk(hid);
        return vk && xbox_FramebufferKeyDown(vk);
    }
#endif
    return 0;
}

static uint8_t key_value(void *ctx, uint16_t src)
{
    (void)ctx;
    return src < KEY_SLOTS && key_down(src) ? 255 : 0;
}

/* The keyboard as a pad. */
static void keyboard_state(XBOX_INPUT_STATE *pState)
{
    static DWORD packet;
    static const WORD dig[8] = {
        XBOX_GAMEPAD_DPAD_UP, XBOX_GAMEPAD_DPAD_DOWN, XBOX_GAMEPAD_DPAD_LEFT,
        XBOX_GAMEPAD_DPAD_RIGHT, XBOX_GAMEPAD_START, XBOX_GAMEPAD_BACK,
        XBOX_GAMEPAD_LEFT_THUMB, XBOX_GAMEPAD_RIGHT_THUMB,
    };
    uint8_t held[XBOX_KEY_CONTROL_COUNT];
    int i;

    memset(pState, 0, sizeof(*pState));
    if (!s_kbd_loaded) {
        xbox_bind_from_keymap(&s_kbd_def, s_keymap, s_keymap_len);
        kbd_bind_load();
    }
    xbox_bind_eval(XBOX_BIND_KEYBOARD, &s_kbd_own, &s_kbd_def, s_bind_menu, key_value, NULL, held);

    for (i = 0; i < 8; i++)
        if (held[XBOX_KEY_DPAD_UP + i])
            pState->Gamepad.wButtons |= dig[i];

    /* Analog on the console, so a key is 255 rather than a flag -- a title
     * that reads these as a pressure never sees a press if they are 1. The
     * XBOX_KEY_A..RTRIGGER order is the bAnalogButtons order. */
    for (i = 0; i < 8; i++)
        pState->Gamepad.bAnalogButtons[i] = held[XBOX_KEY_A + i] ? 255 : 0;

    /* A thumb axis from two keys, at the full deflection a key implies. */
#define KEY_AXIS(neg, pos) \
    (SHORT)((held[pos] ? 32767 : 0) - (held[neg] ? 32767 : 0))
    pState->Gamepad.sThumbLX = KEY_AXIS(XBOX_KEY_LX_NEG, XBOX_KEY_LX_POS);
    pState->Gamepad.sThumbLY = KEY_AXIS(XBOX_KEY_LY_NEG, XBOX_KEY_LY_POS);
    pState->Gamepad.sThumbRX = KEY_AXIS(XBOX_KEY_RX_NEG, XBOX_KEY_RX_POS);
    pState->Gamepad.sThumbRY = KEY_AXIS(XBOX_KEY_RY_NEG, XBOX_KEY_RY_POS);
#undef KEY_AXIS

    /* The title's input layer looks for button edges, so the packet number
     * has to move whenever the state does or a press is never noticed. */
    pState->dwPacketNumber = ++packet;
}

/* Merge the keyboard on top of a pad's state rather than only standing in
 * for a missing pad.
 *
 * The first version put the keyboard behind XInput's failure, on the
 * assumption that with nothing plugged in the call would fail. It does
 * not: under Wine XInputGetState returns ERROR_SUCCESS and a gamepad
 * with every button at rest, so the fallback was unreachable and
 * pressing a key did nothing at all.
 *
 * Merging is also the better rule. A real pad keeps working -- its
 * buttons are already in pState and the keyboard only adds to them --
 * and there is no special case left to get wrong. */
static void keyboard_merge(XBOX_INPUT_STATE *pState)
{
    XBOX_INPUT_STATE kb;
    int i;

    keyboard_state(&kb);
    pState->Gamepad.wButtons |= kb.Gamepad.wButtons;
    for (i = 0; i < 8; i++)
        if (kb.Gamepad.bAnalogButtons[i] > pState->Gamepad.bAnalogButtons[i])
            pState->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
    if (kb.Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
    if (kb.Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
    if (kb.Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
    if (kb.Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
    /* The input layer records edges, so an unchanged packet number is
     * read as the same state and the press never happens. */
    pState->dwPacketNumber = kb.dwPacketNumber;
}

/* ======================================================================== */
/* ====================  SDL3 gamepad backend (every host)  =============== */
/* ======================================================================== */

/* SDL3 on every host (A10: portability, one code path; SDL3 is the window
 * and audio library anyway). It replaces the Windows XInput and the POSIX
 * SDL2 backends: SDL3 reads XInput, WGI, DirectInput, HIDAPI, evdev and
 * Android pads behind one API, so PlayStation, Switch and generic pads work
 * on Windows too.
 *
 * The thread that owns the window initialises SDL_INIT_GAMEPAD next to the
 * video subsystem and pumps the events (nv2a_vk). Without that -- a headless
 * run, or a renderer that does not use SDL -- there are no host pads, only
 * the keyboard and the script.
 *
 * Ports: host pads take free ports in the order SDL reports them and keep
 * them while attached (unplugging pad 1 does not move pad 2 to port 0). The
 * keyboard is merged on top of port 0 by default; RECOMP_KEYBOARD_PORT=<0-3>
 * makes it a controller of its own on that port, which host pads skip, so a
 * keyboard and one pad are two players. RECOMP_PADS=<n> reports ports 0..n-1
 * as connected with nothing pressed (tests: split-screen without pads), and
 * a script step for "p2-" connects port 1 the same way. */

#include <SDL3/SDL.h>

static SDL_Gamepad   *g_pads[XBOX_MAX_CONTROLLERS];
static SDL_JoystickID g_pad_id[XBOX_MAX_CONTROLLERS];
static char           g_pad_guid[XBOX_MAX_CONTROLLERS][33];   /* SDL GUID: one binding per pad model */
static XboxBindMap    g_pad_own[XBOX_MAX_CONTROLLERS];
static DWORD          g_packet[XBOX_MAX_CONTROLLERS];
static XBOX_GAMEPAD   g_last[XBOX_MAX_CONTROLLERS];
static SDL_Mutex     *g_lock;
static int            g_kbd_port = -2;   /* -2 unread, -1 merged on port 0 */
static unsigned       g_forced;           /* RECOMP_PADS */

static void read_config(void)
{
    const char *e;
    if (g_kbd_port != -2)
        return;
    g_kbd_port = -1;
    e = getenv("RECOMP_KEYBOARD_PORT");
    if (e && *e >= '0' && *e <= '3')
        g_kbd_port = *e - '0';
    e = getenv("RECOMP_PADS");
    if (e) {
        int n = atoi(e);
        if (n > XBOX_MAX_CONTROLLERS) n = XBOX_MAX_CONTROLLERS;
        g_forced = n > 0 ? (1u << n) - 1u : 0u;
    }
}

/* Open new pads and drop removed ones, at most every 500 ms: a title polls
 * every frame, and enumerating each time would cost more than the read. */
static void refresh_pads(void)
{
    static Uint64 last;
    Uint64 now;
    SDL_JoystickID *ids;
    int n = 0, i, p;

    if (!SDL_WasInit(SDL_INIT_GAMEPAD))
        return;
    now = SDL_GetTicks();
    if (last && now - last < 500)
        return;
    last = now;

    for (p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        if (g_pads[p] && !SDL_GamepadConnected(g_pads[p])) {
            SDL_CloseGamepad(g_pads[p]);
            g_pads[p] = NULL;
            g_pad_id[p] = 0;
        }

    ids = SDL_GetGamepads(&n);
    for (i = 0; ids && i < n; i++) {
        int known = 0;
        for (p = 0; p < XBOX_MAX_CONTROLLERS; p++)
            if (g_pads[p] && g_pad_id[p] == ids[i])
                known = 1;
        if (known)
            continue;
        for (p = 0; p < XBOX_MAX_CONTROLLERS; p++)
            if (!g_pads[p] && p != g_kbd_port)
                break;
        if (p == XBOX_MAX_CONTROLLERS)
            break;
        g_pads[p] = SDL_OpenGamepad(ids[i]);
        if (g_pads[p]) {
            char env_key[48], store_key[48];
            g_pad_id[p] = ids[i];
            SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[i]), g_pad_guid[p], (int)sizeof g_pad_guid[p]);
            fprintf(stderr, "  INPUT: pad \"%s\" on port %d, GUID %s\n",
                    SDL_GetGamepadName(g_pads[p]) ? SDL_GetGamepadName(g_pads[p]) : "?", p, g_pad_guid[p]);
            snprintf(env_key, sizeof env_key, "PAD_%s", g_pad_guid[p]);
            snprintf(store_key, sizeof store_key, "bind.pad.%s", g_pad_guid[p]);
            bind_load(XBOX_BIND_PAD, env_key, "PAD", store_key, &s_pad_def, &g_pad_own[p], "pad");
            fflush(stderr);
        }
    }
    SDL_free(ids);
}

void xbox_InputInit(void)
{
    static int pad_def;
    if (!pad_def) {
        xbox_bind_pad_default(&s_pad_def);
        pad_def = 1;
    }
    read_config();
    if (!g_lock)
        g_lock = SDL_CreateMutex();
}

static void lock(void)   { if (g_lock) SDL_LockMutex(g_lock); }
static void unlock(void) { if (g_lock) SDL_UnlockMutex(g_lock); }

unsigned xbox_InputConnectedMask(void)
{
    unsigned m;
    int p;

    xbox_InputInit();
    lock();
    refresh_pads();
    m = g_forced | xbox_InputScriptPorts();
    for (p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        if (g_pads[p])
            m |= 1u << p;
    if (keyboard_enabled())
        m |= 1u << (g_kbd_port >= 0 ? g_kbd_port : 0);
    unlock();
    return m;
}

/* A pad source's value, 0..255 (XBOX_PAD_SRC_*: SDL positional names --
 * SOUTH is A on an Xbox pad and Cross on a PlayStation one; Black and White
 * default to where a modern pad has its shoulder buttons). */
static uint8_t pad_value(void *ctx, uint16_t src)
{
    static const SDL_GamepadButton btn[XBOX_PAD_SRC_COUNT] = {
        [XBOX_PAD_SRC_SOUTH] = SDL_GAMEPAD_BUTTON_SOUTH, [XBOX_PAD_SRC_EAST] = SDL_GAMEPAD_BUTTON_EAST,
        [XBOX_PAD_SRC_WEST] = SDL_GAMEPAD_BUTTON_WEST, [XBOX_PAD_SRC_NORTH] = SDL_GAMEPAD_BUTTON_NORTH,
        [XBOX_PAD_SRC_BACK] = SDL_GAMEPAD_BUTTON_BACK, [XBOX_PAD_SRC_START] = SDL_GAMEPAD_BUTTON_START,
        [XBOX_PAD_SRC_LSTICK] = SDL_GAMEPAD_BUTTON_LEFT_STICK, [XBOX_PAD_SRC_RSTICK] = SDL_GAMEPAD_BUTTON_RIGHT_STICK,
        [XBOX_PAD_SRC_LSHOULDER] = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
        [XBOX_PAD_SRC_RSHOULDER] = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        [XBOX_PAD_SRC_DUP] = SDL_GAMEPAD_BUTTON_DPAD_UP, [XBOX_PAD_SRC_DDOWN] = SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        [XBOX_PAD_SRC_DLEFT] = SDL_GAMEPAD_BUTTON_DPAD_LEFT, [XBOX_PAD_SRC_DRIGHT] = SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    };
    SDL_Gamepad *c = ctx;

    /* Trigger axes are 0..32767 -> 0..255. */
    if (src == XBOX_PAD_SRC_LTRIGGER)
        return (uint8_t)(SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
    if (src == XBOX_PAD_SRC_RTRIGGER)
        return (uint8_t)(SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
    if (src > XBOX_PAD_SRC_NONE && src < XBOX_PAD_SRC_LTRIGGER)
        return SDL_GetGamepadButton(c, btn[src]) ? 255 : 0;
    return 0;
}

static void pad_state(SDL_Gamepad *c, const XboxBindMap *own, XBOX_GAMEPAD *g)
{
    static const WORD dig[8] = {
        XBOX_GAMEPAD_DPAD_UP, XBOX_GAMEPAD_DPAD_DOWN, XBOX_GAMEPAD_DPAD_LEFT, XBOX_GAMEPAD_DPAD_RIGHT,
        XBOX_GAMEPAD_START, XBOX_GAMEPAD_BACK, XBOX_GAMEPAD_LEFT_THUMB, XBOX_GAMEPAD_RIGHT_THUMB,
    };
    uint8_t v[XBOX_KEY_CONTROL_COUNT];
    int i;

    /* Buttons and triggers through the pad's binding (the lock-out rule in
     * menus); a trigger bound to a digital control counts past the console's
     * analog threshold. */
    xbox_bind_eval(XBOX_BIND_PAD, own, &s_pad_def, s_bind_menu, pad_value, c, v);
    for (i = 0; i < 8; i++)
        if (v[XBOX_KEY_DPAD_UP + i] > XBOX_ANALOG_BUTTON_THRESHOLD)
            g->wButtons |= dig[i];
    for (i = 0; i < 8; i++)   /* XBOX_KEY_A..RTRIGGER = the bAnalogButtons order */
        g->bAnalogButtons[i] = v[XBOX_KEY_A + i];
    /* SDL's Y axes point down, the Xbox's up; -1 - v keeps -32768 in range. */
    g->sThumbLX = SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFTX);
    g->sThumbLY = (SHORT)(-1 - SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFTY));
    g->sThumbRX = SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHTX);
    g->sThumbRY = (SHORT)(-1 - SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHTY));
}

/* RECOMP_INPUT_DIAG: port 0's state once a second, to tell "the key never
 * arrives" from "the chain is not wired" without a rebuild. RECOMP_INPUT_DIAG=2: every port,
 * each state change with its packet number, and every 2 s how often each port
 * was polled -- which player a screen is waiting on shows as the port it
 * stops (or keeps) reading. */
static int input_diag_level(void)
{
    static int diag = -1;
    if (diag < 0) {
        const char *e = getenv("RECOMP_INPUT_DIAG");
        diag = !e ? 0 : (*e == '2') ? 2 : 1;
    }
    return diag;
}

static void input_diag_ports(DWORD port, const XBOX_INPUT_STATE *s, DWORD rc)
{
    static XBOX_GAMEPAD last[XBOX_MAX_CONTROLLERS];
    static unsigned long polls[XBOX_MAX_CONTROLLERS], t0;
    unsigned long now = (unsigned long)GetTickCount();
    const XBOX_GAMEPAD *g = &s->Gamepad;

    if (port >= XBOX_MAX_CONTROLLERS)
        return;
    polls[port]++;
    if (memcmp(g, &last[port], sizeof *g) != 0) {
        last[port] = *g;
        fprintf(stderr, "  [INPUT] port %lu packet %lu rc=%lu buttons=0x%04X a/b/x/y=%u,%u,%u,%u lx=%d ly=%d\n",
                (unsigned long)port, (unsigned long)s->dwPacketNumber, (unsigned long)rc, g->wButtons,
                g->bAnalogButtons[0], g->bAnalogButtons[1], g->bAnalogButtons[2], g->bAnalogButtons[3],
                g->sThumbLX, g->sThumbLY);
    }
    if (!t0)
        t0 = now;
    if (now - t0 >= 2000) {
        fprintf(stderr, "  [INPUT] polls in %lu ms: port0 %lu port1 %lu port2 %lu port3 %lu\n",
                now - t0, polls[0], polls[1], polls[2], polls[3]);
        memset(polls, 0, sizeof polls);
        t0 = now;
    }
    fflush(stderr);
}

static void input_diag(const XBOX_INPUT_STATE *s, DWORD rc)
{
    static unsigned long last;
    unsigned long now;
    const XBOX_GAMEPAD *g = &s->Gamepad;

    now = (unsigned long)GetTickCount();
    if (input_diag_level() != 1 || now - last < 1000)
        return;
    last = now;
    fprintf(stderr, "  [INPUT] kbd=%d kbd_port=%d rc=%lu buttons=0x%04X analog=%u,%u,%u,%u,"
            "%u,%u,%u,%u lx=%d ly=%d rx=%d ry=%d\n",
            keyboard_enabled(), g_kbd_port, (unsigned long)rc, g->wButtons,
            g->bAnalogButtons[0], g->bAnalogButtons[1], g->bAnalogButtons[2],
            g->bAnalogButtons[3], g->bAnalogButtons[4], g->bAnalogButtons[5],
            g->bAnalogButtons[6], g->bAnalogButtons[7],
            g->sThumbLX, g->sThumbLY, g->sThumbRX, g->sThumbRY);
    fflush(stderr);
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    unsigned mask;
    DWORD rc;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;
    mask = xbox_InputConnectedMask();
    memset(pState, 0, sizeof(*pState));

    lock();
    if (g_pads[dwPort])
        pad_state(g_pads[dwPort], &g_pad_own[dwPort], &pState->Gamepad);
    unlock();
    if (keyboard_enabled() && (int)dwPort == (g_kbd_port >= 0 ? g_kbd_port : 0))
        keyboard_merge(pState);
    xbox_InputScriptApply((int)dwPort, &pState->Gamepad);
    if (s_bind_suppress)   /* a press being recorded (rebinding menu) */
        memset(&pState->Gamepad, 0, sizeof pState->Gamepad);

    /* A title's input layer looks for changes by the packet number: it moves
     * exactly when the state does. */
    if (memcmp(&pState->Gamepad, &g_last[dwPort], sizeof(XBOX_GAMEPAD)) != 0) {
        g_last[dwPort] = pState->Gamepad;
        g_packet[dwPort]++;
    }
    pState->dwPacketNumber = g_packet[dwPort];
    rc = (mask >> dwPort) & 1u ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
    if (dwPort == 0)
        input_diag(pState, rc);
    if (input_diag_level() == 2)
        input_diag_ports(dwPort, pState, rc);
    return rc;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    DWORD rc = ERROR_DEVICE_NOT_CONNECTED;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return rc;
    lock();
    /* SDL rumble runs for a duration; the title sets the motors again when it
     * changes them, so hold each setting until the next (or 10 s). */
    if (g_pads[dwPort]) {
        SDL_RumbleGamepad(g_pads[dwPort], pVibration->wLeftMotorSpeed,
                          pVibration->wRightMotorSpeed, 10000);
        rc = ERROR_SUCCESS;
    }
    unlock();
    return rc;
}

/* ---- rebinding menu interface (xinput_bind.h) ---- */

static int bind_store(const char *key, XboxBindKind kind, const XboxBindMap *own, const XboxBindMap *def)
{
    char text[1024];
    if (!s_bind_set || xbox_bind_format(kind, own, def, text, sizeof text) < 0)
        return -1;
    s_bind_set(key, text);
    return 0;
}

void xbox_InputBindKeyboard(XboxBindMap *own, XboxBindMap *def)
{
    if (!s_kbd_loaded) {
        xbox_bind_from_keymap(&s_kbd_def, s_keymap, s_keymap_len);
        kbd_bind_load();
    }
    *own = s_kbd_own;
    *def = s_kbd_def;
}

int xbox_InputBindStoreKeyboard(const XboxBindMap *own)
{
    s_kbd_own = *own;
    return bind_store("bind.keyboard", XBOX_BIND_KEYBOARD, own, &s_kbd_def);
}

int xbox_InputBindPad(int port, char guid[33], XboxBindMap *own, XboxBindMap *def)
{
    int ok;
    if (port < 0 || port >= XBOX_MAX_CONTROLLERS)
        return 0;
    lock();
    ok = g_pads[port] != NULL;
    if (ok) {
        memcpy(guid, g_pad_guid[port], 33);
        *own = g_pad_own[port];
        *def = s_pad_def;
    }
    unlock();
    return ok;
}

int xbox_InputBindStorePad(int port, const XboxBindMap *own)
{
    char key[48], guid[33];
    int p;
    if (port < 0 || port >= XBOX_MAX_CONTROLLERS)
        return -1;
    lock();
    if (!g_pads[port]) {
        unlock();
        return -1;
    }
    memcpy(guid, g_pad_guid[port], 33);
    for (p = 0; p < XBOX_MAX_CONTROLLERS; p++)
        if (g_pads[p] && !strcmp(g_pad_guid[p], guid))
            g_pad_own[p] = *own;
    unlock();
    snprintf(key, sizeof key, "bind.pad.%s", guid);
    return bind_store(key, XBOX_BIND_PAD, own, &s_pad_def);
}

void xbox_InputPadName(int port, char *buf, size_t size)
{
    const char *n = NULL;
    if (!size)
        return;
    lock();
    if (port >= 0 && port < XBOX_MAX_CONTROLLERS && g_pads[port])
        n = SDL_GetGamepadName(g_pads[port]);
    snprintf(buf, size, "%s", n ? n : "");
    unlock();
}

int xbox_InputKeyboardPort(void)
{
    read_config();
    if (!keyboard_enabled())
        return -1;
    return g_kbd_port >= 0 ? g_kbd_port : 0;
}

uint16_t xbox_InputBindHeldKeyboard(void)
{
    int k;
    for (k = 1; k < KEY_SLOTS; k++)
        if (key_down(k))
            return (uint16_t)k;
    return 0;
}

uint16_t xbox_InputBindHeldPad(int port)
{
    uint16_t s, got = 0;
    if (port < 0 || port >= XBOX_MAX_CONTROLLERS)
        return 0;
    lock();
    if (g_pads[port])
        for (s = 1; s < XBOX_PAD_SRC_COUNT && !got; s++)
            if (pad_value(g_pads[port], s) > 127)
                got = s;
    unlock();
    return got;
}

void xbox_InputBindSuppress(int on)
{
    s_bind_suppress = on ? 1 : 0;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return (xbox_InputConnectedMask() >> dwPort) & 1u;
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps || !xbox_InputIsConnected(dwPort))
        return ERROR_DEVICE_NOT_CONNECTED;

    /* Every host pad is presented as the standard Xbox gamepad: every button
     * and axis present at full resolution, both motors. */
    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GC_GAMEPAD */
    pCaps->Gamepad.wButtons = 0x00FF;
    memset(pCaps->Gamepad.bAnalogButtons, 0xFF, sizeof pCaps->Gamepad.bAnalogButtons);
    pCaps->Gamepad.sThumbLX = pCaps->Gamepad.sThumbLY = (SHORT)0xFFC0;
    pCaps->Gamepad.sThumbRX = pCaps->Gamepad.sThumbRY = (SHORT)0xFFC0;
    pCaps->Vibration.wLeftMotorSpeed = pCaps->Vibration.wRightMotorSpeed = 0xFFFF;
    return ERROR_SUCCESS;
}
