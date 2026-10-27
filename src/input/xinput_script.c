/*
 * xinput_script.c -- scripted and synthetic pad input, for runs with nobody at
 * the controls (moved here from src/usb/usb_gamepad.c so the USB pad model and
 * the XAPI HLE layer drive the title from the same script).
 *
 * Applied on top of the host state in xbox_InputRead (xinput_device.c).
 */
#include "xinput_xbox.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USB_GAMEPAD_MAX XBOX_MAX_CONTROLLERS

/* A synthetic press, for bringing a title up without a pad on the desk.
 *
 * RECOMP_PAD_PRESS=0x10 holds Start for a quarter of a second every two
 * seconds. A title sitting on a "press Start" screen needs an edge, not a
 * level, so this pulses rather than latching -- and it repeats because the
 * moment the title starts reading input is not knowable from here.
 *
 * Bit values are the Xbox digital button mask: 0x01/02/04/08 dpad
 * up/down/left/right, 0x10 Start, 0x20 Back, 0x40/0x80 thumb clicks.
 *
 * This is a bring-up probe and nothing else. It is off unless the variable
 * is set, and a real pad on the host is always the better input.
 */
static uint8_t synthetic_buttons(void)
{
    static int      configured = -1;
    static unsigned mask, period_ms, hold_ms;
    static unsigned long t0;
    unsigned long now, phase;

    if (configured < 0) {
        const char *spec = getenv("RECOMP_PAD_PRESS");
        configured = 0;
        if (spec && *spec) {
            char *end;
            mask = (unsigned)strtoul(spec, &end, 0) & 0xFFu;
            period_ms = (*end == ',') ? (unsigned)strtoul(end + 1, &end, 0)
                                      : 2000u;
            hold_ms   = (*end == ',') ? (unsigned)strtoul(end + 1, &end, 0)
                                      : 250u;
            if (!period_ms) period_ms = 2000u;
            if (!hold_ms || hold_ms >= period_ms) hold_ms = period_ms / 4u;
            if (mask) {
                configured = 1;
                t0 = (unsigned long)GetTickCount();
                fprintf(stderr, "  PAD: synthesising button mask 0x%02X for "
                        "%u ms every %u ms\n", mask, hold_ms, period_ms);
                fflush(stderr);
            }
        }
    }
    if (!configured)
        return 0;
    now = (unsigned long)GetTickCount();
    phase = (now - t0) % period_ms;
    return (uint8_t)((phase < hold_ms) ? mask : 0u);
}

/* A scripted pad, for driving a title through its menus with nobody at it.
 *
 * RECOMP_PAD_PRESS pulses one digital mask forever, which gets past a
 * "press Start" screen and no further: a menu wants A, which is an analog
 * button on this pad, and it wants presses in order. This takes a timeline:
 *
 *   RECOMP_PAD_SCRIPT=40000:start,44000:a,46000:down,47000:a:400
 *
 * Each entry is <ms>:<button>[+<button>...][:<hold ms>], with times measured
 * from the first report the title asks for (so from enumeration, not from
 * process start) and a default hold of 150 ms. RECOMP_PAD_SCRIPT=@file reads
 * the same syntax from a file, one entry per line or comma separated; a '#'
 * where an entry would start comments out the rest of the line.
 *
 * "@include <file> <ms>" on a line of its own reads another script there, its
 * times shifted by <ms> (a path relative to the including file): reusable
 * snippets such as a cheat code entered from a menu.
 *
 * Buttons: up down left right start back lthumb rthumb (the digital byte),
 * a b x y black white lt rt (the analog bytes, reported fully pressed), and
 * lleft lright lup ldown / rleft rright rup rdown (a stick at full throw),
 * and "connect" (nothing pressed). Every controller the script names counts
 * as connected from the start (xbox_InputScriptPorts): "0:p2-connect" is a
 * second player for split-screen tests. Every entry is printed as it fires,
 * so a run log says what was pressed when.
 *
 * RECOMP_PAD_LIVE=<file> is the same thing driven while the title runs: each
 * line appended to the file -- <button>[+<button>...][:<hold ms>], no time --
 * is pressed as soon as it is read. With RECOMP_FB_DUMP and a short
 * RECOMP_PB_REPORT_MS that makes a loop: look at the frame, append a press,
 * look again. A fixed timeline cannot do that on a rasterised run, where
 * how long a screen takes to appear depends on how much the software
 * rasteriser has to draw.
 *
 * Like RECOMP_PAD_PRESS this is a bring-up tool, off unless set. */
#define PAD_SCRIPT_MAX 4096   /* long soak runs (10 min of looped input) */

typedef struct {
    unsigned long at_ms, hold_ms;
    uint8_t digital;         /* report byte 2 */
    uint8_t analog;          /* bit n = report byte 4 + n */
    uint8_t pad;             /* which controller: "p2-a" is pad 1 */
    uint8_t stick;           /* full deflection: 1 L-left 2 L-right 4 L-up
                                8 L-down, then the same four for the right */
    int announced;
    char shot[32];           /* "shot[=name]": a screenshot, no buttons */
    uint16_t key;            /* "key=<HID usage ID>": a physical key held (through
                                the keyboard's binding, xinput_bind.h) */
} PadStep;

static PadStep s_script[PAD_SCRIPT_MAX];
static int     s_script_len = -1;
static int     s_has_keys;   /* the script holds key steps */

static int same_word(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    return 1;
}

static int pad_button_bits(const char *name, size_t n, uint8_t *dig,
                           uint8_t *ana, uint8_t *stick)
{
    /* Stick directions, for screens and games that read the stick and
     * ignore the d-pad: Burnout 3 steers and picks map locations with it. */
    static const struct { const char *name; uint8_t bit; } sk[] = {
        {"lleft", 0x01}, {"lright", 0x02}, {"lup", 0x04}, {"ldown", 0x08},
        {"rleft", 0x10}, {"rright", 0x20}, {"rup", 0x40}, {"rdown", 0x80},
    };
    static const struct { const char *name; uint8_t dig, ana; } k[] = {
        {"up", 0x01, 0}, {"down", 0x02, 0}, {"left", 0x04, 0},
        {"right", 0x08, 0}, {"start", 0x10, 0}, {"back", 0x20, 0},
        {"lthumb", 0x40, 0}, {"rthumb", 0x80, 0},
        {"a", 0, 0x01}, {"b", 0, 0x02}, {"x", 0, 0x04}, {"y", 0, 0x08},
        {"black", 0, 0x10}, {"white", 0, 0x20}, {"lt", 0, 0x40}, {"rt", 0, 0x80},
    };
    size_t i;
    if (n == 7 && same_word("connect", name, n))
        return 1;
    for (i = 0; i < sizeof sk / sizeof sk[0]; i++) {
        if (strlen(sk[i].name) == n && same_word(sk[i].name, name, n)) {
            *stick |= sk[i].bit;
            return 1;
        }
    }
    for (i = 0; i < sizeof k / sizeof k[0]; i++) {
        if (strlen(k[i].name) == n && same_word(k[i].name, name, n)) {
            *dig |= k[i].dig;
            *ana |= k[i].ana;
            return 1;
        }
    }
    return 0;
}

static void pad_script_file(const char *path, unsigned long base, int depth);

static void pad_script_parse(const char *text, unsigned long base, const char *dir, int depth)
{
    const char *p = text;

    while (*p && s_script_len < PAD_SCRIPT_MAX) {
        PadStep st;
        char *end;
        const char *b;

        while (*p == ',' || *p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')
            p++;
        if (*p == '#') {             /* comment to the end of the line */
            while (*p && *p != '\n')
                p++;
            continue;
        }
        if (!*p)
            break;
        if (!strncmp(p, "@include", 8) && (p[8] == ' ' || p[8] == '\t')) {
            char name[260], path[600];
            size_t n;
            unsigned long at;
            p += 8;
            while (*p == ' ' || *p == '\t')
                p++;
            n = strcspn(p, " \t\r\n");
            if (n >= sizeof name)
                n = sizeof name - 1;
            memcpy(name, p, n);
            name[n] = 0;
            p += n;
            at = strtoul(p, &end, 0);
            p = end;
            if (name[0] == '/' || name[0] == '\\' || (name[0] && name[1] == ':') || !dir || !*dir)
                snprintf(path, sizeof path, "%s", name);
            else
                snprintf(path, sizeof path, "%s/%s", dir, name);
            if (depth < 4)
                pad_script_file(path, base + at, depth + 1);
            else
                fprintf(stderr, "  PAD: @include nested too deep: %s\n", path);
            continue;
        }
        memset(&st, 0, sizeof st);
        st.hold_ms = 150;
        st.at_ms = base + strtoul(p, &end, 0);
        if (end == p || *end != ':') {
            fprintf(stderr, "  PAD: script entry without <ms>: at \"%.20s\"\n", p);
            break;
        }
        b = end + 1;
        /* "p2-" .. "p4-": the step is for that controller, not the first. */
        if ((b[0] == 'p' || b[0] == 'P') && b[1] >= '1' && b[1] <= '4'
            && b[2] == '-') {
            st.pad = (uint8_t)(b[1] - '1');
            b += 3;
        }
        for (;;) {
            size_t n = strcspn(b, "+:,\r\n ");
            if (n >= 4 && same_word(b, "shot", 4) && (n == 4 || b[4] == '=')) {
                if (n > 5)
                    snprintf(st.shot, sizeof st.shot, "%.*s", (int)(n - 5), b + 5);
                else
                    snprintf(st.shot, sizeof st.shot, "%05lu", st.at_ms);
            } else if (n > 4 && same_word(b, "key=", 4)) {
                unsigned long k = strtoul(b + 4, NULL, 10);
                if (k > 0 && k < 512) {
                    st.key = (uint16_t)k;
                    s_has_keys = 1;
                }
                else
                    fprintf(stderr, "  PAD: bad key \"%.*s\"\n", (int)n, b);
            } else if (!pad_button_bits(b, n, &st.digital, &st.analog, &st.stick))
                fprintf(stderr, "  PAD: unknown button \"%.*s\"\n", (int)n, b);
            b += n;
            if (*b != '+')
                break;
            b++;
        }
        if (*b == ':')
            st.hold_ms = strtoul(b + 1, (char **)&b, 0);
        s_script[s_script_len++] = st;
        p = b;
    }
}

/* A script file; its directory is where its own @include paths start. */
static void pad_script_file(const char *path, unsigned long base, int depth)
{
    extern FILE *xbox_fopen_utf8(const char *path, const char *mode);
    FILE *f = xbox_fopen_utf8(path, "rb");
    char *buf, dir[600];
    const char *slash;
    size_t n;
    if (!f) {
        fprintf(stderr, "  PAD: cannot open script %s\n", path);
        return;
    }
    buf = (char *)malloc(65536);
    if (!buf) {
        fclose(f);
        return;
    }
    n = fread(buf, 1, 65535, f);
    fclose(f);
    buf[n] = 0;
    slash = strrchr(path, '/');
    {
        const char *bs = strrchr(path, '\\');
        if (!slash || (bs && bs > slash))
            slash = bs;
    }
    if (slash && (size_t)(slash - path) < sizeof dir) {
        memcpy(dir, path, (size_t)(slash - path));
        dir[slash - path] = 0;
    } else {
        dir[0] = 0;
    }
    pad_script_parse(buf, base, dir, depth);
    free(buf);
}

static void pad_script_load(void)
{
    extern const char *xbox_getenv_utf8(const char *name);
    extern FILE *xbox_fopen_utf8(const char *path, const char *mode);
    const char *spec = xbox_getenv_utf8("RECOMP_PAD_SCRIPT");

    s_script_len = 0;
    if (!spec || !*spec)
        return;
    if (spec[0] == '@')
        pad_script_file(spec + 1, 0, 0);
    else
        pad_script_parse(spec, 0, NULL, 0);
    fprintf(stderr, "  PAD: script of %d step(s)\n", s_script_len);
    fflush(stderr);
}

/* Drop steps that have finished, so a long live session never fills up. */
static void pad_script_compact(unsigned long t)
{
    int i, n = 0;
    for (i = 0; i < s_script_len; i++)
        if (t < s_script[i].at_ms + s_script[i].hold_ms)
            s_script[n++] = s_script[i];
    s_script_len = n;
}

/* Read whatever has been appended to RECOMP_PAD_LIVE since last time, and
 * schedule each complete line at t. Polled at most every 50 ms. */
static void pad_live_poll(unsigned long t)
{
    static const char *path;
    static int checked;
    static long consumed;
    static unsigned long last_poll;
    static char pending[512];
    static size_t npending;
    static unsigned long next_free[USB_GAMEPAD_MAX];
    FILE *f;
    int c;

    if (!checked) {
        checked = 1;
        extern const char *xbox_getenv_utf8(const char *name);
        path = xbox_getenv_utf8("RECOMP_PAD_LIVE");
        if (path && *path) {
            fprintf(stderr, "  PAD: live input from %s\n", path);
            fflush(stderr);
        }
    }
    if (!path || !*path || (t - last_poll < 50 && last_poll))
        return;
    last_poll = t;
    {
        extern FILE *xbox_fopen_utf8(const char *path, const char *mode);
        f = xbox_fopen_utf8(path, "rb");
    }
    if (!f)
        return;
    if (fseek(f, consumed, SEEK_SET) != 0) {
        fclose(f);
        return;
    }
    while ((c = fgetc(f)) != EOF) {
        consumed++;
        if (c == '\n' || c == '\r') {
            if (npending) {
                char line[560];
                /* Several lines in one read go one after another, with a
                 * 200 ms release between: a title polling its pad a few times
                 * a second would otherwise see "down" and "a" in the same
                 * report, which is a different input from "down, then a". */
                /* Serialised per pad: pad 2's throttle must not wait out
                 * pad 1's -- split-screen players press at the same time. */
                int pd = (pending[0] == 'p' || pending[0] == 'P')
                         && pending[1] >= '1' && pending[1] <= '4'
                         && pending[2] == '-' ? pending[1] - '1' : 0;
                unsigned long at = t > next_free[pd] ? t : next_free[pd];
                pending[npending] = 0;
                snprintf(line, sizeof line, "%lu:%s", at, pending);
                if (s_script_len >= PAD_SCRIPT_MAX - 1)
                    pad_script_compact(t);
                pad_script_parse(line, 0, NULL, 0);
                if (s_script_len > 0)
                    next_free[pd] = at + s_script[s_script_len - 1].hold_ms + 200;
                fprintf(stderr, "  PAD: live \"%s\" at t=%lu ms\n", pending, at);
                fflush(stderr);
                npending = 0;
            }
        } else if (npending < sizeof pending - 1) {
            pending[npending++] = (char)c;
        }
    }
    fclose(f);
}

/* Ports the script drives ("p2-a" is port 1), so a scripted second player is
 * a connected controller without a pad on the desk. */
static unsigned s_script_ports;

static void script_ports_update(void)
{
    int i;
    for (i = 0; i < s_script_len; i++)
        s_script_ports |= 1u << s_script[i].pad;
}

static unsigned long s_t0;   /* the script's clock: the first pad read */

int xbox_InputScriptKeyDown(int scancode)
{
    unsigned long t;
    int i;

    if (!s_has_keys || s_script_len <= 0 || !s_t0)
        return 0;
    t = (unsigned long)GetTickCount() - s_t0;
    for (i = 0; i < s_script_len; i++) {
        PadStep *st = &s_script[i];
        if (st->key == scancode && t >= st->at_ms && t < st->at_ms + st->hold_ms) {
            if (!st->announced) {
                st->announced = 1;
                fprintf(stderr, "  PAD: t=%lu ms step %d (key %u)\n", t, i, (unsigned)st->key);
                fflush(stderr);
            }
            return 1;
        }
    }
    return 0;
}

static void pad_script_apply(int pad, XBOX_GAMEPAD *g)
{
    unsigned long now, t;
    int i, j;

    if (s_script_len < 0)
        pad_script_load();
    now = (unsigned long)GetTickCount();
    if (!s_t0)
        s_t0 = now;
    t = now - s_t0;
    pad_live_poll(t);
    script_ports_update();
    for (i = 0; i < s_script_len; i++) {
        PadStep *st = &s_script[i];
        if (st->pad != pad || st->key)   /* key steps: xbox_InputScriptKeyDown */
            continue;
        if (t < st->at_ms || t >= st->at_ms + st->hold_ms)
            continue;
        if (!st->announced) {
            st->announced = 1;
            fprintf(stderr, "  PAD: t=%lu ms step %d (digital 0x%02X analog 0x%02X)%s%s\n",
                    t, i, st->digital, st->analog, st->shot[0] ? " shot " : "", st->shot);
            fflush(stderr);
            if (st->shot[0]) {
                extern void (*g_xbox_screenshot_hook)(const char *name);
                if (g_xbox_screenshot_hook)
                    g_xbox_screenshot_hook(st->shot);
            }
        }
        g->wButtons |= st->digital;
        for (j = 0; j < 8; j++)
            if (st->analog & (1u << j))
                g->bAnalogButtons[j] = 0xFF;
        /* Sticks at full throw, up positive. */
        if (st->stick & 0x01) g->sThumbLX = -32767;
        if (st->stick & 0x02) g->sThumbLX = 32767;
        if (st->stick & 0x04) g->sThumbLY = 32767;
        if (st->stick & 0x08) g->sThumbLY = -32767;
        if (st->stick & 0x10) g->sThumbRX = -32767;
        if (st->stick & 0x20) g->sThumbRX = 32767;
        if (st->stick & 0x40) g->sThumbRY = 32767;
        if (st->stick & 0x80) g->sThumbRY = -32767;
    }
}

void xbox_InputScriptApply(int port, XBOX_GAMEPAD *g)
{
    if (port == 0)
        g->wButtons |= synthetic_buttons();
    pad_script_apply(port, g);
}

unsigned xbox_InputScriptPorts(void)
{
    if (s_script_len < 0)
        pad_script_load();
    script_ports_update();
    return s_script_ports;
}
