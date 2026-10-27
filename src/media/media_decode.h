/*
 * media_decode.h -- one music file as 16-bit stereo PCM at 44100 Hz.
 *
 * Decoders: dr_mp3 / dr_flac / dr_wav (mackron/dr_libs, public domain or
 * MIT-0) and stb_vorbis (nothings/stb, MIT or public domain), vendored in
 * third_party/ (versions in NOTICE). No Media Foundation, no OS codecs, so
 * the same code runs on every host. Mono is doubled, more than two channels
 * keep the first two; other sample rates are resampled linearly.
 */
#ifndef MEDIA_DECODE_H
#define MEDIA_DECODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_RATE 44100        /* output: 44100 Hz, 2 channels, s16 */

typedef struct MediaDecoder MediaDecoder;

/* NULL (and a log line) when the file cannot be opened or decoded. */
MediaDecoder *media_decoder_open(const char *path);
/* Up to `frames` stereo frames into out; returns the frames written: fewer
 * only at the end of the file, 0 after it, -1 on a decode error. */
long     media_decoder_read(MediaDecoder *d, int16_t *out, long frames);
int      media_decoder_rewind(MediaDecoder *d);
/* Length in ms at the output rate, 0 when the file does not say (an MP3
 * without a Xing/Info header). */
uint32_t media_decoder_length_ms(const MediaDecoder *d);
void     media_decoder_close(MediaDecoder *d);

/* Length in ms without keeping the file open (0 = unknown). */
uint32_t media_probe_length_ms(const char *path);

#ifdef __cplusplus
}
#endif

#endif
