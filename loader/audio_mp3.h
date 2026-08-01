/* audio_mp3.h -- decode KOTOR's MP3-in-a-.wav assets to PCM (see audio_mp3.c) */

#ifndef __AUDIO_MP3_H__
#define __AUDIO_MP3_H__

#include <stdint.h>

typedef struct {
  int16_t *pcm;        // interleaved 16-bit, `channels` per frame (malloc'd)
  unsigned nsamples;   // frames (per channel), NOT total int16 count
  unsigned channels;
  unsigned rate;       // Hz, from the MPEG frame header (not the fake RIFF)
  unsigned ms;         // duration, what FMOD::Sound::getLength must report
} AudioPcm;

// One-time hardware decoder library init. Safe to call repeatedly.
int  audio_mp3_init_library(void);

// Decode a whole in-memory asset. Handles real RIFF/WAVE PCM, the 58-byte fake
// RIFF header, ID3v2 tags, and raw MP3. Returns 1 and fills `out` (caller owns
// out->pcm), else 0.
int  audio_mp3_decode(const void *data, unsigned len, AudioPcm *out);

// Header-only: fill rate/channels/nsamples/ms WITHOUT decoding or allocating
// (out->pcm stays NULL). Duration is estimated from the frame bitrate. Used to
// build a correctly-timed silent placeholder when a stream is too large to hold
// in RAM, so the game's pacing stays right instead of retrying forever.
int  audio_mp3_probe(const void *data, unsigned len, AudioPcm *out);

void audio_pcm_free(AudioPcm *p);

#endif
