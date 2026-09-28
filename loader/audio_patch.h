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

// The companion's channel-slot table (FModAudioSystem, 45 slots). attach hands
// over the FModAudioSystem and its ChannelInfo::Reset; ensure_free runs before
// PlaySound/PlayStream pick a slot and frees a dead one only when none is free;
// stop_begin/end bracket StopChannel so a stale stop cannot kill another slot's
// voice; log_slots prints the census on the watchdog clock.
void audio_slots_attach(void *fmod_sys, void (*reset)(void *info));
void audio_slots_ensure_free(void);
void audio_slot_stop_begin(unsigned key);
void audio_slot_stop_end(void);
void audio_log_slots(void);

typedef struct {
  unsigned feed_count, feed_max_us, underruns;
  uint64_t feed_us;
} audio_perf_t;

// Cumulative streaming-decoder work for correlation with slow frames.
void audio_perf_snapshot(audio_perf_t *out);

// Ensure the application's single sceAudioOut thread is ready. The custom Bink
// OpenSL adapter shares this output rather than opening a second BGM port.
int audio_ensure_output(void);

// FModAudioSystem's wrapper knows the stable resource ID, but FMOD::createSound
// receives only the transient buffer. Scope the ID around that nested call so
// decoded SFX can be found without hashing memory the game may already reuse.
unsigned audio_sfx_context_push(unsigned id);
void audio_sfx_context_pop(unsigned previous_id);
// The resref of the sound being created on this thread, for the loudness
// census. Pass NULL when the create returns; the pointer is not kept.
void audio_sfx_context_name(const char *name);

#endif
