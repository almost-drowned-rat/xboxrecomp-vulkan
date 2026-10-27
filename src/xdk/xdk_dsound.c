/*
 * xdk_dsound.c -- DirectSound entry points at API level, mixed with SDL3
 * (see xdk_dsound.h).
 *
 * Reference design: XboxXBE-Recomp runtime/xdk/dsound.cpp (TGP482, MIT,
 * commit 65e959d4a5fd0c171dc7b17f3f9b44bab00928e6): buffers that play
 * straight out of title memory, streams as packet queues completed on the
 * title's side, one host mixer. Rewritten in C for this runtime.
 *
 * Layouts (DSOUND 3925, read from the THPS 2X code):
 *   DSBUFFERDESC   +0 dwSize, +4 dwFlags, +8 dwBufferBytes, +0xC lpwfxFormat
 *   DSSTREAMDESC   +0 dwFlags, +4 dwMaxAttachedPackets, +8 lpwfxFormat,
 *                  +0xC lpfnCallback, +0x10 lpvContext
 *   WAVEFORMATEX   +0 wFormatTag, +2 nChannels, +4 nSamplesPerSec,
 *                  +8 nAvgBytesPerSec, +0xC nBlockAlign, +0xE wBitsPerSample
 *   XMEDIAPACKET   +0 pvBuffer, +4 dwMaxSize, +8 pdwCompletedSize,
 *                  +0xC pdwStatus, +0x10 hCompletionEvent / pContext
 *   callback       stdcall (pStreamContext, pPacketContext, dwStatus)
 *
 * Every object the title gets is a small block of guest memory: a stream
 * starts with the title's CDirectSoundStream vtables (the title calls
 * Process/GetStatus through them, and those methods are bound here too);
 * every other object is only ever passed back to a bound entry point. A
 * magic and an index at +0x10 lead back to the host voice.
 */
#include "xdk_dsound.h"

#include "kernel.h"
#include "xbox_memory_layout.h"
#include "xbox_adpcm.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);

/* ---- guest memory ------------------------------------------------------ */

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

#define ARG(n) r32(g_esp + 4u + 4u * (n))
#define RETURN(v, nargs) do { g_eax = (uint32_t)(v); g_esp += 4u + 4u * (nargs); return; } while (0)

#define DS_OK               0u
#define DSERR_INVALIDPARAM  0x80070057u
#define DSERR_OUTOFMEMORY   0x8007000Eu
#define E_FAIL_             0x80004005u
#define PKT_SUCCESS         0u
#define PKT_PENDING         0x8000000Au
#define PKT_FLUSHED         0x8000000Bu
#define DSBSTATUS_PLAYING   1u
#define DSBSTATUS_LOOPING   4u
#define DSBPLAY_LOOPING     1u
#define DSBLOCK_FROMWRITECURSOR 1u
#define DSBLOCK_ENTIREBUFFER    2u
#define XMO_STATUSF_ACCEPT_INPUT_DATA 1u
#define FMT_PCM             1
#define FMT_XBOX_ADPCM      0x69

#define OBJ_MAGIC   0x58445356u     /* "VSDX" */
#define OBJ_SIZE    0x20u           /* +0 vtable, +4 vtable2, +0x10 magic, +0x14 index */
#define MAX_VOICES  256
#define MAX_PACKETS 64
#define OUT_RATE    48000

/* ---- voices -------------------------------------------------------------- */

/* XMEDIAPACKET fields are copied at Process, as the XDK (and Cxbx-Reloaded's
 * DirectSoundStream.cpp, xmp_data) does: titles may reuse or free their packet
 * struct -- often a stack local -- once Process returns. Reading it again at
 * completion wrote through stale pointers (THPS 2X, level load: write to
 * 0xFFFFFFFF in DirectSoundDoWork). */
typedef struct {
    uint32_t pkt, buf, size;
    uint32_t size_ptr, status_ptr, ev;
} Packet;

typedef struct {
    int      used, refs, stream;
    uint32_t obj;                     /* guest object */
    /* format */
    int      tag, ch, bits, rate, align;
    /* data (buffers): guest memory the title owns or we allocated */
    uint32_t data, bytes;
    uint32_t loop_start, loop_len;    /* bytes; 0 length = whole buffer */
    /* play state */
    int      playing, looping, paused;
    double   pos;                     /* frames into the data / current packet */
    uint32_t freq;                    /* 0 = the format's rate */
    float    gain;
    /* stream */
    Packet   q[MAX_PACKETS];
    int      qn;
    Packet   done[MAX_PACKETS];
    uint32_t done_status[MAX_PACKETS];
    int      dn;
    uint32_t callback, context;
    /* ADPCM: the last decoded block */
    long     blk;
    int16_t  pcm[2 * 65];
    /* debug (RECOMP_DSOUND_LOG / _VOICE_WAV): counters since the last report */
    uint32_t st_underrun, st_jump, st_gain, st_freq;
    uint32_t st_lock, st_lockhit, st_getpos, st_setdata, st_setpos, st_play;
    int32_t  st_lead_min;             /* lock offset - play byte, smallest seen (bytes) */
    int      st_starved;              /* a stream that had data and ran dry */
    float    last_l, last_r;
    FILE    *vwav;
    uint32_t vwav_bytes;
} Voice;

static Voice     s_v[MAX_VOICES];
static uint32_t  s_free_obj[MAX_VOICES];
static int       s_nfree;
static uint32_t  s_vt1, s_vt2;
static uint32_t  s_ds;               /* the one IDirectSound */
static int       s_ds_refs;
static SDL_Mutex *s_lock;
static SDL_AudioStream *s_out;
static int       s_trace = -1;
static FILE     *s_wav;
static uint32_t  s_wav_bytes;

/* ---- debug: which voice crackles --------------------------------------------
 * RECOMP_DSOUND_MUTE=<slot>[,<slot>...]  those voices are silent (still timed)
 * RECOMP_DSOUND_SOLO=<slot>[,<slot>...]  only those voices are heard
 * RECOMP_DSOUND_VOICE_WAV=<dir>          each voice that plays into <dir>\vSSS_<obj>.wav
 *                                        (48 kHz, after gain, before the clip)
 * RECOMP_DSOUND_LOG=1                    every second of output: the voices playing,
 *                                        their format, rate, gain, queue, and counts of
 *                                        stream underruns, sample jumps (> 0.25 between two
 *                                        output samples), volume and frequency changes,
 *                                        and clipped output samples
 * Slots are the voice numbers in the log. Off by default; the mix is unchanged. */
static int      s_dbg = -1;               /* any of the above set */
static uint8_t  s_mute[MAX_VOICES], s_solo[MAX_VOICES];
static int      s_any_solo, s_log;
static char     s_vwav_dir[260];
static uint64_t s_out_frames;             /* output frames mixed so far */
static uint32_t s_clips;
static int      s_chunk_min = 1 << 30, s_chunk_max; /* frames per mix pass */

static void dbg_list(const char *e, uint8_t *set)
{
    while (e && *e) {
        char *end;
        long k = strtol(e, &end, 10);
        if (end == e)
            break;
        if (k >= 0 && k < MAX_VOICES)
            set[k] = 1;
        e = *end == ',' ? end + 1 : NULL;
    }
}

static int dbg_on(void)
{
    if (s_dbg < 0) {
        const char *m = getenv("RECOMP_DSOUND_MUTE"), *so = getenv("RECOMP_DSOUND_SOLO");
        const char *w = getenv("RECOMP_DSOUND_VOICE_WAV"), *l = getenv("RECOMP_DSOUND_LOG");
        int i;
        dbg_list(m, s_mute);
        dbg_list(so, s_solo);
        for (i = 0; i < MAX_VOICES; i++)
            s_any_solo |= s_solo[i];
        if (w && *w)
            snprintf(s_vwav_dir, sizeof s_vwav_dir, "%s", w);
        s_log = l && *l == '1';
        s_dbg = (m && *m) || s_any_solo || s_vwav_dir[0] || s_log;
        if (s_dbg)
            fprintf(stderr, "  DSOUND: debug%s%s%s%s\n", m && *m ? " mute" : "", s_any_solo ? " solo" : "",
                    s_vwav_dir[0] ? " voice-wav" : "", s_log ? " log" : "");
    }
    return s_dbg;
}

static void wav_header(FILE *f)
{
    static const uint8_t hdr[44] = {
        'R','I','F','F', 36,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0,
        1,0, 2,0, 0x80,0xBB,0,0, 0x00,0xEE,0x02,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    fwrite(hdr, 1, sizeof hdr, f);
}

/* Append frames of float stereo to a WAV opened with wav_header; the header
 * is kept right as we go (a run ends by --max-seconds). */
static void wav_append(FILE *f, uint32_t *bytes, const float *x, int frames)
{
    static int16_t buf[4096 * 2];
    int i, n = frames * 2 > 8192 ? 8192 : frames * 2;
    for (i = 0; i < n; i++) {
        float v = x[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        buf[i] = (int16_t)(v * 32767.0f);
    }
    fwrite(buf, 2, (size_t)n, f);
    *bytes += (uint32_t)n * 2u;
    {
        long at = ftell(f);
        uint32_t riff = 36u + *bytes;
        fseek(f, 4, SEEK_SET);  fwrite(&riff, 4, 1, f);
        fseek(f, 40, SEEK_SET); fwrite(bytes, 4, 1, f);
        fseek(f, at, SEEK_SET);
    }
}

static void dbg_report(void)
{
    int i;
    fprintf(stderr, "[DSLOG] t=%.1f s clips %u, mix chunk %d..%d frames\n", (double)s_out_frames / OUT_RATE,
            s_clips, s_chunk_min, s_chunk_max);
    s_chunk_min = 1 << 30;
    s_chunk_max = 0;
    for (i = 0; i < MAX_VOICES; i++) {
        Voice *v = &s_v[i];
        if (!v->used || (!v->playing && !v->qn && !v->st_underrun))
            continue;
        fprintf(stderr, "[DSLOG]   v%03d %08X %s tag %d %dch %d Hz%s freq %u gain %.3f%s q %d"
                " | underrun %u jumps %u vol %u freq %u | play %u setdata %u setpos %u getpos %u lock %u"
                " lock-at-read-head %u lead-min %d%s%s\n",
                i, v->obj, v->stream ? "stream" : "buffer", v->tag, v->ch, v->rate,
                v->tag == FMT_XBOX_ADPCM ? " adpcm" : "", v->freq, v->gain,
                v->looping ? " loop" : "", v->qn, v->st_underrun, v->st_jump, v->st_gain, v->st_freq,
                v->st_play, v->st_setdata, v->st_setpos, v->st_getpos, v->st_lock, v->st_lockhit,
                v->st_lock ? v->st_lead_min : 0,
                s_mute[i] ? " MUTED" : "", s_any_solo && !s_solo[i] ? " (not solo)" : "");
        v->st_underrun = v->st_jump = v->st_gain = v->st_freq = 0;
        v->st_lock = v->st_lockhit = v->st_getpos = v->st_setdata = v->st_setpos = v->st_play = 0;
        v->st_lead_min = 1 << 30;
    }
    s_clips = 0;
    fflush(stderr);
}

static void lock(void)   { SDL_LockMutex(s_lock); }
static void unlock(void) { SDL_UnlockMutex(s_lock); }

static int trace_on(void)
{
    if (s_trace < 0) {
        const char *e = getenv("RECOMP_DSOUND_TRACE");
        s_trace = e && *e && *e != '0';
    }
    return s_trace;
}

void xdk_dsound_trace(const char *name, int nargs)
{
    int i;
    if (!trace_on())
        return;
    fprintf(stderr, "[DSTRACE] %s(", name);
    for (i = 0; i < nargs; i++)
        fprintf(stderr, "%s0x%08X", i ? ", " : "", ARG(i));
    fprintf(stderr, ") ret=0x%08X\n", r32(g_esp));
}

void xdk_dsound_bind(uint32_t stream_vtable, uint32_t stream_vtable2)
{
    s_vt1 = stream_vtable;
    s_vt2 = stream_vtable2;
}

int xdk_dsound_active(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_DSOUND_HLE");
        on = s_vt1 && !(e && *e == '0');
    }
    return on;
}

static uint32_t new_obj(int index, int stream)
{
    uint32_t o = s_nfree ? s_free_obj[--s_nfree] : xbox_HeapAlloc(OBJ_SIZE, 16);
    if (!o)
        return 0;
    memset(gp(o), 0, OBJ_SIZE);
    if (stream) {
        w32(o + 0, s_vt1);
        w32(o + 4, s_vt2);
    }
    w32(o + 0x10, OBJ_MAGIC);
    w32(o + 0x14, (uint32_t)index);
    return o;
}

static Voice *voice_of(uint32_t obj)
{
    uint32_t i;
    if (!obj || r32(obj + 0x10) != OBJ_MAGIC)
        return NULL;
    i = r32(obj + 0x14);
    return (i < MAX_VOICES && s_v[i].used && s_v[i].obj == obj) ? &s_v[i] : NULL;
}

static float mb_to_gain(int32_t mb)
{
    if (mb <= -10000) return 0.0f;
    if (mb >= 0) return 1.0f;
    return powf(10.0f, (float)mb / 2000.0f);
}

static void set_format(Voice *v, uint32_t wfx)
{
    v->tag = FMT_PCM; v->ch = 2; v->bits = 16; v->rate = OUT_RATE; v->align = 4;
    if (!wfx)
        return;
    v->tag = r16(wfx + 0);
    v->ch = r16(wfx + 2) == 1 ? 1 : 2;
    v->rate = (int)r32(wfx + 4);
    v->align = r16(wfx + 0xC);
    v->bits = r16(wfx + 0xE);
    if (v->rate <= 0) v->rate = OUT_RATE;
    if (v->tag == FMT_XBOX_ADPCM)
        v->align = 36 * v->ch;
    else if (v->align <= 0)
        v->align = v->ch * (v->bits / 8 ? v->bits / 8 : 2);
}

static Voice *new_voice(int stream)
{
    int i;
    for (i = 0; i < MAX_VOICES; i++)
        if (!s_v[i].used) {
            Voice *v = &s_v[i];
            memset(v, 0, sizeof *v);
            v->obj = new_obj(i, stream);
            if (!v->obj)
                return NULL;
            v->used = 1;
            v->refs = 1;
            v->stream = stream;
            v->gain = 1.0f;
            v->blk = -1;
            v->st_lead_min = 1 << 30;
            return v;
        }
    return NULL;
}

static void free_voice(Voice *v)
{
    if (s_nfree < MAX_VOICES)
        s_free_obj[s_nfree++] = v->obj;
    w32(v->obj + 0x10, 0);
    v->used = 0;
}

/* Frames in n bytes of the voice's format, and back. */
static uint32_t bytes_to_frames(const Voice *v, uint32_t n)
{
    if (v->tag == FMT_XBOX_ADPCM)
        return n / (uint32_t)v->align * 65u;
    return n / (uint32_t)v->align;
}
static uint32_t frames_to_bytes(const Voice *v, uint32_t f)
{
    if (v->tag == FMT_XBOX_ADPCM)
        return f / 65u * (uint32_t)v->align;
    return f * (uint32_t)v->align;
}

/* One frame (both output channels, -1..1) of data at base. */
static void frame_at(Voice *v, uint32_t base, uint32_t f, float *l, float *r)
{
    if (v->tag == FMT_XBOX_ADPCM) {
        long b = (long)(f / 65u);
        uint32_t k = f % 65u;
        if (b != v->blk) {
            adpcm_decode_block(v->pcm, gp(base + (uint32_t)b * (uint32_t)v->align),
                               (size_t)v->align, v->ch);
            v->blk = b;
        }
        *l = v->pcm[k * (uint32_t)v->ch] / 32768.0f;
        *r = v->ch == 2 ? v->pcm[k * 2u + 1u] / 32768.0f : *l;
    } else if (v->bits == 8) {
        const uint8_t *p = gp(base + f * (uint32_t)v->align);
        *l = (p[0] - 128) / 128.0f;
        *r = v->ch == 2 ? (p[1] - 128) / 128.0f : *l;
    } else {
        const uint8_t *p = gp(base + f * (uint32_t)v->align);
        *l = (int16_t)(p[0] | p[1] << 8) / 32768.0f;
        *r = v->ch == 2 ? (int16_t)(p[2] | p[3] << 8) / 32768.0f : *l;
    }
}

/* Mix one voice into out[frames*2]. Called with the lock held. */
static void mix_voice(Voice *v, float *out, int frames)
{
    double step = (double)(v->freq ? v->freq : (uint32_t)v->rate) / OUT_RATE;
    int i;

    if (v->paused || v->gain <= 0.0f)
        goto advance_only;
    for (i = 0; i < frames; i++) {
        uint32_t base, total, f;
        float l, r;

        if (v->stream) {
            if (!v->qn)
                return;
            base = v->q[0].buf;
            total = bytes_to_frames(v, v->q[0].size);
        } else {
            if (!v->playing || !v->data)
                return;
            base = v->data;
            total = bytes_to_frames(v, v->bytes);
        }
        f = (uint32_t)v->pos;
        if (f >= total) {
            if (v->stream) {
                /* packet consumed: hand it back on the title's thread */
                if (v->dn < MAX_PACKETS) {
                    v->done[v->dn] = v->q[0];
                    v->done_status[v->dn++] = PKT_SUCCESS;
                }
                memmove(&v->q[0], &v->q[1], (size_t)(--v->qn) * sizeof v->q[0]);
                v->pos -= total;
                v->blk = -1;
                i--;
                continue;
            }
            if (v->looping) {
                uint32_t ls = bytes_to_frames(v, v->loop_start);
                uint32_t ll = v->loop_len ? bytes_to_frames(v, v->loop_len) : total - ls;
                if (!ll || ls >= total) { v->playing = 0; return; }
                v->pos = ls + fmod(v->pos - ls, (double)ll);
                if ((uint32_t)v->pos >= total) v->pos = ls;
                f = (uint32_t)v->pos;
            } else {
                v->playing = 0;
                v->pos = 0;
                return;
            }
        }
        frame_at(v, base, f, &l, &r);
        /* Linear interpolation to the next frame: titles play 22/44.1 kHz
         * data into the 48 kHz mix, and repeating samples is audible. */
        if (f + 1 < total) {
            float l2, r2, t = (float)(v->pos - f);
            frame_at(v, base, f + 1, &l2, &r2);
            l += (l2 - l) * t;
            r += (r2 - r) * t;
        }
        out[2 * i] += l * v->gain;
        out[2 * i + 1] += r * v->gain;
        v->pos += step;
        /* A loop region shorter than the buffer wraps inside it. */
        if (!v->stream && v->looping && v->loop_len) {
            uint32_t le = bytes_to_frames(v, v->loop_start + v->loop_len);
            if (v->pos >= le)
                v->pos -= bytes_to_frames(v, v->loop_len);
        }
    }
    return;

advance_only:
    /* Muted or paused streams keep time like the hardware does (a paused
     * stream does not). */
    if (!v->paused && !v->stream && v->playing) {
        uint32_t total = bytes_to_frames(v, v->bytes);
        v->pos += step * frames;
        if (v->pos >= total) {
            if (v->looping && total)
                v->pos = fmod(v->pos, (double)total);
            else { v->playing = 0; v->pos = 0; }
        }
    }
}

static void wav_write(const float *mix, int frames)
{
    static int16_t buf[4096 * 2];
    int i, n = frames * 2 > 8192 ? 8192 : frames * 2;
    for (i = 0; i < n; i++) {
        float s = mix[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        buf[i] = (int16_t)(s * 32767.0f);
    }
    fwrite(buf, 2, (size_t)n, s_wav);
    s_wav_bytes += (uint32_t)n * 2u;
    /* Keep the header right as we go: a run ends by --max-seconds. */
    {
        long at = ftell(s_wav);
        uint32_t riff = 36u + s_wav_bytes;
        fseek(s_wav, 4, SEEK_SET);  fwrite(&riff, 4, 1, s_wav);
        fseek(s_wav, 40, SEEK_SET); fwrite(&s_wav_bytes, 4, 1, s_wav);
        fseek(s_wav, at, SEEK_SET);
    }
}

static void SDLCALL mix_cb(void *ud, SDL_AudioStream *st, int additional, int total)
{
    static float mix[4096 * 2];
    (void)ud; (void)total;
    while (additional > 0) {
        int frames = additional / 8, i;
        if (frames > 4096) frames = 4096;
        if (frames <= 0) break;
        memset(mix, 0, (size_t)frames * 2 * sizeof(float));
        lock();
        if (!dbg_on()) {
            for (i = 0; i < MAX_VOICES; i++)
                if (s_v[i].used)
                    mix_voice(&s_v[i], mix, frames);
        } else {
            static float vt[4096 * 2];
            for (i = 0; i < MAX_VOICES; i++) {
                Voice *v = &s_v[i];
                int k, any = 0, had = v->stream && v->qn;
                if (!v->used)
                    continue;
                memset(vt, 0, (size_t)frames * 2 * sizeof(float));
                mix_voice(v, vt, frames);
                if (v->stream && had && !v->qn && !v->paused)
                    v->st_starved = 1;
                if (v->stream && !v->qn && v->st_starved && !v->paused) {
                    v->st_underrun++;
                    v->st_starved = 0;
                }
                for (k = 0; k < frames; k++) {
                    float l = vt[2 * k], r = vt[2 * k + 1];
                    if (l != 0.0f || r != 0.0f)
                        any = 1;
                    if (fabsf(l - v->last_l) > 0.25f || fabsf(r - v->last_r) > 0.25f)
                        v->st_jump++;
                    v->last_l = l;
                    v->last_r = r;
                }
                if (any && s_vwav_dir[0] && !v->vwav) {
                    extern FILE *xbox_fopen_utf8(const char *path, const char *mode);
                    char path[300];
                    snprintf(path, sizeof path, "%s/v%03d_%08X.wav", s_vwav_dir, i, v->obj);
                    v->vwav = xbox_fopen_utf8(path, "wb");
                    if (v->vwav)
                        wav_header(v->vwav);
                    v->vwav_bytes = 0;
                }
                if (v->vwav)
                    wav_append(v->vwav, &v->vwav_bytes, vt, frames);
                if (s_mute[i] || (s_any_solo && !s_solo[i]))
                    continue;
                for (k = 0; k < frames * 2; k++)
                    mix[k] += vt[k];
            }
        }
        unlock();
        for (i = 0; i < frames * 2; i++) {
            if (mix[i] > 1.0f) { mix[i] = 1.0f; s_clips++; }
            else if (mix[i] < -1.0f) { mix[i] = -1.0f; s_clips++; }
        }
        if (s_dbg > 0) {
            uint64_t before = s_out_frames;
            if (frames < s_chunk_min) s_chunk_min = frames;
            if (frames > s_chunk_max) s_chunk_max = frames;
            s_out_frames += (uint64_t)frames;
            if (s_log && before / OUT_RATE != s_out_frames / OUT_RATE)
                dbg_report();
        }
        if (s_wav)
            wav_write(mix, frames);
        SDL_PutAudioStreamData(st, mix, frames * 8);
        additional -= frames * 8;
    }
}

static void audio_open(void)
{
    SDL_AudioSpec spec;
    const char *wav = getenv("RECOMP_DSOUND_WAV");

    if (s_out)
        return;
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = OUT_RATE;
    /* No device (CI, a headless box): the dummy driver still runs the mixer
     * in real time, so play cursors and stream packets keep moving. */
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
        SDL_InitSubSystem(SDL_INIT_AUDIO);
    }
    s_out = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, mix_cb, NULL);
    if (!s_out) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
        SDL_InitSubSystem(SDL_INIT_AUDIO);
        s_out = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, mix_cb, NULL);
    }
    if (wav && *wav) {
        extern FILE *xbox_fopen_utf8(const char *path, const char *mode);
        s_wav = xbox_fopen_utf8(wav, "wb");
        if (s_wav) {
            static const uint8_t hdr[44] = {
                'R','I','F','F', 36,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0,
                1,0, 2,0, 0x80,0xBB,0,0, 0x00,0xEE,0x02,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
            fwrite(hdr, 1, sizeof hdr, s_wav);
        }
    }
    fprintf(stderr, "  DSOUND: HLE mixer %s (%s, 48 kHz stereo)%s\n",
            s_out ? "up" : "FAILED", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "none",
            s_wav ? ", recording RECOMP_DSOUND_WAV" : "");
    fflush(stderr);
    if (s_out)
        SDL_ResumeAudioStreamDevice(s_out);
}

static void init(void)
{
    if (!s_lock)
        s_lock = SDL_CreateMutex();
}

/* ---- stream packet completion, on the title's thread --------------------- */

static void guest_callback(uint32_t fn, uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t esp = g_esp, eax = g_eax;
    recomp_func_t f = recomp_lookup(fn);
    if (!f)
        return;
    g_esp -= 16;
    w32(g_esp + 0, 0);          /* return address: the callee only pops it */
    w32(g_esp + 4, a);
    w32(g_esp + 8, b);
    w32(g_esp + 12, c);
    f();
    g_esp = esp;                /* stdcall or not */
    g_eax = eax;
}

static void complete_packets(void)
{
    int i;
    for (i = 0; i < MAX_VOICES; i++) {
        Voice *v = &s_v[i];
        while (v->used && v->stream && v->dn) {
            Packet p;
            uint32_t st, cb, ctx, size_ptr, status_ptr, ev;
            lock();
            if (!v->dn) { unlock(); break; }
            p = v->done[0];
            st = v->done_status[0];
            memmove(&v->done[0], &v->done[1], (size_t)(v->dn - 1) * sizeof v->done[0]);
            memmove(&v->done_status[0], &v->done_status[1], (size_t)(v->dn - 1) * sizeof v->done_status[0]);
            v->dn--;
            cb = v->callback;
            ctx = v->context;
            unlock();
            size_ptr = p.size_ptr;
            status_ptr = p.status_ptr;
            ev = p.ev;
            if (size_ptr)
                w32(size_ptr, st == PKT_SUCCESS ? p.size : 0);
            if (status_ptr)
                w32(status_ptr, st);
            if (cb)
                guest_callback(cb, ctx, ev, st);
            else if (ev)
                xbox_GuestSetEvent(ev);
        }
    }
}

/* ---- DirectSound ----------------------------------------------------------- */

static void make_ds(void)
{
    if (!s_ds) {
        s_ds = xbox_HeapAlloc(OBJ_SIZE, 16);
        if (s_ds)
            memset(gp(s_ds), 0, OBJ_SIZE);
        audio_open();
    }
    s_ds_refs++;
}

void xdk_DirectSoundCreate(void)
{
    uint32_t pp = ARG(1);
    init();
    make_ds();
    if (pp)
        w32(pp, s_ds);
    RETURN(s_ds ? DS_OK : DSERR_OUTOFMEMORY, 3);
}

void xdk_DirectSoundDoWork(void)
{
    init();
    complete_packets();
    RETURN(0, 0);
}

void xdk_IDirectSound_AddRef(void)
{
    RETURN(++s_ds_refs, 1);
}

void xdk_IDirectSound_Release(void)
{
    /* The one device lives for the run, like the console's. */
    if (s_ds_refs > 0)
        s_ds_refs--;
    RETURN(s_ds_refs, 1);
}

void xdk_IDirectSound_CreateSoundBuffer(void)
{
    uint32_t desc = ARG(1), pp = ARG(2);
    Voice *v;

    init();
    if (!desc || !pp)
        RETURN(DSERR_INVALIDPARAM, 4);
    lock();
    v = new_voice(0);
    if (v) {
        set_format(v, r32(desc + 0xC));
        v->bytes = r32(desc + 8);
        if (v->bytes) {
            v->data = xbox_HeapAlloc(v->bytes, 16);
            if (v->data)
                memset(gp(v->data), 0, v->bytes);
            else
                v->bytes = 0;
        }
    }
    unlock();
    if (!v)
        RETURN(DSERR_OUTOFMEMORY, 4);
    w32(pp, v->obj);
    if (trace_on())
        fprintf(stderr, "[DSTRACE]   buffer 0x%08X: tag %d, %d ch, %d bit, %d Hz, %u bytes, flags 0x%X\n",
                v->obj, v->tag, v->ch, v->bits, v->rate, v->bytes, r32(desc + 4));
    RETURN(DS_OK, 4);
}

static uint32_t create_stream(uint32_t desc, uint32_t pp)
{
    Voice *v;
    init();
    if (!desc || !pp)
        return DSERR_INVALIDPARAM;
    lock();
    v = new_voice(1);
    if (v) {
        set_format(v, r32(desc + 8));
        v->callback = r32(desc + 0xC);
        v->context = r32(desc + 0x10);
    }
    unlock();
    if (!v)
        return DSERR_OUTOFMEMORY;
    w32(pp, v->obj);
    if (trace_on())
        fprintf(stderr, "[DSTRACE]   stream 0x%08X: tag %d, %d ch, %d bit, %d Hz, callback 0x%08X\n",
                v->obj, v->tag, v->ch, v->bits, v->rate, v->callback);
    return DS_OK;
}

void xdk_IDirectSound_CreateSoundStream(void)
{
    uint32_t rc = create_stream(ARG(1), ARG(2));
    RETURN(rc, 4);
}

void xdk_DirectSoundCreateStream(void)
{
    uint32_t rc;
    init();
    if (!s_ds)
        make_ds();
    rc = create_stream(ARG(0), ARG(1));
    RETURN(rc, 2);
}

/* ---- IDirectSoundBuffer ---------------------------------------------------- */

#define BUF_OR_FAIL(n)                                   \
    Voice *v;                                            \
    init();                                              \
    v = voice_of(ARG(0));                                \
    if (!v) RETURN(DSERR_INVALIDPARAM, n)

void xdk_DSB_Release(void)
{
    uint32_t refs;
    BUF_OR_FAIL(1);
    lock();
    refs = (uint32_t)--v->refs;
    if (!v->refs)
        free_voice(v);
    unlock();
    RETURN(refs, 1);
}

/* RECOMP_DSOUND_DUMP_SRC=<dir> (debug): the data a buffer plays, as the title
 * stored it (native rate, PCM 8/16 or Xbox ADPCM decoded), written once per
 * buffer and data address when it starts playing: <dir>\vSSS_<obj>_<data>.wav.
 * Tells a sound that crackles in the source (intended) from one we render
 * wrongly. */
static void dump_src(const Voice *v, int slot)
{
    static int on = -1;
    static char dir[260];
    static uint32_t seen[512];
    static int nseen;
    extern FILE *xbox_fopen_utf8(const char *path, const char *mode);
    char path[320];
    FILE *f;
    uint32_t frames, i, key = v->data ^ (uint32_t)slot;
    int k;
    if (on < 0) {
        const char *e = getenv("RECOMP_DSOUND_DUMP_SRC");
        on = e && *e;
        if (on)
            snprintf(dir, sizeof dir, "%s", e);
    }
    if (!on || !v->data || !v->bytes)
        return;
    for (k = 0; k < nseen; k++)
        if (seen[k] == key)
            return;
    if (nseen < 512)
        seen[nseen++] = key;
    snprintf(path, sizeof path, "%s/v%03d_%08X_%08X.wav", dir, slot, v->obj, v->data);
    f = xbox_fopen_utf8(path, "wb");
    if (!f)
        return;
    frames = bytes_to_frames(v, v->bytes);
    {
        uint32_t rate = (uint32_t)v->rate, ch = (uint32_t)v->ch, ba = ch * 2u, br = rate * ba,
                 data = frames * ba, riff = 36u + data;
        uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0,
                          (uint8_t)ch,0, 0,0,0,0, 0,0,0,0, (uint8_t)ba,0, 16,0, 'd','a','t','a', 0,0,0,0 };
        memcpy(h + 4, &riff, 4); memcpy(h + 24, &rate, 4); memcpy(h + 28, &br, 4); memcpy(h + 40, &data, 4);
        fwrite(h, 1, 44, f);
    }
    {
        Voice t = *v;                         /* frame_at caches ADPCM blocks */
        t.blk = -1;
        for (i = 0; i < frames; i++) {
            float l, r;
            int16_t o[2];
            frame_at(&t, t.data, i, &l, &r);
            o[0] = (int16_t)(l * 32767.0f);
            o[1] = (int16_t)(r * 32767.0f);
            fwrite(o, 2, (size_t)t.ch, f);
        }
    }
    fclose(f);
    fprintf(stderr, "  DSOUND: source v%03d %08X data %08X: %u frames, %d Hz, %d ch, tag %d, loop %u+%u\n",
            slot, v->obj, v->data, frames, v->rate, v->ch, v->tag, v->loop_start, v->loop_len);
}

void xdk_DSB_Play(void)
{
    uint32_t flags = ARG(3);
    BUF_OR_FAIL(4);
    lock();
    v->st_play++;
    v->playing = 1;
    v->looping = (flags & DSBPLAY_LOOPING) != 0;
    dump_src(v, (int)(v - s_v));
    unlock();
    RETURN(DS_OK, 4);
}

void xdk_DSB_Stop(void)
{
    BUF_OR_FAIL(1);
    lock();
    v->playing = 0;
    unlock();
    RETURN(DS_OK, 1);
}

void xdk_DSB_GetStatus(void)
{
    uint32_t p = ARG(1), st;
    BUF_OR_FAIL(2);
    lock();
    st = v->playing ? DSBSTATUS_PLAYING | (v->looping ? DSBSTATUS_LOOPING : 0) : 0;
    unlock();
    if (p)
        w32(p, st);
    RETURN(DS_OK, 2);
}

void xdk_DSB_GetCurrentPosition(void)
{
    uint32_t pplay = ARG(1), pwrite = ARG(2), at;
    BUF_OR_FAIL(3);
    lock();
    at = frames_to_bytes(v, (uint32_t)v->pos);
    if (v->bytes && at >= v->bytes)
        at = 0;
    v->st_getpos++;
    unlock();
    if (pplay)
        w32(pplay, at);
    if (pwrite)
        w32(pwrite, at);
    RETURN(DS_OK, 3);
}

void xdk_DSB_SetCurrentPosition(void)
{
    uint32_t at = ARG(1);
    BUF_OR_FAIL(2);
    lock();
    v->st_setpos++;
    v->pos = bytes_to_frames(v, at);
    unlock();
    RETURN(DS_OK, 2);
}

void xdk_DSB_Lock(void)
{
    uint32_t off = ARG(1), n = ARG(2), pp1 = ARG(3), pn1 = ARG(4), pp2 = ARG(5), pn2 = ARG(6),
             flags = ARG(7), n1, n2;
    BUF_OR_FAIL(8);
    lock();
    if (flags & DSBLOCK_FROMWRITECURSOR)
        off = frames_to_bytes(v, (uint32_t)v->pos);
    if (flags & DSBLOCK_ENTIREBUFFER)
        n = v->bytes;
    if (!v->bytes || n > v->bytes) {
        unlock();
        RETURN(DSERR_INVALIDPARAM, 8);
    }
    off %= v->bytes;
    n1 = n <= v->bytes - off ? n : v->bytes - off;
    n2 = n - n1;
    {
        /* debug: where the title writes relative to the read head */
        uint32_t head = frames_to_bytes(v, (uint32_t)v->pos) % v->bytes;
        int32_t lead = (int32_t)((off + v->bytes - head) % v->bytes);
        v->st_lock++;
        if (lead < v->st_lead_min)
            v->st_lead_min = lead;
        if (v->playing && (uint32_t)lead > v->bytes - n)   /* the region covers the read head */
            v->st_lockhit++;
    }
    unlock();
    /* The title writes straight into the buffer; Unlock has nothing to do. */
    if (pp1) w32(pp1, v->data + off);
    if (pn1) w32(pn1, n1);
    if (pp2) w32(pp2, n2 ? v->data : 0);
    if (pn2) w32(pn2, n2);
    RETURN(DS_OK, 8);
}

void xdk_DSB_SetBufferData(void)
{
    uint32_t data = ARG(1), bytes = ARG(2);
    BUF_OR_FAIL(3);
    lock();
    v->st_setdata++;
    v->data = data;
    v->bytes = data ? bytes : 0;
    v->pos = 0;
    v->blk = -1;
    if (v->loop_start + v->loop_len > v->bytes)
        v->loop_start = v->loop_len = 0;
    unlock();
    RETURN(DS_OK, 3);
}

void xdk_DSB_SetLoopRegion(void)
{
    uint32_t start = ARG(1), len = ARG(2);
    BUF_OR_FAIL(3);
    lock();
    v->loop_start = start;
    v->loop_len = len;
    unlock();
    RETURN(DS_OK, 3);
}

void xdk_DSB_SetFrequency(void)
{
    uint32_t hz = ARG(1);
    BUF_OR_FAIL(2);
    lock();
    if (v->freq != hz)
        v->st_freq++;
    v->freq = hz;
    unlock();
    RETURN(DS_OK, 2);
}

void xdk_DSB_SetVolume(void)
{
    int32_t mb = (int32_t)ARG(1);
    BUF_OR_FAIL(2);
    lock();
    if (v->gain != mb_to_gain(mb))
        v->st_gain++;
    v->gain = mb_to_gain(mb);
    unlock();
    RETURN(DS_OK, 2);
}

/* ---- IDirectSoundStream (XMediaObject) ------------------------------------- */

void xdk_DSS_AddRef(void)
{
    uint32_t refs;
    BUF_OR_FAIL(1);
    lock();
    refs = (uint32_t)++v->refs;
    unlock();
    RETURN(refs, 1);
}

static void flush_locked(Voice *v)
{
    int i;
    for (i = 0; i < v->qn && v->dn < MAX_PACKETS; i++) {
        v->done[v->dn] = v->q[i];
        v->done_status[v->dn++] = PKT_FLUSHED;
    }
    v->qn = 0;
    v->pos = 0;
    v->blk = -1;
}

void xdk_DSS_Release(void)
{
    uint32_t refs;
    BUF_OR_FAIL(1);
    lock();
    refs = (uint32_t)--v->refs;
    if (!v->refs)
        flush_locked(v);
    unlock();
    complete_packets();
    if (!refs) {
        lock();
        free_voice(v);
        unlock();
    }
    RETURN(refs, 1);
}

void xdk_DSS_GetInfo(void)
{
    uint32_t p = ARG(1);
    BUF_OR_FAIL(2);
    /* XMEDIAINFO: dwFlags, dwInputSize, dwOutputSize, dwMaxLookahead. */
    if (p) {
        w32(p + 0, 0);
        w32(p + 4, (uint32_t)v->align);
        w32(p + 8, 0);
        w32(p + 12, 0);
    }
    RETURN(DS_OK, 2);
}

void xdk_DSS_GetStatus(void)
{
    uint32_t p = ARG(1), st;
    BUF_OR_FAIL(2);
    complete_packets();
    lock();
    st = v->qn < MAX_PACKETS ? XMO_STATUSF_ACCEPT_INPUT_DATA : 0;
    unlock();
    if (p)
        w32(p, st);
    RETURN(DS_OK, 2);
}

void xdk_DSS_Process(void)
{
    uint32_t in = ARG(1);
    int ok = 0;
    BUF_OR_FAIL(3);
    complete_packets();
    if (!in)
        RETURN(DSERR_INVALIDPARAM, 3);
    if (r32(in + 0xC))
        w32(r32(in + 0xC), PKT_PENDING);
    if (r32(in + 8))
        w32(r32(in + 8), 0);
    lock();
    if (v->qn < MAX_PACKETS) {
        v->q[v->qn].pkt = in;
        v->q[v->qn].buf = r32(in + 0);
        v->q[v->qn].size = r32(in + 4);
        v->q[v->qn].size_ptr = r32(in + 8);
        v->q[v->qn].status_ptr = r32(in + 0xC);
        v->q[v->qn].ev = r32(in + 0x10);
        v->qn++;
        ok = 1;
    }
    unlock();
    RETURN(ok ? DS_OK : E_FAIL_, 3);
}

void xdk_DSS_Discontinuity(void)
{
    BUF_OR_FAIL(1);
    complete_packets();
    RETURN(DS_OK, 1);
}

void xdk_DSS_Flush(void)
{
    BUF_OR_FAIL(1);
    lock();
    flush_locked(v);
    unlock();
    complete_packets();
    RETURN(DS_OK, 1);
}

void xdk_DSS_SetVolume(void)
{
    int32_t mb = (int32_t)ARG(1);
    BUF_OR_FAIL(2);
    lock();
    if (v->gain != mb_to_gain(mb))
        v->st_gain++;
    v->gain = mb_to_gain(mb);
    unlock();
    RETURN(DS_OK, 2);
}

void xdk_DSS_Pause(void)
{
    uint32_t pause = ARG(1);
    BUF_OR_FAIL(2);
    lock();
    if (pause == 1) v->paused = 1;
    else if (pause == 0) v->paused = 0;
    unlock();
    RETURN(DS_OK, 2);
}
