/* audio_patch.h -- FMOD/OpenSLES backend over sceAudioOut (see audio_patch.c) */

#ifndef __AUDIO_PATCH_H__
#define __AUDIO_PATCH_H__

#include "so_util.h"

// Number of entries appended by audio_get_dynlib().
extern const int audio_dynlib_size;

// Returns the FMOD/OpenSLES table so main can splice it into the resolver
// (kept separate to keep dynlib.c readable).
const so_default_dynlib *audio_get_dynlib(void);

// Bytes of decoded PCM currently held by the sound cache.
unsigned audio_cache_bytes(void);

// Free every cached decode nothing is currently referencing, and return how
// many bytes that recovered. Called when the heap is exhausted: the cache is
// pure speed, so handing it back beats an allocation failure. Anything a live
// Sound still points at is kept.
unsigned audio_cache_purge(void);

// One [snd] stats census line. Driven by the watchdog's clock rather than by
// createSound volume, because a volume-triggered summary thins out exactly when
// the thing it measures stops -- which is what happened in log172.
void audio_log_stats(void);

// playSound calls that actually reached the mixer. The sound pipeline census in
// main.c prints this beside the game's own PlaySound count: a gap between them
// is the game refusing itself, which no counter on this side can see.
unsigned audio_play_count(void);

#endif
