/* input_bind: bindings per device (src/input/xinput_bind.c) -- defaults, text
 * form, duplicates and the lock-out rule (the own binding wins; in menus a
 * default source acts only when the own binding does not use it). */
#include <stdio.h>
#include <string.h>
#include "xinput_bind.h"

static int s_fail, s_checks;

static void check(int ok, const char *what)
{
    s_checks++;
    if (!ok) {
        printf("FAIL  %s\n", what);
        s_fail++;
    }
}

/* Keys held in this test: a small set of HID usage IDs. */
static uint16_t s_held[8];
static uint8_t key_value(void *ctx, uint16_t src)
{
    int i;
    (void)ctx;
    for (i = 0; i < 8; i++)
        if (s_held[i] && s_held[i] == src)
            return 255;
    return 0;
}

static void hold(uint16_t a, uint16_t b)
{
    memset(s_held, 0, sizeof s_held);
    s_held[0] = a;
    s_held[1] = b;
}

/* THPS 2X's layout (src/input.c): K = A, L = B, I = Y, J = X, Enter and keypad
 * Enter = Start, W A S D left stick. */
enum { K = 14, L = 15, I = 12, J = 13, P = 19, ENTER = 40, KP_ENTER = 88, W = 26, S_ = 22 };
static const XBOX_KEY_BINDING s_keymap[] = {
    { W, XBOX_KEY_LY_POS }, { S_, XBOX_KEY_LY_NEG },
    { ENTER, XBOX_KEY_START }, { KP_ENTER, XBOX_KEY_START },
    { I, XBOX_KEY_Y }, { J, XBOX_KEY_X }, { K, XBOX_KEY_A }, { L, XBOX_KEY_B },
};

int main(void)
{
    XboxBindMap def, own, back, pdef, pown;
    uint8_t v[XBOX_KEY_CONTROL_COUNT];
    char buf[512];

    xbox_bind_from_keymap(&def, s_keymap, (int)(sizeof s_keymap / sizeof s_keymap[0]));
    check(def.src[XBOX_KEY_START][0] == ENTER && def.src[XBOX_KEY_START][1] == KP_ENTER, "two keys on Start");

    /* default: empty text, and the empty text gives the default */
    check(xbox_bind_format(XBOX_BIND_KEYBOARD, &def, &def, buf, sizeof buf) == 0 && !buf[0], "default formats empty");
    check(xbox_bind_parse(XBOX_BIND_KEYBOARD, "", &def, &own) == 0 && !memcmp(&own, &def, sizeof own), "empty = default");
    check(xbox_bind_parse(XBOX_BIND_KEYBOARD, NULL, &def, &own) == 0 && !memcmp(&own, &def, sizeof own), "NULL = default");

    /* errors leave out untouched */
    own = def;
    check(xbox_bind_parse(XBOX_BIND_KEYBOARD, "a:sc15,zz:sc1", &def, &own) == -1, "unknown control refused");
    check(xbox_bind_parse(XBOX_BIND_KEYBOARD, "a:south", &def, &own) == -1, "pad source on the keyboard refused");
    check(xbox_bind_parse(XBOX_BIND_KEYBOARD, "a:sc999", &def, &own) == -1, "scancode out of range refused");
    check(xbox_bind_parse(XBOX_BIND_PAD, "lup:south", &def, &own) == -1, "pad sticks are not bindable");
    check(!memcmp(&own, &def, sizeof own), "failed parse keeps the map");

    /* The maintainer's case: A rebound to the default B key (L), B moved to P.
     * In a menu L is A only -- the default L->B does not act, because the own
     * binding uses L. */
    check(xbox_bind_parse(XBOX_BIND_KEYBOARD, "a:sc15,b:sc19", &def, &own) == 0, "parse a:sc15,b:sc19");
    hold(L, 0);
    xbox_bind_eval(XBOX_BIND_KEYBOARD, &own, &def, 1, key_value, NULL, v);
    check(v[XBOX_KEY_A] == 255 && v[XBOX_KEY_B] == 0, "menu: L (default B key) bound to A triggers A only");
    xbox_bind_eval(XBOX_BIND_KEYBOARD, &own, &def, 0, key_value, NULL, v);
    check(v[XBOX_KEY_A] == 255 && v[XBOX_KEY_B] == 0, "gameplay: L triggers A only");
    /* the default A key K is no longer used by the own binding: it still
     * confirms in menus (no lock-out), not in gameplay */
    hold(K, 0);
    xbox_bind_eval(XBOX_BIND_KEYBOARD, &own, &def, 1, key_value, NULL, v);
    check(v[XBOX_KEY_A] == 255, "menu: old default A key K still confirms");
    xbox_bind_eval(XBOX_BIND_KEYBOARD, &own, &def, 0, key_value, NULL, v);
    check(v[XBOX_KEY_A] == 0, "gameplay: old default A key K does nothing");
    hold(P, 0);
    xbox_bind_eval(XBOX_BIND_KEYBOARD, &own, &def, 1, key_value, NULL, v);
    check(v[XBOX_KEY_B] == 255 && v[XBOX_KEY_A] == 0 && v[XBOX_KEY_RTRIGGER] == 0, "P is B now");

    /* swap A and B: no double action either way, in menus too */
    check(xbox_bind_parse(XBOX_BIND_KEYBOARD, "a:sc15,b:sc14", &def, &own) == 0, "parse swap");
    hold(L, 0);
    xbox_bind_eval(XBOX_BIND_KEYBOARD, &own, &def, 1, key_value, NULL, v);
    check(v[XBOX_KEY_A] == 255 && v[XBOX_KEY_B] == 0, "swap, menu: L = A only");
    hold(K, 0);
    xbox_bind_eval(XBOX_BIND_KEYBOARD, &own, &def, 1, key_value, NULL, v);
    check(v[XBOX_KEY_B] == 255 && v[XBOX_KEY_A] == 0, "swap, menu: K = B only");

    /* everything unbound: the default layout still navigates menus, gameplay
     * gets nothing */
    {
        XboxBindMap none;
        xbox_bind_clear(&none);
        hold(ENTER, 0);
        xbox_bind_eval(XBOX_BIND_KEYBOARD, &none, &def, 1, key_value, NULL, v);
        check(v[XBOX_KEY_START] == 255, "nothing bound, menu: Enter = Start");
        xbox_bind_eval(XBOX_BIND_KEYBOARD, &none, &def, 0, key_value, NULL, v);
        check(v[XBOX_KEY_START] == 0, "nothing bound, gameplay: nothing");
    }

    /* duplicates: A moved onto L while B stays on L */
    check(xbox_bind_parse(XBOX_BIND_KEYBOARD, "a:sc15", &def, &own) == 0, "parse a:sc15");
    check(xbox_bind_duplicates(XBOX_BIND_KEYBOARD, &own) == ((1u << XBOX_KEY_A) | (1u << XBOX_KEY_B)), "A and B marked double");
    check(xbox_bind_duplicates(XBOX_BIND_KEYBOARD, &def) == 0, "default has no duplicates");

    /* text round trip, two sources and an unbound control */
    check(xbox_bind_parse(XBOX_BIND_KEYBOARD, "start:sc44+sc40,y:none,lup:sc82", &def, &own) == 0, "parse multi");
    check(own.src[XBOX_KEY_Y][0] == 0 && own.src[XBOX_KEY_Y][1] == 0, "y:none unbinds");
    check(xbox_bind_format(XBOX_BIND_KEYBOARD, &own, &def, buf, sizeof buf) > 0, "format");
    check(xbox_bind_parse(XBOX_BIND_KEYBOARD, buf, &def, &back) == 0 && !memcmp(&own, &back, sizeof own), "round trip");
    check(!strcmp(buf, "start:sc44+sc40,y:none,lup:sc82"), "only changed controls written");
    check(xbox_bind_format(XBOX_BIND_KEYBOARD, &own, &def, buf, 8) == -1, "small buffer refused");

    /* set one control */
    own = def;
    xbox_bind_set(&own, XBOX_KEY_START, 44);
    check(own.src[XBOX_KEY_START][0] == 44 && own.src[XBOX_KEY_START][1] == 0, "set replaces both sources");
    check(xbox_bind_uses(XBOX_BIND_KEYBOARD, &own, 44) && !xbox_bind_uses(XBOX_BIND_KEYBOARD, &own, KP_ENTER), "uses");

    /* pads: positional default, A/B swap without double action */
    xbox_bind_pad_default(&pdef);
    check(pdef.src[XBOX_KEY_A][0] == XBOX_PAD_SRC_SOUTH && pdef.src[XBOX_KEY_BLACK][0] == XBOX_PAD_SRC_LSHOULDER &&
          pdef.src[XBOX_KEY_LTRIGGER][0] == XBOX_PAD_SRC_LTRIGGER, "pad default positional");
    check(xbox_bind_format(XBOX_BIND_PAD, &pdef, &pdef, buf, sizeof buf) == 0, "pad default formats empty");
    check(xbox_bind_parse(XBOX_BIND_PAD, "a:east,b:south", &pdef, &pown) == 0, "pad parse swap");
    hold(XBOX_PAD_SRC_EAST, 0);
    xbox_bind_eval(XBOX_BIND_PAD, &pown, &pdef, 1, key_value, NULL, v);
    check(v[XBOX_KEY_A] == 255 && v[XBOX_KEY_B] == 0, "pad swap, menu: east = A only");
    check(xbox_bind_format(XBOX_BIND_PAD, &pown, &pdef, buf, sizeof buf) > 0 && !strcmp(buf, "a:east,b:south"), "pad format");
    {
        char t[16];
        check(!strcmp(xbox_bind_source_name(XBOX_BIND_PAD, XBOX_PAD_SRC_RTRIGGER, t, sizeof t), "rtrigger") &&
              !strcmp(xbox_bind_source_name(XBOX_BIND_KEYBOARD, 40, t, sizeof t), "sc40") &&
              !strcmp(xbox_bind_control_name(XBOX_KEY_LY_POS), "lup"), "names");
    }

    printf("%d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
