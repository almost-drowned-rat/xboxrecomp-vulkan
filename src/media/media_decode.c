/* media_decode.c -- music files to 16-bit stereo PCM at 44100 Hz (see media_decode.h). */

#include "media_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wtautological-compare"
#else
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wtype-limits"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define DR_MP3_IMPLEMENTATION
#include "third_party/dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#include "third_party/dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "third_party/dr_wav.h"
#define STB_VORBIS_NO_PUSHDATA_API
#include "third_party/stb_vorbis.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

typedef enum { K_MP3, K_FLAC, K_WAV, K_VORBIS } Kind;

#define SRC_CHUNK 1024                  /* source frames per read */

struct MediaDecoder {
    Kind        kind;
    drmp3       mp3;
    drflac     *flac;
    drwav       wav;
    stb_vorbis *vorbis;
    unsigned    channels, rate;
    uint64_t    frames;                 /* source frames, 0 = unknown */
    /* source frames, stereo, waiting to be resampled */
    int16_t     buf[SRC_CHUNK * 2 + 2];
    long        nbuf, at;
    double      frac;                   /* position between buf[at] and buf[at + 1] */
    int         eof;
};

static int ieq_ext(const char *path, const char *ext)
{
    const char *d = strrchr(path, '.');
    size_t i;
    if (!d)
        return 0;
    for (i = 0; ext[i]; i++)
        if ((d[1 + i] | 0x20) != ext[i])
            return 0;
    return d[1 + i] == 0;
}

#ifdef _WIN32
static wchar_t *widen(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? (wchar_t *)malloc((size_t)n * sizeof *w) : NULL;
    if (w)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
#endif

static FILE *open_utf8(const char *path)
{
#ifdef _WIN32
    wchar_t *w = widen(path);
    FILE *f = w ? _wfopen(w, L"rb") : NULL;
    free(w);
    return f;
#else
    return fopen(path, "rb");
#endif
}

static int open_backend(MediaDecoder *d, const char *path)
{
#ifdef _WIN32
    wchar_t *w = widen(path);
    int ok = 0;
    if (!w)
        return 0;
    switch (d->kind) {
    case K_MP3:  ok = drmp3_init_file_w(&d->mp3, w, NULL); break;
    case K_FLAC: ok = (d->flac = drflac_open_file_w(w, NULL)) != NULL; break;
    case K_WAV:  ok = drwav_init_file_w(&d->wav, w, NULL); break;
    default: break;
    }
    free(w);
    if (d->kind != K_VORBIS)
        return ok;
#else
    switch (d->kind) {
    case K_MP3:  return drmp3_init_file(&d->mp3, path, NULL);
    case K_FLAC: return (d->flac = drflac_open_file(path, NULL)) != NULL;
    case K_WAV:  return drwav_init_file(&d->wav, path, NULL);
    default: break;
    }
#endif
    {
        FILE *f = open_utf8(path);
        int err = 0;
        if (!f)
            return 0;
        d->vorbis = stb_vorbis_open_file(f, 1, &err, NULL);
        if (!d->vorbis) {
            fclose(f);
            return 0;
        }
        return 1;
    }
}

MediaDecoder *media_decoder_open(const char *path)
{
    MediaDecoder *d = (MediaDecoder *)calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->kind = ieq_ext(path, "mp3") ? K_MP3 : ieq_ext(path, "flac") ? K_FLAC : ieq_ext(path, "wav") ? K_WAV : K_VORBIS;
    if (!open_backend(d, path)) {
        fprintf(stderr, "[MEDIA] %s: cannot be opened or decoded\n", path);
        free(d);
        return NULL;
    }
    switch (d->kind) {
    case K_MP3:
        d->channels = d->mp3.channels; d->rate = d->mp3.sampleRate;
        d->frames = d->mp3.totalPCMFrameCount == DRMP3_UINT64_MAX ? 0 : d->mp3.totalPCMFrameCount;
        break;
    case K_FLAC:
        d->channels = d->flac->channels; d->rate = d->flac->sampleRate; d->frames = d->flac->totalPCMFrameCount;
        break;
    case K_WAV:
        d->channels = d->wav.channels; d->rate = d->wav.sampleRate; d->frames = d->wav.totalPCMFrameCount;
        break;
    case K_VORBIS: {
        stb_vorbis_info vi = stb_vorbis_get_info(d->vorbis);
        d->channels = (unsigned)vi.channels; d->rate = vi.sample_rate;
        d->frames = stb_vorbis_stream_length_in_samples(d->vorbis);
        break;
    }
    }
    if (!d->channels || !d->rate || d->rate > 384000) {
        fprintf(stderr, "[MEDIA] %s: unusable format (%u channels, %u Hz)\n", path, d->channels, d->rate);
        media_decoder_close(d);
        return NULL;
    }
    return d;
}

/* Up to n source frames as interleaved stereo s16; frames read, -1 on error. */
static long read_source(MediaDecoder *d, int16_t *out, long n)
{
    int16_t tmp[SRC_CHUNK * 8];
    unsigned ch = d->channels;
    long got, i;
    if (n > (long)(sizeof tmp / sizeof tmp[0] / ch))
        n = (long)(sizeof tmp / sizeof tmp[0] / ch);
    if (n <= 0)
        return -1;
    switch (d->kind) {
    case K_MP3:  got = (long)drmp3_read_pcm_frames_s16(&d->mp3, (drmp3_uint64)n, tmp); break;
    case K_FLAC: got = (long)drflac_read_pcm_frames_s16(d->flac, (drflac_uint64)n, tmp); break;
    case K_WAV:  got = (long)drwav_read_pcm_frames_s16(&d->wav, (drwav_uint64)n, tmp); break;
    default:     got = stb_vorbis_get_samples_short_interleaved(d->vorbis, (int)ch, tmp, (int)(n * ch)); break;
    }
    if (got < 0)
        return -1;
    for (i = 0; i < got; i++) {
        out[2 * i] = tmp[i * ch];
        out[2 * i + 1] = tmp[i * ch + (ch > 1)];
    }
    return got;
}

long media_decoder_read(MediaDecoder *d, int16_t *out, long frames)
{
    long done = 0;
    if (d->rate == MEDIA_RATE) {
        while (done < frames) {
            long got = read_source(d, out + 2 * done, frames - done);
            if (got < 0)
                return done ? done : -1;
            if (!got)
                break;
            done += got;
        }
        return done;
    }
    {
        const double step = (double)d->rate / MEDIA_RATE;
        while (done < frames) {
            /* need buf[at] and buf[at + 1] */
            if (d->at + 1 >= d->nbuf) {
                long got;
                if (d->eof)
                    break;
                if (d->nbuf) {
                    d->buf[0] = d->buf[2 * (d->nbuf - 1)];
                    d->buf[1] = d->buf[2 * (d->nbuf - 1) + 1];
                    d->at -= d->nbuf - 1;
                    d->nbuf = 1;
                }
                got = read_source(d, d->buf + 2 * d->nbuf, SRC_CHUNK);
                if (got < 0)
                    return done ? done : -1;
                if (!got) {
                    d->eof = 1;
                    break;
                }
                d->nbuf += got;
                continue;
            }
            {
                const int16_t *a = d->buf + 2 * d->at, *b = a + 2;
                double f = d->frac;
                out[2 * done] = (int16_t)(a[0] + (b[0] - a[0]) * f);
                out[2 * done + 1] = (int16_t)(a[1] + (b[1] - a[1]) * f);
                done++;
                d->frac += step;
                while (d->frac >= 1.0) {
                    d->frac -= 1.0;
                    d->at++;
                }
            }
        }
        return done;
    }
}

int media_decoder_rewind(MediaDecoder *d)
{
    int ok;
    switch (d->kind) {
    case K_MP3:  ok = drmp3_seek_to_pcm_frame(&d->mp3, 0); break;
    case K_FLAC: ok = drflac_seek_to_pcm_frame(d->flac, 0); break;
    case K_WAV:  ok = drwav_seek_to_pcm_frame(&d->wav, 0); break;
    default:     ok = stb_vorbis_seek_start(d->vorbis); break;
    }
    d->nbuf = d->at = 0;
    d->frac = 0;
    d->eof = 0;
    return ok;
}

uint32_t media_decoder_length_ms(const MediaDecoder *d)
{
    return d->frames ? (uint32_t)(d->frames * 1000u / d->rate) : 0;
}

void media_decoder_close(MediaDecoder *d)
{
    if (!d)
        return;
    switch (d->kind) {
    case K_MP3:  drmp3_uninit(&d->mp3); break;
    case K_FLAC: drflac_close(d->flac); break;
    case K_WAV:  drwav_uninit(&d->wav); break;
    default:     if (d->vorbis) stb_vorbis_close(d->vorbis); break;
    }
    free(d);
}

uint32_t media_probe_length_ms(const char *path)
{
    MediaDecoder *d = media_decoder_open(path);
    uint32_t ms = d ? media_decoder_length_ms(d) : 0;
    media_decoder_close(d);
    return ms;
}
