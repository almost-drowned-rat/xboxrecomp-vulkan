/**
 * xinput_bind.h - button bindings per input device (rebinding).
 *
 * A device is the keyboard or a host pad model (SDL GUID: identical pads
 * share one binding). A binding maps each control (XBOX_KEY_*: d-pad, Start,
 * Back, thumb clicks, A B X Y, Black White, triggers, and for the keyboard
 * the stick half-axes) to up to XBOX_BIND_SRC_MAX sources: HID usage IDs
 * (= SDL scancodes, the physical key) for the keyboard, XBOX_PAD_SRC_* (SDL
 * positional names) for pads. Pad sticks stay fixed (not bindable).
 *
 * Lock-out rule: the device's own binding always wins. In menus a source of
 * the default binding also acts, but only when the own binding does not use
 * that source -- so the default layout keeps navigating after a bad binding,
 * and a key moved to another control never triggers two controls.
 *
 * Text form (settings files, env overrides): "control:source[+source],..."
 * applied on top of the default; "control:none" unbinds. Controls are the
 * pad-script names (up down left right start back lthumb rthumb a b x y black
 * white lt rt lleft lright ldown lup rleft rright rdown rup); keyboard
 * sources "sc<HID usage ID>", pad sources south east west north back start
 * lstick rstick lshoulder rshoulder dup ddown dleft dright ltrigger rtrigger.
 * Only controls that differ from the default are written.
 *
 * This file is pure logic (no SDL): tests/input_bind.
 */
#ifndef XBOX_XINPUT_BIND_H
#define XBOX_XINPUT_BIND_H

#include <stddef.h>
#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_BIND_SRC_MAX 2

typedef enum { XBOX_BIND_KEYBOARD, XBOX_BIND_PAD } XboxBindKind;

/* Pad sources; 0 = none. */
enum {
    XBOX_PAD_SRC_NONE,
    XBOX_PAD_SRC_SOUTH, XBOX_PAD_SRC_EAST, XBOX_PAD_SRC_WEST, XBOX_PAD_SRC_NORTH,
    XBOX_PAD_SRC_BACK, XBOX_PAD_SRC_START, XBOX_PAD_SRC_LSTICK, XBOX_PAD_SRC_RSTICK,
    XBOX_PAD_SRC_LSHOULDER, XBOX_PAD_SRC_RSHOULDER,
    XBOX_PAD_SRC_DUP, XBOX_PAD_SRC_DDOWN, XBOX_PAD_SRC_DLEFT, XBOX_PAD_SRC_DRIGHT,
    XBOX_PAD_SRC_LTRIGGER, XBOX_PAD_SRC_RTRIGGER,
    XBOX_PAD_SRC_COUNT
};

/* Pads bind the buttons and triggers only (controls below this). */
#define XBOX_BIND_PAD_CONTROLS (XBOX_KEY_RTRIGGER + 1)

typedef struct {
    uint16_t src[XBOX_KEY_CONTROL_COUNT][XBOX_BIND_SRC_MAX];   /* 0 = unused slot */
} XboxBindMap;

/* The number of controls a kind binds. */
int xbox_bind_controls(XboxBindKind kind);

void xbox_bind_clear(XboxBindMap *m);
/* The toolkit's pad default: positional (south = A, shoulders = Black/White). */
void xbox_bind_pad_default(XboxBindMap *m);
/* A keymap (xbox_InputSetKeymap form) as a binding. */
void xbox_bind_from_keymap(XboxBindMap *m, const XBOX_KEY_BINDING *map, int count);

const char *xbox_bind_control_name(int control);
/* Source name: "sc40" for the keyboard, "south" for pads; buf for "sc<n>". */
const char *xbox_bind_source_name(XboxBindKind kind, uint16_t src, char *buf, size_t size);

/* Parse text on top of *def into *out. Returns 0, or -1 (out untouched) on
 * an unknown control or source. NULL or "" gives the default. */
int xbox_bind_parse(XboxBindKind kind, const char *text, const XboxBindMap *def, XboxBindMap *out);
/* The controls of *m that differ from *def, as text; "" when equal. Returns
 * the length, or -1 if buf is too small. */
int xbox_bind_format(XboxBindKind kind, const XboxBindMap *m, const XboxBindMap *def, char *buf, size_t size);

/* Does *m use src for any control? */
int xbox_bind_uses(XboxBindKind kind, const XboxBindMap *m, uint16_t src);
/* Bit c set when control c shares a source with another control. */
uint32_t xbox_bind_duplicates(XboxBindKind kind, const XboxBindMap *m);
/* Set control c to exactly one source (0 = unbind). */
void xbox_bind_set(XboxBindMap *m, int control, uint16_t src);

/* The controls' values (0..255) from the sources' values (value(ctx, src),
 * 0..255): the own binding; in a menu also the default binding's sources the
 * own binding does not use. */
void xbox_bind_eval(XboxBindKind kind, const XboxBindMap *own, const XboxBindMap *def, int menu,
                    uint8_t (*value)(void *ctx, uint16_t src), void *ctx,
                    uint8_t out[XBOX_KEY_CONTROL_COUNT]);

/* ---- device registry (xinput_device.c) ---------------------------------
 * Where bindings are kept: the port's settings store (get returns def when
 * the key is absent; an empty value is the default) and its env prefix
 * (<prefix>_KEYBOARD, <prefix>_PAD, <prefix>_PAD_<GUID>, <prefix>_RESET=1).
 * Store keys: bind.keyboard, bind.pad.<GUID>. Without a call nothing is
 * stored and the env prefix is RECOMP_BIND. */
typedef const char *(*XboxBindGet)(const char *key, const char *def);
typedef void (*XboxBindSet)(const char *key, const char *value);
void xbox_InputBindSetStore(XboxBindGet get, XboxBindSet set, const char *env_prefix);
/* Menu or gameplay: the lock-out rule above applies in menus only. */
void xbox_InputBindSetMenu(int menu);
/* For a rebinding menu: the keyboard's own and default binding; store a new
 * own binding (written as text, empty when it equals the default). Returns 0,
 * or -1 when there is no store. */
void xbox_InputBindKeyboard(XboxBindMap *own, XboxBindMap *def);
int  xbox_InputBindStoreKeyboard(const XboxBindMap *own);
/* The same for the pad on a port: its GUID (33 bytes) and bindings; returns
 * 0 when no host pad is on the port. Storing applies to every port with a pad
 * of that GUID (identical pads share one binding). */
int  xbox_InputBindPad(int port, char guid[33], XboxBindMap *own, XboxBindMap *def);
int  xbox_InputBindStorePad(int port, const XboxBindMap *own);
/* The host pad's name on a port ("" without one). */
void xbox_InputPadName(int port, char *buf, size_t size);
/* The port the keyboard plays on (RECOMP_KEYBOARD_PORT, default 0), -1 when
 * the keyboard is off. */
int  xbox_InputKeyboardPort(void);
/* Recording a press: the first source held now (lowest number; 0 = none) of
 * the keyboard / of the pad on a port. Pad sources are buttons and triggers
 * (a trigger past half). */
uint16_t xbox_InputBindHeldKeyboard(void);
uint16_t xbox_InputBindHeldPad(int port);
/* While on, every port reads as nothing pressed (the title's menus do not see
 * the press being recorded). */
void xbox_InputBindSuppress(int on);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_XINPUT_BIND_H */
