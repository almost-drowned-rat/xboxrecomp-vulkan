/*
 * xinput_bind.c - button bindings per input device: defaults, text form,
 * duplicates and the lock-out rule (see xinput_bind.h). Pure logic, no SDL.
 */
#include "xinput_bind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const s_control_name[XBOX_KEY_CONTROL_COUNT] = {
    "up", "down", "left", "right", "start", "back", "lthumb", "rthumb",
    "a", "b", "x", "y", "black", "white", "lt", "rt",
    "lleft", "lright", "ldown", "lup", "rleft", "rright", "rdown", "rup",
};

static const char *const s_pad_src_name[XBOX_PAD_SRC_COUNT] = {
    "none", "south", "east", "west", "north", "back", "start", "lstick", "rstick",
    "lshoulder", "rshoulder", "dup", "ddown", "dleft", "dright", "ltrigger", "rtrigger",
};

#define KEY_SRC_MAX 512   /* HID usage IDs below this (xinput_device.c KEY_SLOTS) */

int xbox_bind_controls(XboxBindKind kind)
{
    return kind == XBOX_BIND_PAD ? XBOX_BIND_PAD_CONTROLS : XBOX_KEY_CONTROL_COUNT;
}

void xbox_bind_clear(XboxBindMap *m)
{
    memset(m, 0, sizeof *m);
}

static void add_src(XboxBindMap *m, int control, uint16_t src)
{
    int i;
    for (i = 0; i < XBOX_BIND_SRC_MAX; i++) {
        if (m->src[control][i] == src)
            return;
        if (!m->src[control][i]) {
            m->src[control][i] = src;
            return;
        }
    }
}

void xbox_bind_pad_default(XboxBindMap *m)
{
    static const uint16_t def[XBOX_BIND_PAD_CONTROLS] = {
        XBOX_PAD_SRC_DUP, XBOX_PAD_SRC_DDOWN, XBOX_PAD_SRC_DLEFT, XBOX_PAD_SRC_DRIGHT,
        XBOX_PAD_SRC_START, XBOX_PAD_SRC_BACK, XBOX_PAD_SRC_LSTICK, XBOX_PAD_SRC_RSTICK,
        XBOX_PAD_SRC_SOUTH, XBOX_PAD_SRC_EAST, XBOX_PAD_SRC_WEST, XBOX_PAD_SRC_NORTH,
        XBOX_PAD_SRC_LSHOULDER, XBOX_PAD_SRC_RSHOULDER, XBOX_PAD_SRC_LTRIGGER, XBOX_PAD_SRC_RTRIGGER,
    };
    int c;
    xbox_bind_clear(m);
    for (c = 0; c < XBOX_BIND_PAD_CONTROLS; c++)
        m->src[c][0] = def[c];
}

void xbox_bind_from_keymap(XboxBindMap *m, const XBOX_KEY_BINDING *map, int count)
{
    int i;
    xbox_bind_clear(m);
    for (i = 0; map && i < count; i++)
        if (map[i].control < XBOX_KEY_CONTROL_COUNT && map[i].scancode && map[i].scancode < KEY_SRC_MAX)
            add_src(m, map[i].control, map[i].scancode);
}

const char *xbox_bind_control_name(int control)
{
    return control >= 0 && control < XBOX_KEY_CONTROL_COUNT ? s_control_name[control] : "?";
}

const char *xbox_bind_source_name(XboxBindKind kind, uint16_t src, char *buf, size_t size)
{
    if (!src)
        return "none";
    if (kind == XBOX_BIND_PAD)
        return src < XBOX_PAD_SRC_COUNT ? s_pad_src_name[src] : "?";
    snprintf(buf, size, "sc%u", (unsigned)src);
    return buf;
}

static int find_control(const char *s, size_t n, int controls)
{
    int c;
    for (c = 0; c < controls; c++)
        if (strlen(s_control_name[c]) == n && !strncmp(s, s_control_name[c], n))
            return c;
    return -1;
}

/* 0 = "none", >0 a source, -1 unknown. */
static int find_source(XboxBindKind kind, const char *s, size_t n)
{
    if (n == 4 && !strncmp(s, "none", 4))
        return 0;
    if (kind == XBOX_BIND_PAD) {
        int i;
        for (i = 1; i < XBOX_PAD_SRC_COUNT; i++)
            if (strlen(s_pad_src_name[i]) == n && !strncmp(s, s_pad_src_name[i], n))
                return i;
        return -1;
    }
    if (n > 2 && (s[0] == 's' || s[0] == 'S') && (s[1] == 'c' || s[1] == 'C')) {
        long v = 0;
        size_t i;
        for (i = 2; i < n; i++) {
            if (s[i] < '0' || s[i] > '9')
                return -1;
            v = v * 10 + (s[i] - '0');
            if (v >= KEY_SRC_MAX)
                return -1;
        }
        return v > 0 ? (int)v : -1;
    }
    return -1;
}

int xbox_bind_parse(XboxBindKind kind, const char *text, const XboxBindMap *def, XboxBindMap *out)
{
    XboxBindMap m = *def;
    const char *p = text;
    int controls = xbox_bind_controls(kind);

    while (p && *p) {
        const char *end, *colon;
        int c;
        while (*p == ' ' || *p == ',')
            p++;
        if (!*p)
            break;
        end = strchr(p, ',');
        if (!end)
            end = p + strlen(p);
        colon = memchr(p, ':', (size_t)(end - p));
        if (!colon || (c = find_control(p, (size_t)(colon - p), controls)) < 0)
            return -1;
        memset(m.src[c], 0, sizeof m.src[c]);
        p = colon + 1;
        while (p < end) {
            const char *plus = memchr(p, '+', (size_t)(end - p));
            const char *stop = plus ? plus : end;
            int s = find_source(kind, p, (size_t)(stop - p));
            if (s < 0)
                return -1;
            if (s)
                add_src(&m, c, (uint16_t)s);
            p = plus ? plus + 1 : end;
        }
        p = end;
    }
    *out = m;
    return 0;
}

static int same_control(const XboxBindMap *a, const XboxBindMap *b, int c)
{
    int i, j;
    for (i = 0; i < XBOX_BIND_SRC_MAX; i++) {
        int found = !a->src[c][i];
        for (j = 0; j < XBOX_BIND_SRC_MAX && !found; j++)
            found = a->src[c][i] == b->src[c][j];
        if (!found)
            return 0;
    }
    for (i = 0; i < XBOX_BIND_SRC_MAX; i++) {
        int found = !b->src[c][i];
        for (j = 0; j < XBOX_BIND_SRC_MAX && !found; j++)
            found = b->src[c][i] == a->src[c][j];
        if (!found)
            return 0;
    }
    return 1;
}

int xbox_bind_format(XboxBindKind kind, const XboxBindMap *m, const XboxBindMap *def, char *buf, size_t size)
{
    size_t at = 0;
    int c, controls = xbox_bind_controls(kind);

    if (!size)
        return -1;
    buf[0] = 0;
    for (c = 0; c < controls; c++) {
        char tmp[16];
        int i, any = 0, n;
        if (same_control(m, def, c))
            continue;
        n = snprintf(buf + at, size - at, "%s%s:", at ? "," : "", s_control_name[c]);
        if (n < 0 || (size_t)n >= size - at)
            return -1;
        at += (size_t)n;
        for (i = 0; i < XBOX_BIND_SRC_MAX; i++) {
            if (!m->src[c][i])
                continue;
            n = snprintf(buf + at, size - at, "%s%s", any ? "+" : "",
                         xbox_bind_source_name(kind, m->src[c][i], tmp, sizeof tmp));
            if (n < 0 || (size_t)n >= size - at)
                return -1;
            at += (size_t)n;
            any = 1;
        }
        if (!any) {
            n = snprintf(buf + at, size - at, "none");
            if (n < 0 || (size_t)n >= size - at)
                return -1;
            at += (size_t)n;
        }
    }
    return (int)at;
}

int xbox_bind_uses(XboxBindKind kind, const XboxBindMap *m, uint16_t src)
{
    int c, i, controls = xbox_bind_controls(kind);
    if (!src)
        return 0;
    for (c = 0; c < controls; c++)
        for (i = 0; i < XBOX_BIND_SRC_MAX; i++)
            if (m->src[c][i] == src)
                return 1;
    return 0;
}

uint32_t xbox_bind_duplicates(XboxBindKind kind, const XboxBindMap *m)
{
    uint32_t dup = 0;
    int c, d, i, j, controls = xbox_bind_controls(kind);
    for (c = 0; c < controls; c++)
        for (d = c + 1; d < controls; d++)
            for (i = 0; i < XBOX_BIND_SRC_MAX; i++)
                for (j = 0; j < XBOX_BIND_SRC_MAX; j++)
                    if (m->src[c][i] && m->src[c][i] == m->src[d][j])
                        dup |= (1u << c) | (1u << d);
    return dup;
}

void xbox_bind_set(XboxBindMap *m, int control, uint16_t src)
{
    if (control < 0 || control >= XBOX_KEY_CONTROL_COUNT)
        return;
    memset(m->src[control], 0, sizeof m->src[control]);
    m->src[control][0] = src;
}

void xbox_bind_eval(XboxBindKind kind, const XboxBindMap *own, const XboxBindMap *def, int menu,
                    uint8_t (*value)(void *ctx, uint16_t src), void *ctx,
                    uint8_t out[XBOX_KEY_CONTROL_COUNT])
{
    int c, i, controls = xbox_bind_controls(kind);

    memset(out, 0, XBOX_KEY_CONTROL_COUNT);
    for (c = 0; c < controls; c++) {
        for (i = 0; i < XBOX_BIND_SRC_MAX; i++) {
            uint16_t s = own->src[c][i];
            uint8_t v;
            if (s && (v = value(ctx, s)) > out[c])
                out[c] = v;
        }
        if (!menu || !def)
            continue;
        /* Lock-out rule: a default source the own binding does not use. */
        for (i = 0; i < XBOX_BIND_SRC_MAX; i++) {
            uint16_t s = def->src[c][i];
            uint8_t v;
            if (s && !xbox_bind_uses(kind, own, s) && (v = value(ctx, s)) > out[c])
                out[c] = v;
        }
    }
}
