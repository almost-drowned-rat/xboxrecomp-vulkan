/* boot_video.c -- the optional boot animation (see boot_video.h). */

#include "boot_video.h"

#include <stdio.h>                                         /* before pl_mpeg.h: it names FILE */
#include <stdlib.h>
#include <string.h>

#include "third_party/pl_mpeg/pl_mpeg.h"

#include <SDL3/SDL.h>

#define BV_CHUNK (64 * 1024)

typedef struct {
    SDL_IOStream *io;
    int eof;
    uint8_t chunk[BV_CHUNK];
    uint8_t *px;                 /* the frame as B8G8R8A8 for the sink */
    int w, h;
    const BootVideoSink *sink;
    SDL_AudioStream *audio;
    int stop;
} BvCtx;

/* pl_mpeg asks for more data: the next chunk of the file (SDL_IOStream reads
 * UTF-8 paths on every platform) */
static void bv_load(plm_buffer_t *b, void *user)
{
    BvCtx *c = (BvCtx *)user;
    size_t r;
    if (c->eof)
        return;
    r = SDL_ReadIO(c->io, c->chunk, sizeof c->chunk);
    if (r > 0)
        plm_buffer_write(b, c->chunk, r);
    if (r < sizeof c->chunk) {
        c->eof = 1;
        plm_buffer_signal_end(b);
    }
}

/* the probe at the start seeks back (plm_demux_probe): a file-mode buffer
 * re-reads from there instead of losing what it read */
static void bv_seek(plm_buffer_t *b, size_t offset, void *user)
{
    BvCtx *c = (BvCtx *)user;
    (void)b;
    SDL_SeekIO(c->io, (Sint64)offset, SDL_IO_SEEK_SET);
    c->eof = 0;
}

static size_t bv_tell(plm_buffer_t *b, void *user)
{
    BvCtx *c = (BvCtx *)user;
    Sint64 at = SDL_TellIO(c->io);
    (void)b;
    return at > 0 ? (size_t)at : 0;
}

static void bv_video(plm_t *plm, plm_frame_t *frame, void *user)
{
    BvCtx *c = (BvCtx *)user;
    (void)plm;
    if (c->stop || (int)frame->width != c->w || (int)frame->height != c->h)
        return;
    plm_frame_to_bgra(frame, c->px, c->w * 4);
    if (!c->sink->frame(c->px, c->w, c->h, c->sink->user))
        c->stop = 1;
}

static void bv_audio(plm_t *plm, plm_samples_t *samples, void *user)
{
    BvCtx *c = (BvCtx *)user;
    (void)plm;
    if (c->audio && !c->stop)
        SDL_PutAudioStreamData(c->audio, samples->interleaved, (int)(samples->count * 2 * sizeof(float)));
}

int boot_video_play(const char *path, const BootVideoSink *sink)
{
    BvCtx *c;
    plm_buffer_t *buf;
    plm_t *plm;
    int result, audio_init = 0;
    Uint64 t0, last;
    if (!path || !*path || !sink || !sink->frame)
        return BOOT_VIDEO_NOT_PLAYED;
    if (!(c = (BvCtx *)calloc(1, sizeof *c)))
        return BOOT_VIDEO_NOT_PLAYED;
    c->sink = sink;
    if (!(c->io = SDL_IOFromFile(path, "rb"))) {
        fprintf(stderr, "[BOOT] boot video: cannot open %s (%s)\n", path, SDL_GetError());
        free(c);
        return BOOT_VIDEO_NOT_PLAYED;
    }
    /* a file-mode buffer on our SDL stream (load / seek / tell), set up before
     * plm_create_with_buffer: it reads the headers at once */
    {
        Sint64 size = SDL_GetIOSize(c->io);
        buf = plm_buffer_create_with_callbacks(bv_load, bv_seek, bv_tell, size > 0 ? (size_t)size : 0, c);
    }
    plm = buf ? plm_create_with_buffer(buf, 1) : NULL;
    if (!plm) {
        if (buf)
            plm_buffer_destroy(buf);
        SDL_CloseIO(c->io);
        free(c);
        fprintf(stderr, "[BOOT] boot video: out of memory\n");
        return BOOT_VIDEO_NOT_PLAYED;
    }
    if (!plm_probe(plm, 5000 * 1024) || plm_get_num_video_streams(plm) < 1
        || plm_get_width(plm) <= 0 || plm_get_height(plm) <= 0) {
        fprintf(stderr, "[BOOT] boot video: %s has no MPEG-1 video stream -- not played (convert it as the README "
                        "says)\n", path);
        plm_destroy(plm);
        SDL_CloseIO(c->io);
        free(c);
        return BOOT_VIDEO_NOT_PLAYED;
    }
    c->w = plm_get_width(plm);
    c->h = plm_get_height(plm);
    if (!(c->px = (uint8_t *)malloc((size_t)c->w * (size_t)c->h * 4))) {
        plm_destroy(plm);
        SDL_CloseIO(c->io);
        free(c);
        return BOOT_VIDEO_NOT_PLAYED;
    }
    plm_set_video_decode_callback(plm, bv_video, c);
    if (plm_get_num_audio_streams(plm) > 0 && plm_get_samplerate(plm) > 0) {
        SDL_AudioSpec spec;
        spec.format = SDL_AUDIO_F32;
        spec.channels = 2;
        spec.freq = plm_get_samplerate(plm);
        if ((audio_init = SDL_InitSubSystem(SDL_INIT_AUDIO))
            && (c->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL)) != NULL) {
            plm_set_audio_enabled(plm, 1);
            plm_set_audio_stream(plm, 0);
            plm_set_audio_decode_callback(plm, bv_audio, c);
            plm_set_audio_lead_time(plm, 0.05);    /* decoded this far ahead of the picture: the device's buffer */
            SDL_ResumeAudioStreamDevice(c->audio);
        } else {
            fprintf(stderr, "[BOOT] boot video: no audio output (%s) -- picture only\n", SDL_GetError());
            plm_set_audio_enabled(plm, 0);
        }
    } else {
        plm_set_audio_enabled(plm, 0);
    }
    fprintf(stderr, "[BOOT] boot video: %s -- %dx%d, %.2f fps, audio %s (%d Hz), %.1f s\n", path, c->w, c->h,
            plm_get_framerate(plm), c->audio ? "on" : "none", plm_get_samplerate(plm), plm_get_duration(plm));
    /* paced by the clock: pl_mpeg decodes what the elapsed time asks for
     * (picture and sound in step), the thread sleeps between frames */
    t0 = last = SDL_GetTicksNS();
    while (!c->stop && !plm_has_ended(plm)) {
        Uint64 now = SDL_GetTicksNS();
        double dt = (double)(now - last) / 1e9;
        last = now;
        if (dt > 0.25)
            dt = 0.25;                             /* a stall: catch up, never race through */
        plm_decode(plm, dt);
        SDL_DelayNS(2 * 1000 * 1000);
    }
    result = c->stop ? BOOT_VIDEO_STOPPED : BOOT_VIDEO_PLAYED;
    if (c->audio) {
        int i;
        for (i = 0; result == BOOT_VIDEO_PLAYED && i < 100 && SDL_GetAudioStreamQueued(c->audio) > 0; i++)
            SDL_Delay(5);                          /* the last sound of the clip, at most 0.5 s */
        SDL_DestroyAudioStream(c->audio);
    }
    if (audio_init)
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    fprintf(stderr, "[BOOT] boot video: %s after %.1f s\n", result == BOOT_VIDEO_STOPPED ? "stopped (no window)" : "played",
            (double)(SDL_GetTicksNS() - t0) / 1e9);
    plm_destroy(plm);
    SDL_CloseIO(c->io);
    free(c->px);
    free(c);
    return result;
}

static int bv_exists(const char *path)
{
    SDL_PathInfo pi;
    return SDL_GetPathInfo(path, &pi) && pi.type == SDL_PATHTYPE_FILE;
}

int boot_video_find(const char *media_dir, char *out, size_t n, int *convert)
{
    char p[1024];
    int have;
    if (convert)
        *convert = 0;
    if (!media_dir || !*media_dir)
        return 0;
    snprintf(p, sizeof p, "%s/boot.mpg", media_dir);
    have = bv_exists(p);
    if (have && out && n)
        snprintf(out, n, "%s", p);
    if (!have && convert) {
        char q[1024];
        snprintf(q, sizeof q, "%s/boot.mp4", media_dir);
        *convert = bv_exists(q);
        snprintf(q, sizeof q, "%s/boot.mkv", media_dir);
        *convert = *convert || bv_exists(q);
    }
    return have;
}
