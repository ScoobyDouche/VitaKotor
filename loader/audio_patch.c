/* audio_patch.c -- real FMOD backend over sceAudioOut + the hardware MP3 decoder
 *
 * KOTOR's audio path is FModAudioSystem (in libandroid_port.so) -> FMOD ->
 * OpenSLES. We do not so-load libfmod.so; we implement the ~30-call FMOD subset
 * the game actually imports, mix in software, and push to one sceAudioOut port.
 * Asset decode lives in audio_mp3.c (KOTOR's `.wav` files are MP3 in disguise).
 *
 * Why this matters beyond "there is now sound": conversation pacing runs through
 * CClientExoAppInternal::IsSoundPlayingInDialog (+0x1a576c), which bottoms out in
 * CExoStreamingSoundSource::IsPlaying(). With the old no-op stubs there was never
 * a playing source, so every line reported finished the instant it started and
 * dialogue was unreadable.
 *
 * That path does NOT reach FMOD::ChannelControl::isPlaying, though -- which is
 * why implementing it correctly changed nothing and why it is called zero times
 * in a whole session. FModAudioSystem::GetIsChannelPlaying (+0x746a5) reads a
 * cached flag on its own ChannelInfo, and the sole writer of that flag is
 * HandleChannelEnd, reached only from the END callback the game installs with
 * setCallback. Delivering that callback is what makes the engine's view of
 * playback true; see the end-of-sound section below.
 *
 * ---- HOW THE COMPANION CALLS US ---------------------------------------------
 * Established by disassembly 2026-07-31; both of its entry points funnel into
 * FMOD::System::createSound(this, name, mode, exinfo, &out), never createStream:
 *
 *   FModAudioSystem::CreateSound  (+0x73221) -- SFX, already in memory
 *       mode = 0x08000809 (2D) / 0x08000811 (3D)
 *            = FMOD_LOWMEM | FMOD_OPENMEMORY | FMOD_LOOP_OFF | FMOD_2D/3D
 *       `name` is NOT a path: it is the PCM-source BUFFER POINTER, and the byte
 *       count is exinfo->length. exinfo is 148 bytes with cbsize=148.
 *
 *   FModAudioSystem::CreateStream (+0x73935) -- music/VO
 *       mode = 0x08000089 (2D) / 0x08000091 (3D)
 *            = FMOD_LOWMEM | FMOD_CREATESTREAM | FMOD_LOOP_OFF | FMOD_2D/3D
 *       `name` IS a path, built by snprintf (".\STREAMMUSIC\mus_theme_cult.mp3"),
 *       with exinfo->fileoffset / ->length naming the window inside the file --
 *       58 for the fake-RIFF MP3s, 470 for the .wav-wrapped ones.
 *
 * The previous cut of this file rejected `mode & FMOD_OPENMEMORY` outright, which
 * is every single sound effect -- that, not anything upstream, is why it was
 * silent. log104/log105 prove the game asks correctly: 40 CreateSound, 20
 * CreateStream, 40 PlaySound, and Demand succeeding 40/40.
 *
 * ---- ABI WARNING -------------------------------------------------------------
 * These are C++ methods called from a SOFTFP .so into our HARDFP build. Every
 * method taking a float BY VALUE (setVolume, setPan, setFrequency) must declare
 * that parameter as uint32_t and bit-reinterpret -- declaring `float` would read
 * s0 while the caller passed the bits in a core register. Same trap as the GL
 * shims. Pointer and
 * integer parameters need no shim.
 *
 * The old stub's other sin was never writing its OUT-parameters -- `isPlaying(bool*)`
 * left the caller's bool untouched, and FMOD_System_Create never produced a system
 * handle at all, so FModAudioSystem::m_system was NULL for an entire run. Every
 * out-param here is written on every path.
 */

#include <vitasdk.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "config.h"
#include "audio_patch.h"
#include "audio_mp3.h"
#include "audio_ring.h"
#include "bigalloc.h"
#include "sdl_patch.h"
#include "log.h"

/* ---- output format -------------------------------------------------------
 * Sources are mono at 32000 or 22050 Hz (measured across the OBB). We mix to one
 * stereo port and resample per-source with a linear interpolator -- transparent
 * enough for 48 kbps voice and cheap enough to run per sample. */
#define OUT_RATE     48000
#define OUT_CH       2
#define OUT_GRAIN    1024                 /* frames per sceAudioOutOutput call */
/* FModAudioSystem::InitSystem asks for (arg + 5) channels and InitChannels then
 * walks getChannel(0 .. count-1), aborting all of sound if ANY call is not
 * FMOD_OK. With 2D+3D voices at 24+16 that is 45; keep comfortable headroom and
 * log what is actually requested. Mixing cost is per PLAYING channel, so a large
 * pool is nearly free. */
#define MAX_CHANNELS 96
/* The tail of g_chan is not playable: it is where a voice about to be stolen
 * parks its undelivered END so System::update can still hand it to the game.
 * See chan_retire. Reserving it inside g_chan (rather than a separate array)
 * keeps chan_valid -- and therefore getUserData/isPlaying, which the game calls
 * from inside HandleChannelEnd -- working on a retired voice for free. */
#define RETIRE_SLOTS      32
#define MAX_PLAY_CHANNELS (MAX_CHANNELS - RETIRE_SLOTS)
#define MAX_SOUNDS   128

/* Streams decode whole: a 4-minute track at 32 kHz mono is ~15 MB of PCM. The
 * heap is 192 MB and shared with the game, so cap total decoded audio and refuse
 * politely past it rather than failing an allocation somewhere unrelated.
 *
 * This is the HARD ceiling: past it a sound is refused. It is deliberately not
 * the same number as what we retain for reuse -- see PCM_CACHE_KEEP. */
#define PCM_BUDGET_BYTES (40u * 1024u * 1024u)
/* Retention budget: how much decoded PCM we KEEP once nothing is playing it.
 *
 * log142 crashed at t=921 on a 2.5 MB allocation with the arena pinned at 190.6
 * of 192 MB, and the arena is what kills us -- newlib grows it via sbrk whenever
 * a request will not fit and never gives it back. It climbed in lockstep with
 * this cache: arena 130 MB at t=68 with 4.5 MB cached, 190 MB at t=441 with
 * 27.7 MB cached. Roughly 23 of those 60 MB were decoded audio we were holding
 * on the chance of a re-request.
 *
 * That chance is not worth 27 MB of arena. The new-handler wiped the entire
 * cache twice in the same run (25.0 MB at t=455, 21.0 MB at t=822) and the hit
 * rate climbed straight through both purges -- 86% -> 88% -> 89%, finishing at
 * 1777 hits / 211 misses with zero cache-full drops. Two total wipes cost
 * nothing measurable, so a working set far smaller than 40 MB is clearly enough.
 *
 * Soft on purpose: cache_insert trims TOWARD this and never refuses because of
 * it. Entries a live Sound still references cannot be evicted, and a burst of
 * simultaneous sounds must not start dropping audio -- only PCM_BUDGET_BYTES
 * above refuses. */
#define PCM_CACHE_KEEP   (8u * 1024u * 1024u)
/* Per-stream ceiling. A whole 88 s track at 44100 stereo is ~15 MB; anything
 * above this becomes timed silence rather than a multi-second stall plus an
 * allocation the heap cannot take. Short VO/ambient (~1.3 MB) is unaffected. */
#define STREAM_PCM_MAX   (6u * 1024u * 1024u)

/* ---- incremental streams --------------------------------------------------
 * Above STREAM_PCM_MAX an asset is streamed rather than silenced. It costs its
 * compressed bytes (~1.4 MB for a music track) plus this ring, instead of the
 * 14-24 MB the whole waveform would take.
 *
 * RING_FRAMES is the slack between the decoder and the mixer: 32768 frames is
 * 0.74 s at 44.1 kHz and costs 128 KB at stereo. FEED_MAX_FRAMES bounds how much
 * one grain may decode, so a refill can never overrun the output deadline -- the
 * mixer consumes OUT_GRAIN (1024) frames per grain, so 4096 keeps the ring ahead
 * with room to recover after a stall. */
#define RING_FRAMES      32768u
#define FEED_MAX_FRAMES  4096u

typedef struct {
  AudioMp3Stream *dec;
  void     *src;        /* owned compressed ES from big_malloc; decoder reads it */
  int16_t  *ring_buf;   /* RING_FRAMES * ch int16 units */
  AudioRing ring;
  int       loop;       /* honour FMOD_LOOP_NORMAL; the game sends LOOP_OFF */
  int       hw;         /* holds a sceAudiodec handle (MP3) vs software (ADPCM) */
  unsigned  unders;     /* mixer wanted a frame this stream could not supply */
  char      name[32];   /* asset basename, for the finish line below */
  /* The ONE channel this stream feeds. One decoder means one read position, so
   * two channels on one stream cannot both be served: retiring by the minimum
   * of their positions freezes the window between them and starves both (the
   * reader behind on every sample, the reader ahead on most). playSound claims
   * the stream for its channel and rewinds; any other channel gets silence
   * rather than wedging the one that is actually audible. */
  void     *reader;
} Stream;

/* ---- decoded-PCM cache ----------------------------------------------------
 * The game re-creates a Sound for the same asset constantly: in log110 ONE clip
 * was decoded 438 times and the whole run did 2569 decodes, ~12 a second. Each
 * was a malloc + full decode, which is what made the game stutter. Decoded PCM
 * is now shared between Sound handles, keyed by the source bytes, and only freed
 * when nothing references it and we need the room. */
#define MAX_CACHE 192

typedef struct {
  int      used, refs;
  unsigned stamp;                          /* for LRU eviction */
  unsigned key_len;
  uint32_t key_hash;
  AudioPcm pcm;
} PcmEntry;

typedef struct {
  int       used;
  int       is3d;                          /* created with FMOD_3D: attenuates */
  PcmEntry *ent;                           /* owns a reference */
  AudioPcm  pcm;                           /* borrowed copy of ent->pcm */
  Stream   *st;                            /* non-NULL => streamed, pcm.pcm NULL */
} Snd;

/* The END callback the companion registers on every voice. All parameters are
 * integers or pointers, so unlike setVolume/setPan this needs no softfp shim. */
typedef int (*chan_cb)(void *chanctl, int controltype, int callbacktype,
                       void *cmd1, void *cmd2);

typedef struct {
  int    used;
  unsigned stamp;                         /* allocation order, for voice stealing */
  Snd   *snd;
  double pos;                             /* fractional source frame */
  double step;                            /* source frames per output frame */
  float  vol, pan;
  int    playing, paused;
  void  *userdata;
  chan_cb cb;                             /* FModAudioSystem::ChannelCallback */
  int    end_pending;                     /* finished naturally, END not yet sent */
  float  px, py, pz;                      /* emitter position, world units */
  float  mindist, maxdist;                /* rolloff range; FMOD defaults 1 / 10000 */
  float  occl;                            /* direct occlusion, 0 = clear path */
  int    has_pos;                         /* set3DAttributes has been called */
} Chan;

static Snd      g_snd[MAX_SOUNDS];
static Chan     g_chan[MAX_CHANNELS];
static PcmEntry g_cache[MAX_CACHE];
static unsigned g_clock = 0;
static unsigned g_cache_hits = 0, g_cache_miss = 0;

static SceUID   g_mutex   = -1;
static SceUID   g_thread  = -1;
static int      g_port    = -1;
static int      g_running = 0;
static int      g_ready   = 0;
static unsigned g_pcm_bytes = 0;
static int      g_nchannels = MAX_CHANNELS;   /* pool size the game asked for */

static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline void  lock(void)   { if (g_mutex >= 0) sceKernelLockMutex(g_mutex, 1, NULL); }
static inline void  unlock(void) { if (g_mutex >= 0) sceKernelUnlockMutex(g_mutex, 1); }

static unsigned pcm_bytes_of(const AudioPcm *p) {
  return p->pcm ? p->nsamples * p->channels * 2u : 0u;   /* silence holds no PCM */
}

/* Cheap content key: length plus an FNV-1a over the head and tail. Hashing the
 * whole buffer would cost as much as the decode we are trying to avoid, and a
 * length + 512 sampled bytes is ample to tell KOTOR's sound effects apart. */
static uint32_t key_hash(const void *p, unsigned len) {
  const unsigned char *b = (const unsigned char *)p;
  uint32_t h = 2166136261u;
  unsigned n = len < 256 ? len : 256;
  for (unsigned i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
  if (len > 256) {
    unsigned start = len - (len - 256 < 256 ? len - 256 : 256);
    for (unsigned i = start; i < len; i++) { h ^= b[i]; h *= 16777619u; }
  }
  return h ^ len;
}

/* All cache helpers below assume the caller holds the lock. */
static PcmEntry *cache_find(unsigned len, uint32_t h) {
  for (int i = 0; i < MAX_CACHE; i++)
    if (g_cache[i].used && g_cache[i].key_len == len && g_cache[i].key_hash == h)
      return &g_cache[i];
  return NULL;
}

/* Drop the least-recently-used unreferenced entry. Returns 0 if nothing can go. */
static int cache_evict_one(void) {
  PcmEntry *best = NULL;
  for (int i = 0; i < MAX_CACHE; i++) {
    PcmEntry *e = &g_cache[i];
    if (!e->used || e->refs > 0) continue;
    if (!best || e->stamp < best->stamp) best = e;
  }
  if (!best) return 0;
  unsigned bytes = pcm_bytes_of(&best->pcm);
  g_pcm_bytes = (g_pcm_bytes > bytes) ? g_pcm_bytes - bytes : 0;
  audio_pcm_free(&best->pcm);
  memset(best, 0, sizeof *best);
  return 1;
}

/* Takes ownership of *pcm on success (and frees nothing on failure). */
static PcmEntry *cache_insert(unsigned len, uint32_t h, const AudioPcm *pcm) {
  unsigned bytes = pcm_bytes_of(pcm);
  /* Trim retained PCM toward the keep budget, best effort: stop as soon as
   * nothing more can go rather than failing. Referenced entries are unevictable
   * and refusing on their account would drop a sound the game is asking to play
   * right now, which is a worse bug than holding a few MB too long. */
  while (g_pcm_bytes + bytes > PCM_CACHE_KEEP && cache_evict_one())
    ;
  /* The hard ceiling still refuses -- that is what stops one runaway decode from
   * taking the heap with it. */
  if (g_pcm_bytes + bytes > PCM_BUDGET_BYTES) return NULL;

  PcmEntry *slot = NULL;
  for (int i = 0; i < MAX_CACHE; i++)
    if (!g_cache[i].used) { slot = &g_cache[i]; break; }
  if (!slot) { if (!cache_evict_one()) return NULL;
               for (int i = 0; i < MAX_CACHE; i++)
                 if (!g_cache[i].used) { slot = &g_cache[i]; break; } }
  if (!slot) return NULL;

  slot->used = 1; slot->refs = 1; slot->stamp = ++g_clock;
  slot->key_len = len; slot->key_hash = h; slot->pcm = *pcm;
  g_pcm_bytes += bytes;
  return slot;
}

static void cache_release(PcmEntry *e) {
  if (e && e->refs > 0) e->refs--;
}

/* ---- streaming ------------------------------------------------------------
 * Live streaming decoders. Each holds one sceAudiodec handle for the life of a
 * track, and the pool is fixed at InitLibrary. Keeping this strictly below
 * AUDIO_MP3_DECODER_POOL guarantees a free slot for the short synchronous
 * decodes -- VO and effects -- so a stream can never silence dialogue, even if
 * one is somehow leaked.
 *
 * Mutated only under the mixer lock. */
static int      g_stream_decoders = 0;
static unsigned g_streams_open = 0;        /* lifetime count, for the log */
static unsigned g_stream_underruns = 0;    /* mixer wanted a frame we lacked */

/* The ring's decoder end. Looping is honoured but the game passes LOOP_OFF and
 * drives its own music playlist -- looping here would mean a track could never
 * end and the area's music would never change. */
static unsigned stream_fill_cb(void *ctx, int16_t *dst, unsigned frames,
                               int *eos_out) {
  Stream *st = (Stream *)ctx;
  unsigned got = audio_mp3_stream_read(st->dec, dst, frames);
  if (!got && audio_mp3_stream_eos(st->dec)) {
    if (st->loop) {
      audio_mp3_stream_rewind(st->dec);
      got = audio_mp3_stream_read(st->dec, dst, frames);
      if (!got) *eos_out = 1;
    } else {
      *eos_out = 1;
    }
  }
  return got;
}

/* Takes ownership of `src_owned` on success. `es`/`len` name the MP3 bytes
 * inside it. Caller must hold NO lock: this allocates and touches hardware. */
static Stream *stream_open(void *src_owned, const void *es, unsigned len,
                           AudioPcm *fmt, int loop) {
  /* Only hardware-decoded streams are capped: the cap exists to keep a
   * sceAudiodec handle free for the short synchronous decodes, and a software
   * ADPCM stream takes none. */
  int needs_hw = audio_mp3_stream_needs_hw(es, len);
  if (needs_hw) {
    int live;
    lock();
    live = g_stream_decoders;
    unlock();
    if (live >= AUDIO_MP3_STREAM_MAX) {
      log_printf("[snd] stream decoder cap reached (%d of pool %d) -- not streaming this one",
                 live, AUDIO_MP3_DECODER_POOL);
      return NULL;
    }
  }

  Stream *st = (Stream *)calloc(1, sizeof *st);
  if (!st) return NULL;

  st->dec = audio_mp3_stream_open(es, len, fmt);
  if (!st->dec) { free(st); return NULL; }
  st->hw = needs_hw;

  unsigned ch = (fmt && fmt->channels) ? fmt->channels : 1;
  st->src      = src_owned;
  st->loop     = loop;
  st->ring_buf = (int16_t *)big_malloc((size_t)RING_FRAMES * ch * sizeof(int16_t));
  if (!st->ring_buf) {
    audio_mp3_stream_close(st->dec);
    free(st);
    return NULL;
  }
  audio_ring_init(&st->ring, st->ring_buf, RING_FRAMES, ch);

  lock();
  if (st->hw) g_stream_decoders++;
  g_streams_open++;
  unlock();
  return st;
}

/* Caller must hold NO lock, and must already have detached every channel and
 * cleared the owning Snd's `st` under the lock -- see Snd_release.
 *
 * Both buffers go back through big_free: they came from big_malloc and at
 * 1.4 MB always land in the pool, and only the game's imported free does the
 * address-range check that tells pool pointers from newlib's. */
static void stream_close(Stream *st) {
  if (!st) return;
  if (st->dec) {
    int hw = st->hw;
    audio_mp3_stream_close(st->dec);
    st->dec = NULL;
    if (hw) {
      lock();
      g_stream_decoders--;                 /* the handle went back to the pool */
      unlock();
    }
  }
  big_free(st->ring_buf);
  big_free(st->src);
  free(st);
}

unsigned audio_cache_bytes(void) { return g_pcm_bytes; }

/* Called from the new-handler when the heap is exhausted. Everything here is a
 * pure speed optimisation -- worst case the next createSound decodes again --
 * so give all of it back rather than let an allocation fail. Entries a live
 * Sound still references stay. Safe to call from any thread: nothing under this
 * lock allocates, so it cannot re-enter the handler. */
unsigned audio_cache_purge(void) {
  unsigned freed = 0;
  lock();
  for (int i = 0; i < MAX_CACHE; i++) {
    PcmEntry *e = &g_cache[i];
    if (!e->used || e->refs > 0) continue;
    unsigned bytes = pcm_bytes_of(&e->pcm);
    g_pcm_bytes = (g_pcm_bytes > bytes) ? g_pcm_bytes - bytes : 0;
    audio_pcm_free(&e->pcm);
    memset(e, 0, sizeof *e);
    freed += bytes;
  }
  unlock();
  return freed;
}

/* ---- end-of-sound notification --------------------------------------------
 * FModAudioSystem does NOT poll FMOD to find out whether a voice is still
 * going: FModAudioSystem::PlaySound (+0x735a1) registers ChannelCallback and
 * then GetIsChannelPlaying (+0x746a5) just reads ChannelInfo+0x1c, a flag whose
 * only writer is HandleChannelEnd, reached only from that callback with
 * FMOD_CHANNELCONTROL_CALLBACK_END. Stubbing setCallback therefore told the
 * game that every sound it had ever started was still playing, forever:
 * channel slots were never recycled (playSound dried up after ~2 minutes),
 * streams were never closed (log139: 216 opened, 144 closed, then "out of
 * virtual handles for main.obb" from t=1088s), and the leaked sources ate the
 * heap until every decode failed and became silence (t=1307s) and the game
 * fell over. It is also why looping ambience only ever played once --
 * HandleChannelEnd's other branch is the loop restart.
 *
 * Flag the completion in the mixer, deliver it from System::update on the
 * game's own thread. Never deliver it from the audio thread: HandleChannelEnd
 * calls playSound/setUserData/setCallback straight back into us. */
static unsigned g_ends_fired = 0, g_updates = 0;

/* Caller holds the lock. `end_pending` is set even when no callback is
 * installed yet: a 3 ms effect can finish before the companion gets back from
 * playSound to register one, and the flag simply waits for it. */
static void chan_finish(Chan *c) {
  c->playing     = 0;
  c->end_pending = 1;
}

/* ---- 3D attenuation --------------------------------------------------------
 * All four FMOD 3D entry points used to be fmod_stub, so nothing in the game's
 * spatial audio reached the mixer: every positional source played at whatever
 * channel volume it was given, with no rolloff and no pan. On hardware that is
 * rushing water across the level sitting at the same level as water underfoot,
 * footsteps and dialogue buried under ambience, and no directional cue at all.
 * It is not subtle -- log155 marks 418 of 617 logged sounds FMOD_3D.
 *
 * FMOD Ex's default rolloff is inverse: gain = mindistance / distance, held at
 * 1 inside mindistance, and -- this part is easy to get wrong -- NOT silent past
 * maxdistance. maxdistance is where attenuation STOPS, so the gain floors at
 * mindistance/maxdistance and stays there. Match that rather than inventing a
 * curve, because the game picks its min/max per source expecting it.
 *
 * 2D sounds (music, UI, the global ambient bed) are deliberately untouched:
 * they carry no position and are meant to play flat. */
static float g_lis_px = 0.0f, g_lis_py = 0.0f, g_lis_pz = 0.0f;
static float g_lis_rx = 1.0f, g_lis_ry = 0.0f, g_lis_rz = 0.0f;   /* right vector */
static unsigned g_lis_sets = 0, g_pos_sets = 0, g_mm_sets = 0;
/* log159's first listener update carried fwd=(0,0,0) up=(0,0,0), so the cross
 * product was degenerate and the right vector stayed at its (1,0,0) default --
 * a stereo image nailed to a fixed world axis that never turns with the camera.
 * Whether that persists is unknown, because the probe logged only the first
 * call and then went silent, which was a bad probe.
 *
 * Until a valid basis arrives, do not pan at all. A centred image is merely
 * missing information; panning off an arbitrary axis is confidently wrong, and
 * on headphones that is far more disturbing than mono. */
static int      g_lis_basis_ok = 0;
static unsigned g_lis_degenerate = 0;
/* Gain census: is attenuation reasonable, or is it burying everything? */
static unsigned g_g3_n = 0, g_g3_loud = 0, g_g3_quiet = 0;
static float    g_g3_min = 1.0f, g_g3_max = 0.0f, g_dist_max = 0.0f;
/* The quietest attenuation of the session, with the numbers that produced it.
 * log169 raised a question the census could not answer: dialogue was quiet in
 * one spot on Dantooine and normal moments later. "81 gains below 0.05" does
 * not say whether we attenuated a line into the floor or whether that take is
 * simply quiet -- for that you need the distance and the min/max the game set,
 * which is what FMOD's inverse rolloff (gain = mindistance / distance) is
 * actually made of. Three floats, no per-call logging. */
static float    g_g3_worst = 1.0f, g_g3_worst_d = 0.0f;
static float    g_g3_worst_mn = 0.0f, g_g3_worst_mx = 0.0f;
/* Volume census. The mixer computes v = ch->vol * g3, and nothing has ever
 * logged ch->vol -- so "3D sounds are too quiet" has stayed a hypothesis. The
 * discriminator is whether the 3D volumes VARY: a spread means the game is
 * attenuating by distance itself and we are doubling it, while a single
 * constant means the game expects FMOD's 3D system to supply the rest and we
 * are taking its base literally. min == max answers that on its own. */
static unsigned g_vol_n2d = 0, g_vol_n3d = 0;
static float    g_vol_min2d = 1.0f, g_vol_max2d = 0.0f, g_vol_sum2d = 0.0f;
static float    g_vol_min3d = 1.0f, g_vol_max3d = 0.0f, g_vol_sum3d = 0.0f;
/* What actually reaches the bus, per channel per grain, after 3D gain. */
static unsigned g_mix_n2d = 0, g_mix_n3d = 0;
static float    g_mix_sum2d = 0.0f, g_mix_sum3d = 0.0f;

/* Caller holds the lock. Returns linear gain and writes a pan in [-1,1]. */
static float chan_3d_gain(const Chan *c, float *pan_out) {
  *pan_out = c->pan;
  if (!AUDIO_3D_ATTENUATION) return 1.0f;
  if (!c->snd || !c->snd->is3d || !c->has_pos) return 1.0f;

  float dx = c->px - g_lis_px, dy = c->py - g_lis_py, dz = c->pz - g_lis_pz;
  float d2 = dx * dx + dy * dy + dz * dz;
  float d  = (d2 > 0.0f) ? sqrtf(d2) : 0.0f;

  float mn = (c->mindist > 0.0f) ? c->mindist : 1.0f;
  float mx = (c->maxdist > mn)   ? c->maxdist : 10000.0f;
  float dd = d < mn ? mn : (d > mx ? mx : d);
  float g  = mn / dd;

  if (c->occl > 0.0f) g *= (1.0f - (c->occl > 1.0f ? 1.0f : c->occl));

  /* Census the spread so the next log can say whether this is sane. */
  g_g3_n++;
  if (g < g_g3_min) g_g3_min = g;
  if (g > g_g3_max) g_g3_max = g;
  if (d > g_dist_max) g_dist_max = d;
  if (g >= 0.5f) g_g3_loud++; else if (g < 0.05f) g_g3_quiet++;
  if (g < g_g3_worst) {
    g_g3_worst = g; g_g3_worst_d = d; g_g3_worst_mn = mn; g_g3_worst_mx = mx;
  }

  /* Pan by projecting the direction onto the listener's right vector. At the
   * listener's own position there is no direction, so stay centred; and with no
   * valid basis yet, panning would be off an arbitrary axis, so stay centred. */
  if (g_lis_basis_ok && d > 0.0001f) {
    float p = (dx * g_lis_rx + dy * g_lis_ry + dz * g_lis_rz) / d;
    if (p < -1.0f) p = -1.0f; else if (p > 1.0f) p = 1.0f;
    *pan_out = p;
  }
  return g;
}

/* ---- mixer ---------------------------------------------------------------- */
/* Limiter state: a global gain the mix rides instead of clipping. */
static float    g_lim_gain = 1.0f, g_lim_min = 1.0f;
static unsigned g_lim_grains = 0, g_lim_clipped = 0;
static int32_t  g_lim_peak = 0;

static int32_t g_acc[OUT_GRAIN * OUT_CH];
static int16_t g_out[OUT_GRAIN * OUT_CH];

static void mix_grain(void) {
  memset(g_acc, 0, sizeof g_acc);

  lock();

  /* Refill streams before mixing, retiring what the stream's one reader has
   * already passed. A stream with no live reader is still topped up, so it is
   * ready the moment the game unpauses it. The feeder runs under this lock on
   * purpose: it is what makes closing a stream from Snd_release safe.
   * FEED_MAX_FRAMES is what keeps the hold time short. */
  for (int i = 0; i < MAX_SOUNDS; i++) {
    Snd *s = &g_snd[i];
    if (!s->used || !s->st) continue;
    /* Retire by the ONE reader, never by a minimum over several: the window
     * only moves forward, so a second channel sitting behind it would pin it
     * there and starve the channel that is actually playing. */
    Chan *rd = (Chan *)s->st->reader;
    int live = rd && rd->used && rd->playing && rd->snd == s;
    if (live) audio_ring_retire(&s->st->ring, (uint64_t)rd->pos);
    audio_ring_feed(&s->st->ring, stream_fill_cb, s->st, FEED_MAX_FRAMES);

    /* Underruns are counted per sample, so they can only be reported in bulk --
     * and log168 had 5.4 million of them arriving in bursts with no way to say
     * WHICH stream, or why. Two readers pinning the window between them used to
     * be one of the answers; the single-reader claim above makes that case
     * structurally impossible, so what is left is the reader's position against
     * the window, which is the whole diagnosis:
     *
     *   pos BEHIND base -- the reader is reading frames already retired, which
     *   it can never get back. That was log170-172: a replayed track started at
     *   frame 0 against a window sitting at the end of the asset, and underran
     *   on every sample for exactly one track length. Must not happen now.
     *
     *   pos AHEAD of base+fill -- the decoder genuinely is not keeping up, and
     *   the window is starved rather than stale.
     *
     * Print the position and the window rather than a spread, so those two are
     * never again indistinguishable. Rate-limited to roughly one line every two
     * seconds per stream. */
    if (s->st->unders) {
      static unsigned last_report = 0;
      if (g_lim_grains - last_report >= 96) {
        last_report = g_lim_grains;
        uint64_t pos  = live ? (uint64_t)rd->pos : 0;
        uint64_t base = s->st->ring.base;
        log_printf("[snd] stream UNDERRUN \"%.31s\": %u samples, reader %s, "
                   "pos %u vs window %u..%u of ring %u%s",
                   s->st->name, s->st->unders,
                   !live ? "none" : (pos < base ? "BEHIND (stale)" : "ahead (starved)"),
                   (unsigned)pos, (unsigned)base,
                   (unsigned)(base + s->st->ring.fill), RING_FRAMES,
                   s->st->ring.eos ? " (eos)" : "");
        s->st->unders = 0;
      }
    }
  }

  for (int c = 0; c < g_nchannels; c++) {
    Chan *ch = &g_chan[c];
    if (!ch->used || !ch->playing || ch->paused || !ch->snd) continue;
    Snd *s = ch->snd;

    if (s->st) {                      /* streamed: read the decoded window */
      /* Only the channel that claimed this stream may read it. A second one
       * cannot be served -- there is a single decoder and a single window -- so
       * end it rather than let it starve, which also delivers its END and keeps
       * the game's channel bookkeeping honest. */
      if (s->st->reader != ch) { chan_finish(ch); continue; }
      AudioRing *ring = &s->st->ring;
      float pan;
      float g3 = chan_3d_gain(ch, &pan);
      float v  = ch->vol * g3;
      if (s->is3d) { g_mix_n3d++; g_mix_sum3d += v; }
      else         { g_mix_n2d++; g_mix_sum2d += v; }
      float gl = v * (pan <= 0.0f ? 1.0f : 1.0f - pan);
      float gr = v * (pan >= 0.0f ? 1.0f : 1.0f + pan);

      for (int i = 0; i < OUT_GRAIN; i++) {
        uint64_t i0 = (uint64_t)ch->pos;
        float l0, r0, l1, r1;
        if (!audio_ring_frame(ring, i0, &l0, &r0) ||
            !audio_ring_frame(ring, i0 + 1, &l1, &r1)) {
          /* Outside the decoded window. Finish ONLY if the decoder is done AND
           * the window really is drained: eos is set the moment the decoder hits
           * the end of the file, while up to RING_FRAMES of already-decoded
           * audio may still be waiting to be mixed. Finishing on eos alone would
           * cut the last 0.7 s off every single track.
           *
           * Otherwise this is a refill underrun: emit nothing for this sample
           * but keep the clock moving, so a dropout cannot become drift. */
          if (ring->eos && i0 + 1 >= ring->base + (uint64_t)ring->fill) {
            /* A track reaching its end is the handoff point for the game's music
             * director: it plays LOOP_OFF and is supposed to queue the next one
             * off the END callback. In log166 the last music stream was created
             * at t=302 for an 87.8 s track, 2D voices per grain collapsed at
             * t=390 -- exactly 302+87.8 -- and no CreateStream ever followed, so
             * the rest of the session was silent. Printing the end of every
             * track makes that gap measurable against the next CreateStream
             * instead of inferred from a mixing average. Once per track. */
            unsigned rate = s->pcm.rate ? s->pcm.rate : 1;
            log_printf("[snd] stream FINISHED \"%.31s\" after %u ms "
                       "(chan %d, %u decoded frames) -- END now owed to the game",
                       s->st->name, (unsigned)(i0 * 1000ull / rate), c,
                       (unsigned)(ring->base + ring->fill));
            chan_finish(ch);
            break;
          }
          ch->pos += ch->step;
          g_stream_underruns++;
          s->st->unders++;
          continue;
        }
        float frac = (float)(ch->pos - (double)i0);
        float l = l0 + (l1 - l0) * frac;
        float r = r0 + (r1 - r0) * frac;
        g_acc[i * 2]     += (int32_t)(l * gl);
        g_acc[i * 2 + 1] += (int32_t)(r * gr);
        ch->pos += ch->step;
      }
      continue;
    }

    const int16_t *src = s->pcm.pcm;
    unsigned n = s->pcm.nsamples, sch = s->pcm.channels;
    if (!n) { chan_finish(ch); continue; }
    if (!src) {                       /* silent placeholder: keep time, emit nothing */
      ch->pos += ch->step * (double)OUT_GRAIN;
      if (ch->pos >= (double)n) chan_finish(ch);
      continue;
    }

    /* pan -1 = hard left, +1 = hard right */
    float pan;
    float g3 = chan_3d_gain(ch, &pan);
    float v  = ch->vol * g3;
    if (s->is3d) { g_mix_n3d++; g_mix_sum3d += v; }
    else         { g_mix_n2d++; g_mix_sum2d += v; }
    float gl = v * (pan <= 0.0f ? 1.0f : 1.0f - pan);
    float gr = v * (pan >= 0.0f ? 1.0f : 1.0f + pan);

    for (int i = 0; i < OUT_GRAIN; i++) {
      unsigned i0 = (unsigned)ch->pos;
      if (i0 + 1 >= n) { chan_finish(ch); break; }
      float frac = (float)(ch->pos - (double)i0);

      float l, r;
      if (sch == 1) {
        float a = src[i0], b = src[i0 + 1];
        l = r = a + (b - a) * frac;
      } else {
        float a0 = src[i0 * 2],     b0 = src[(i0 + 1) * 2];
        float a1 = src[i0 * 2 + 1], b1 = src[(i0 + 1) * 2 + 1];
        l = a0 + (b0 - a0) * frac;
        r = a1 + (b1 - a1) * frac;
      }
      g_acc[i * 2]     += (int32_t)(l * gl);
      g_acc[i * 2 + 1] += (int32_t)(r * gr);
      ch->pos += ch->step;
    }
  }
  unlock();

  /* Peak limiter.
   *
   * The mix used to be a raw sum hard-clipped at full scale, with every channel
   * free to contribute a full-scale sample of its own. log160 had 15 voices
   * playing at once with 83% of computed gains at or above 0.5, so the sum ran
   * as much as an order of magnitude over the ceiling and simply squared off:
   * once the loudest source saturates the bus, everything quieter under it is
   * gone. That is exactly the hardware report -- environmental loops, which run
   * continuously, drowning footsteps, menu clicks and combat, which do not.
   *
   * Attenuation could not fix this. It reduces each voice, but the sum was over
   * by a factor, not by a margin, and clipping destroys the quiet sources
   * regardless of how loud the loud ones are.
   *
   * So ride a global gain instead of clipping. Attack is instantaneous, because
   * a limiter that lets even one sample through has not limited anything;
   * release is slow enough (~1 s at this grain) that a loud burst does not pump
   * the whole mix around it. Nothing is clipped in normal operation, and the
   * balance between voices is preserved rather than flattened. */
  int32_t peak = 0;
  for (int i = 0; i < OUT_GRAIN * OUT_CH; i++) {
    int32_t a = g_acc[i] < 0 ? -g_acc[i] : g_acc[i];
    if (a > peak) peak = a;
  }
  float target = 1.0f;
  if (peak > 32767) {
    target = 32767.0f / (float)peak;
    g_lim_clipped++;
    if (peak > g_lim_peak) g_lim_peak = peak;
  }
  if (target < g_lim_gain) g_lim_gain = target;              /* instant attack */
  else g_lim_gain += (target - g_lim_gain) * 0.02f;          /* gentle release */
  if (g_lim_gain < g_lim_min) g_lim_min = g_lim_gain;
  g_lim_grains++;

  for (int i = 0; i < OUT_GRAIN * OUT_CH; i++) {
    int32_t v = (int32_t)((float)g_acc[i] * g_lim_gain);
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    g_out[i] = (int16_t)v;
  }
}

static int audio_thread(SceSize args, void *argp) {
  (void)args; (void)argp;
  while (g_running) {
    mix_grain();
    sceAudioOutOutput(g_port, g_out);   /* blocks until the grain is consumed */
  }
  return 0;
}

static void audio_start(void) {
  if (g_ready) return;
  g_ready = 1;                            /* set first: never retry a failed open */
  g_mutex = sceKernelCreateMutex("kotor_snd", 0, 0, NULL);
  g_port  = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, OUT_GRAIN,
                                OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
  if (g_port < 0) {
    log_printf("[snd] sceAudioOutOpenPort failed 0x%08X -- staying silent",
               (unsigned)g_port);
    return;
  }
  int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
  sceAudioOutSetVolume(g_port,
                       SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
  audio_mp3_init_library();
  g_running = 1;
  g_thread = sceKernelCreateThread("kotor_snd", audio_thread, 0x10000100, 0x10000,
                                   0, 0, NULL);
  if (g_thread >= 0) sceKernelStartThread(g_thread, 0, NULL);
  log_printf("[snd] output up: port=%d %dHz stereo grain=%d", g_port, OUT_RATE, OUT_GRAIN);
}

/* ---- handle helpers ------------------------------------------------------- */
static Snd *snd_alloc(void) {
  for (int i = 0; i < MAX_SOUNDS; i++)
    if (!g_snd[i].used) { memset(&g_snd[i], 0, sizeof g_snd[i]); g_snd[i].used = 1; return &g_snd[i]; }
  return NULL;
}
/* A channel that finishes playing has `playing == 0` but stays `used` -- only
 * Ch_stop and Sound::release ever cleared `used`, and the game does not always
 * do either for one-shots. So the 45-voice pool filled up, chan_alloc started
 * returning NULL, and every sound after that was silently dropped: exactly the
 * "some sounds stop working, and it gets worse the longer you play" report.
 *
 * Rather than free on completion (the game may still query the handle), do what
 * real FMOD does under voice pressure: take a free slot, else steal the
 * oldest FINISHED voice, else the oldest playing one. */
static unsigned g_chan_stamp = 0, g_steals = 0, g_steals_live = 0;
static unsigned g_ends_rescued = 0, g_ends_lost = 0;
/* log154's ratchet: playSound outran END delivery by 28 at t=134 and 163 at
 * t=949, monotonically, and the deficit never once fell. At t=854 the pool
 * pinned at 45 of 45 used with ~10 playing, and playSound collapsed from ~6/s
 * to ~0.4/s while createSound held its full rate -- the game kept building
 * sounds and stopped playing them, so its own voice bookkeeping is wedged.
 *
 * chan_alloc cannot fail (it steals), and the only other way out of playSound
 * is !snd_valid, so we are not refusing the game: it stopped asking. The one
 * uncounted path that can wedge a CExoSoundSource is right below in
 * Snd_release, which clears `playing`, `end_pending` and `cb` together on
 * every channel still holding the released Sound. A voice killed there while
 * still playing never raised END at all, and HandleChannelEnd is the only
 * writer of ChannelInfo+0x1c.
 *
 * Whether that is the leak depends on something we cannot see from here --
 * whether the game's ReleaseSound resets ChannelInfo itself, the way
 * StopChannel does. Firing END for a voice it has already reset would be a
 * duplicate, and HandleChannelEnd's other branch is the loop restart, so
 * count first and only then decide whether to re-home these. If the killed
 * total tracks the deficit, this is it. */
static unsigned g_rel_calls = 0, g_rel_kill_live = 0, g_rel_kill_pend = 0;
static unsigned g_play_calls = 0, g_play_badsnd = 0, g_play_nochan = 0;

/* Stealing a voice must not silently swallow an END it still owes.
 *
 * KOTOR never polls FMOD to learn that a sound finished: FModAudioSystem reads
 * ChannelInfo+0x1c, and the only writer of that flag is HandleChannelEnd, off
 * the END callback. A voice that has finished but has not been drained by
 * System::update yet is still holding `end_pending`; memsetting it on steal
 * destroys the one notification its owner will ever get, and that
 * CExoSoundSource is then wedged in "still playing" for the rest of the session.
 *
 * Park the callback and userdata in a reserved slot instead and let
 * System::update deliver it from the game's own thread as usual. The slot lives
 * inside g_chan, so getUserData -- which the game calls from inside
 * HandleChannelEnd -- still resolves the ORIGINAL owner.
 *
 * Deliberately narrow: only a still-pending END is re-homed. Sending one for a
 * voice that has ALREADY been drained would be a SECOND END for that source,
 * and HandleChannelEnd's other branch is the loop restart -- a duplicate there
 * brings back ambience the game believes it already has. Whether this window is
 * wide enough to explain log4's decay is what `END on steal / lost` is there to
 * answer; see the note on g_played in the stats line. */
static void chan_retire(Chan *c) {
  if (!c->used || !c->end_pending || !c->cb) return;   /* nothing owed to anyone */
  for (int i = MAX_PLAY_CHANNELS; i < MAX_CHANNELS; i++) {
    Chan *r = &g_chan[i];
    if (r->used) continue;
    memset(r, 0, sizeof *r);
    r->used        = 1;
    r->playing     = 0;
    r->cb          = c->cb;
    r->userdata    = c->userdata;
    r->end_pending = 1;
    g_ends_rescued++;
    return;
  }
  g_ends_lost++;                              /* ring full: same old silent loss */
}

static Chan *chan_take(Chan *c) {
  memset(c, 0, sizeof *c);
  c->used  = 1;
  c->stamp = ++g_chan_stamp;
  return c;
}

static Chan *chan_alloc(void) {
  Chan *oldest_done = NULL, *oldest_live = NULL;
  for (int i = 0; i < g_nchannels; i++) {
    Chan *c = &g_chan[i];
    if (!c->used) return chan_take(c);
    if (!c->playing) { if (!oldest_done || c->stamp < oldest_done->stamp) oldest_done = c; }
    else             { if (!oldest_live || c->stamp < oldest_live->stamp) oldest_live = c; }
  }
  if (oldest_done) { g_steals++;      chan_retire(oldest_done); return chan_take(oldest_done); }
  if (oldest_live) { g_steals_live++; chan_retire(oldest_live); return chan_take(oldest_live); }
  return NULL;
}
/* The game hands these pointers back to us; validate they are ours before use so
 * a stale or bogus handle cannot turn into a wild write. */
static int snd_valid(const void *p)  { return p >= (void *)g_snd  && p < (void *)(g_snd + MAX_SOUNDS); }
static int chan_valid(const void *p) { return p >= (void *)g_chan && p < (void *)(g_chan + MAX_CHANNELS); }

/* ---- FMOD_RESULT / FMOD_MODE ---------------------------------------------- */
#define FMOD_OK                 0
#define FMOD_ERR_FILE_NOTFOUND 18
#define FMOD_ERR_INVALID_PARAM 34
#define FMOD_ERR_MEMORY         9
#define FMOD_TIMEUNIT_MS   0x00000001
#define FMOD_TIMEUNIT_PCM  0x00000002
#define FMOD_LOOP_NORMAL   0x00000002
#define FMOD_CREATESTREAM  0x00000080
#define FMOD_OPENMEMORY    0x00000800
#define FMOD_2D            0x00000008
#define FMOD_3D            0x00000010

static int fmod_stub(void) { return FMOD_OK; }

/* ---- FMOD::System --------------------------------------------------------- */
/* The companion keeps this pointer as FModAudioSystem::m_system and passes it
 * back as `this` on every C++ call. Our methods ignore `self`, but it must be
 * non-NULL or the companion treats system creation as failed. */
static uint32_t g_system_obj = 0;

static int Sys_Create(void **out) {
  if (!out) return FMOD_ERR_INVALID_PARAM;
  *out = &g_system_obj;                   /* the out-param the old stub never wrote */
  return FMOD_OK;
}

static int Sys_init(void *self, int maxch, unsigned flags, void *extra) {
  (void)self; (void)flags; (void)extra;
  g_nchannels = (maxch > 0 && maxch < MAX_PLAY_CHANNELS) ? maxch : MAX_PLAY_CHANNELS;
  if (maxch > MAX_PLAY_CHANNELS)
    log_printf("[snd] WARNING: game asked for %d channels, pool is %d -- "
               "getChannel will fail past the end and abort InitChannels",
               maxch, g_nchannels);
  log_printf("[snd] System::init maxchannels=%d (pool %d)", maxch, g_nchannels);
  audio_start();
  return FMOD_OK;
}
static int Sys_getNumDrivers(void *self, int *n) { (void)self; if (n) *n = 1; return FMOD_OK; }

/* Real FMOD dispatches channel callbacks from System::update, on the caller's
 * thread, precisely so a callback may re-enter the API. The companion calls
 * this from FModAudioSystem::UpdateSystem (+0x72ecd), which libKOTOR drives
 * from its per-frame audio update -- the same thread that calls playSound, so
 * the re-entry HandleChannelEnd performs is on its home thread and our lock is
 * never held across it. */
#define FMOD_CHANNELCONTROL_CHANNEL      0
#define FMOD_CHANNELCONTROL_CALLBACK_END 0

static int Sys_update(void *self) {
  (void)self;
  static int draining = 0;
  g_updates++;
  if (draining) return FMOD_OK;         /* HandleChannelEnd -> playSound -> ... */
  draining = 1;
  /* Playable voices first, then the retirement ring -- a slot there exists only
   * to carry one END and is freed the moment it is delivered. */
  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (i >= g_nchannels && i < MAX_PLAY_CHANNELS) continue;   /* never allocated */
    Chan *c = &g_chan[i];
    lock();
    chan_cb cb = c->cb;
    int fire = c->end_pending && cb;
    if (fire) c->end_pending = 0;
    unlock();
    if (fire) {                          /* never under the lock */
      g_ends_fired++;
      cb(c, FMOD_CHANNELCONTROL_CHANNEL, FMOD_CHANNELCONTROL_CALLBACK_END, NULL, NULL);
      if (i >= MAX_PLAY_CHANNELS) { lock(); c->used = 0; c->cb = NULL; unlock(); }
    }
  }
  draining = 0;
  return FMOD_OK;
}

/* ---- streams arrive through FMOD's file callbacks, not by path --------------
 * FModAudioSystem::CreateStream builds a name with snprintf that is just a
 * DECIMAL ID ("20", "21", ...) and registers the matching SDL_RWops in its own
 * map. Its SystemOpenCallback (+0x72cfc) does strtoul(name) and looks the id up;
 * it never touches the userdata argument, so NULL is safe to pass.
 *
 * Stubbing setFileSystem meant we treated "20" as a PATH: every open missed,
 * fell through to the OBB fallback, and re-opened main.obb until fopen returned
 * NULL -- the log107/log108 crash. With the callbacks honoured we never touch the
 * filesystem for a stream, so that storm cannot happen at all. */
typedef int (*fs_open_cb)(const char *name, unsigned *filesize, void **handle, void *ud);
typedef int (*fs_close_cb)(void *handle, void *ud);
typedef int (*fs_read_cb)(void *handle, void *buf, unsigned bytes, unsigned *got, void *ud);
typedef int (*fs_seek_cb)(void *handle, unsigned pos, void *ud);

static fs_open_cb  g_fs_open  = NULL;
static fs_close_cb g_fs_close = NULL;
static fs_read_cb  g_fs_read  = NULL;
static fs_seek_cb  g_fs_seek  = NULL;

static int Sys_setFileSystem(void *self, fs_open_cb o, fs_close_cb c, fs_read_cb r,
                             fs_seek_cb s, void *aread, void *acancel, int blockalign) {
  (void)self; (void)aread; (void)acancel;
  g_fs_open = o; g_fs_close = c; g_fs_read = r; g_fs_seek = s;
  log_printf("[snd] setFileSystem open=%p close=%p read=%p seek=%p blockalign=%d",
             (void *)o, (void *)c, (void *)r, (void *)s, blockalign);
  return FMOD_OK;
}

static unsigned g_created = 0, g_played = 0, g_missing = 0, g_overbudget = 0;

/* When InitializeSource fails the game retries the SAME track many times a
 * second. Each retry used to re-read a 1.3 MB OBB member; eventually
 * fopen("main.obb") returned NULL and miniz -- which does not check -- faulted in
 * fseek(NULL) (log107, DATA_ABORT at FAR=0xc). Remember what failed and fail it
 * immediately, so a bad asset costs nothing instead of taking the app down. */
#define MAX_BADNAMES 96
#define BADNAME_LEN  72
static char g_bad[MAX_BADNAMES][BADNAME_LEN];
static int  g_nbad = 0;

static int bad_seen(const char *n) {
  for (int i = 0; i < g_nbad; i++)
    if (strncmp(g_bad[i], n, BADNAME_LEN - 1) == 0) return 1;
  return 0;
}
static void bad_add(const char *n) {
  if (g_nbad >= MAX_BADNAMES || bad_seen(n)) return;
  strncpy(g_bad[g_nbad], n, BADNAME_LEN - 1);
  g_bad[g_nbad][BADNAME_LEN - 1] = '\0';
  g_nbad++;
}

/* What the companion hands us is not the MP3 we assumed -- 188-byte buffers with
 * no MPEG sync, and a 6914-byte one that syncs but decodes to 0 samples. Dump the
 * head so the container is identifiable from a single log rather than another
 * guess. */
static void dump_head(const char *what, const void *p, unsigned len) {
  const unsigned char *b = (const unsigned char *)p;
  unsigned n = len < 32 ? len : 32;
  char hex[3 * 32 + 1], asc[33];
  for (unsigned i = 0; i < n; i++) {
    static const char d[] = "0123456789abcdef";
    hex[i * 3] = d[b[i] >> 4]; hex[i * 3 + 1] = d[b[i] & 15]; hex[i * 3 + 2] = ' ';
    asc[i] = (b[i] >= 0x20 && b[i] < 0x7f) ? (char)b[i] : '.';
  }
  hex[n * 3] = '\0'; asc[n] = '\0';
  log_printf("[snd] head %.48s len=%u: %s |%s|", what, len, hex, asc);
}

static int Sys_createSound(void *self, const char *name, unsigned mode,
                           void *exinfo, void **out) {
  (void)self;
  if (out) *out = NULL;
  if (!name || !out) return FMOD_ERR_INVALID_PARAM;

  /* FMOD_CREATESOUNDEXINFO: cbsize@0, length@4, fileoffset@8 (cbsize is 148 in
   * this build). The companion always supplies one; treat a missing one as
   * "whole file" rather than trusting a NULL deref. */
  const unsigned *ex = (const unsigned *)exinfo;
  unsigned ex_len = ex ? ex[1] : 0;
  unsigned ex_off = ex ? ex[2] : 0;

  const void *buf = NULL;
  unsigned    len = 0;
  void       *owned = NULL;               /* freed before we return */

  if (mode & FMOD_OPENMEMORY) {
    /* SFX: `name` is the buffer, not a path. */
    buf = (const void *)name;
    len = ex_len;
    if (!len) return FMOD_ERR_INVALID_PARAM;
  } else if (g_fs_open && g_fs_read) {
    /* Music/VO: pull it through the companion's own callbacks. NEVER fall back
     * to the filesystem here -- `name` is an ID, not a path. */
    unsigned fsz = 0;
    void *h = NULL;
    int rc = g_fs_open(name, &fsz, &h, NULL);
    if (rc != FMOD_OK || !h) {
      if (g_missing < 32)
        log_printf("[snd] stream open FAILED id=%.32s rc=%d fsz=%u", name, rc, fsz);
      g_missing++;
      return FMOD_ERR_FILE_NOTFOUND;
    }
    unsigned want = ex_len ? ex_len : (fsz > ex_off ? fsz - ex_off : 0);
    if (!want || want > PCM_BUDGET_BYTES) {
      if (g_fs_close) g_fs_close(h, NULL);
      return FMOD_ERR_INVALID_PARAM;
    }
    owned = big_malloc(want);
    if (!owned) { if (g_fs_close) g_fs_close(h, NULL); return FMOD_ERR_MEMORY; }
    if (ex_off && g_fs_seek) g_fs_seek(h, ex_off, NULL);
    unsigned got = 0;
    g_fs_read(h, owned, want, &got, NULL);      /* EOF is fine if got > 0 */
    if (g_fs_close) g_fs_close(h, NULL);
    /* NEVER trust the callback's byte count: once the companion's OBB handle
     * went bad it reported 966980227 for a 1.3 MB buffer, and we then scanned
     * far past the allocation (log109). */
    if (got > want) {
      log_printf("[snd] stream read OVERRUN id=%.32s said %u for a %u buffer -- clamped",
                 name, got, want);
      got = want;
    }
    if (!got) {
      if (g_missing < 32)
        log_printf("[snd] stream read EMPTY id=%.32s want=%u fsz=%u", name, want, fsz);
      g_missing++;
      big_free(owned);
      return FMOD_ERR_FILE_NOTFOUND;
    }
    buf = owned;
    len = got;
  } else {
    /* No callbacks installed: `name` really is a path. */
    if (bad_seen(name)) return FMOD_ERR_FILE_NOTFOUND;   /* no I/O on a retry */
    unsigned flen = 0;
    owned = sdl_load_file(name, &flen);
    if (!owned || !flen) {
      if (g_missing < 32) log_printf("[snd] createSound MISS: %.96s", name);
      g_missing++;
      bad_add(name);
      big_free(owned);
      return FMOD_ERR_FILE_NOTFOUND;
    }
    if (ex_off >= flen) { bad_add(name); big_free(owned); return FMOD_ERR_INVALID_PARAM; }
    unsigned avail = flen - ex_off;
    len = (ex_len && ex_len <= avail) ? ex_len : avail;
    buf = (const char *)owned + ex_off;
  }

  const char *what = (mode & FMOD_OPENMEMORY) ? "<memory>" : name;
  AudioPcm pcm;
  int ok = 0, silent = 0;

  /* Already decoded this exact asset? Share it and skip all the work. */
  uint32_t hkey = key_hash(buf, len);
  lock();
  PcmEntry *ent = cache_find(len, hkey);
  if (ent) { ent->refs++; ent->stamp = ++g_clock; }
  unlock();
  if (ent) { g_cache_hits++; big_free(owned); goto have_pcm; }
  g_cache_miss++;

  /* A whole music track is ~15 MB of PCM. Two of them filled the heap, after
   * which every later decode failed silently, InitializeSource kept failing, and
   * the game retried forever -- leaking a companion OBB handle per retry until
   * the process fell over (log109). So decide affordability BEFORE decoding, and
   * when we cannot afford it hand back a correctly-timed SILENT sound: costs no
   * memory, keeps the game's pacing, and stops the retry loop dead.
   * Proper fix is incremental streaming; this makes it survivable meanwhile. */
  if (mode & FMOD_CREATESTREAM) {
    AudioPcm est;
    if (audio_mp3_probe(buf, len, &est)) {
      unsigned need = est.nsamples * est.channels * 2u;
      if (need > STREAM_PCM_MAX || g_pcm_bytes + need > PCM_BUDGET_BYTES) {
#if AUDIO_STREAM_LONG_ASSETS
        /* Too big to hold decoded, so stream it: the compressed bytes plus a
         * ring instead of the whole waveform, and no decode stall on start.
         * Requires `owned` -- the decoder reads the elementary stream in place
         * for the life of the handle, so we must be the ones who allocated it.
         * FMOD_OPENMEMORY sounds are short effects already in memory and never
         * reach this branch. */
        if (owned) {
          AudioPcm fmt;
          Stream *st = stream_open(owned, buf, len, &fmt,
                                   (mode & FMOD_LOOP_NORMAL) != 0);
          if (st) {
            const char *base = what;
            for (const char *q = what; *q; q++)
              if (*q == '\\' || *q == '/') base = q + 1;
            unsigned bn = 0;
            while (base[bn] && bn < sizeof st->name - 1) { st->name[bn] = base[bn]; bn++; }
            st->name[bn] = '\0';
            lock();
            Snd *ss = snd_alloc();
            unlock();
            if (!ss) {
              stream_close(st);            /* takes `owned` with it */
              return FMOD_ERR_INVALID_PARAM;
            }
            ss->is3d = (mode & FMOD_3D) ? 1 : 0;
            ss->ent  = NULL;
            ss->st   = st;
            ss->pcm  = fmt;                /* fmt.pcm is NULL */
            *out = ss;
            log_printf("[snd] createSound STREAMING \"%.64s\" -> %u ms %uHz %uch "
                       "(es %u KB, ring %u KB; whole decode would have been %u KB)",
                       what, fmt.ms, fmt.rate, fmt.channels, len / 1024,
                       (unsigned)((RING_FRAMES * fmt.channels * 2u) / 1024u),
                       need / 1024);
            g_created++;
            return FMOD_OK;                /* `owned` now belongs to st */
          }
        }
#endif
        /* Could not stream it -- fall back to timed silence, which at least
         * keeps the game's pacing and stops the retry loop. */
        pcm = est;                                  /* est.pcm is already NULL */
        ok = silent = 1;
        if (g_overbudget < 12)
          log_printf("[snd] stream too large and NOT streamable: id=%.32s needs %u KB "
                     "(cap %u KB, in use %u KB, owned=%d) -- playing %u ms of SILENCE",
                     name, need / 1024, STREAM_PCM_MAX / 1024,
                     g_pcm_bytes / 1024, owned ? 1 : 0, est.ms);
        g_overbudget++;
      }
    }
  }

  if (!ok) ok = audio_mp3_decode(buf, len, &pcm);

  /* Decode failed outright. For a stream, still prefer timed silence over an
   * error for exactly the same reason as above. */
  if (!ok && (mode & FMOD_CREATESTREAM) && audio_mp3_probe(buf, len, &pcm)) {
    ok = silent = 1;
    if (g_missing < 32)
      log_printf("[snd] decode failed, substituting %u ms silence: id=%.32s", pcm.ms, name);
    g_missing++;
  }

  if (!ok) {
    if (g_missing < 32) {
      log_printf("[snd] decode FAILED (mode=0x%x len=%u) %.72s", mode, len, what);
      dump_head(what, buf, len);
    }
    g_missing++;
    if (!(mode & (FMOD_OPENMEMORY | FMOD_CREATESTREAM))) bad_add(name);
    big_free(owned);
    return FMOD_ERR_FILE_NOTFOUND;
  }
  big_free(owned);

  lock();
  ent = cache_insert(len, hkey, &pcm);
  unlock();
  if (!ent) {
    audio_pcm_free(&pcm);
    if (g_overbudget < 8)
      log_printf("[snd] cache full -- dropping %.64s (%u KB, in use %u KB)",
                 what, pcm_bytes_of(&pcm) / 1024, g_pcm_bytes / 1024);
    g_overbudget++;
    return FMOD_ERR_MEMORY;
  }

have_pcm:;
  /* Steady-state visibility. The first-24 caps hide exactly the phase that
   * matters (combat), and without a hit/miss ratio there is no way to tell a
   * working cache from a broken key. */
  {
    static unsigned n = 0;
    /* g_played is the discriminator log4 lacked. Its END rate fell from ~127 per
     * 128 createSound to 0 while createSound itself kept climbing, and both
     * steal counters froze -- which is either "the game stopped calling
     * playSound" or "sounds are played but never finish", and nothing logged
     * could tell the two apart. A live channel census settles it alongside. */
    int nused = 0, nplaying = 0, npend = 0;
    lock();
    for (int i = 0; i < MAX_CHANNELS; i++) {
      if (!g_chan[i].used) continue;
      nused++;
      if (g_chan[i].playing)     nplaying++;
      if (g_chan[i].end_pending) npend++;
    }
    unlock();
    if ((++n & 127) == 0)
      log_printf("[snd] stats: %u createSound, cache %u hit / %u miss, "
                 "%u KB PCM held, %u decode fail, voices reused %u/%u (done/live), "
                 "%u END sent over %u updates, %u playSound, "
                 "%u END on steal / %u lost, "
                 "%u release killed %u live / %u pending, "
                 "3D %u listener / %u pos / %u minmax, "
                 "basis %s (%u degenerate), "
                 "gain n=%u min=%.3f max=%.3f loud=%u quiet=%u maxdist=%.0f "
                 "(worst %.3f at d=%.1f, min/max %.1f/%.1f), "
                 "playSound %u calls / %u badsnd / %u nochan, "
                 "limiter gain %.3f (min %.3f), %u of %u grains over (peak %d = %.1fx), "
                 "vol2d n=%u min=%.3f max=%.3f avg=%.3f, "
                 "vol3d n=%u min=%.3f max=%.3f avg=%.3f, "
                 "bus2d avg=%.3f over %u, bus3d avg=%.3f over %u, "
                 "streams %u opened / %d live / %u underruns, "
                 "chans %d used / %d playing / %d endPending of %d",
                 n, g_cache_hits, g_cache_miss, g_pcm_bytes / 1024, g_missing,
                 g_steals, g_steals_live, g_ends_fired, g_updates, g_played,
                 g_ends_rescued, g_ends_lost,
                 g_rel_calls, g_rel_kill_live, g_rel_kill_pend,
                 g_lis_sets, g_pos_sets, g_mm_sets,
                 g_lis_basis_ok ? "OK" : "NONE", g_lis_degenerate,
                 g_g3_n, g_g3_min, g_g3_max, g_g3_loud, g_g3_quiet, g_dist_max,
                 g_g3_worst, g_g3_worst_d, g_g3_worst_mn, g_g3_worst_mx,
                 g_play_calls, g_play_badsnd, g_play_nochan,
                 g_lim_gain, g_lim_min, g_lim_clipped, g_lim_grains,
                 (int)g_lim_peak, (double)g_lim_peak / 32767.0,
                 g_vol_n2d, g_vol_min2d, g_vol_max2d,
                 g_vol_n2d ? g_vol_sum2d / (float)g_vol_n2d : 0.0f,
                 g_vol_n3d, g_vol_min3d, g_vol_max3d,
                 g_vol_n3d ? g_vol_sum3d / (float)g_vol_n3d : 0.0f,
                 g_mix_n2d ? g_mix_sum2d / (float)g_mix_n2d : 0.0f, g_mix_n2d,
                 g_mix_n3d ? g_mix_sum3d / (float)g_mix_n3d : 0.0f, g_mix_n3d,
                 g_streams_open, g_stream_decoders, g_stream_underruns,
                 nused, nplaying, npend, g_nchannels);
  }

  lock();
  Snd *s = snd_alloc();
  unlock();
  if (!s) { lock(); cache_release(ent); unlock(); return FMOD_ERR_INVALID_PARAM; }
  s->is3d = (mode & FMOD_3D) ? 1 : 0;
  s->ent = ent;
  s->pcm = ent->pcm;                 /* borrowed: the cache owns the samples */
  *out = s;

  /* Streams are rare (~40 per 13 min) and are the one path we still cannot
   * see the outcome of, so never throttle them. SFX stay capped. */
  if (g_created < 24 || (mode & FMOD_CREATESTREAM))
    log_printf("[snd] createSound %s%s\"%.64s\" -> %u ms %uHz %uch (mode=0x%x, %u KB) "
               "[cache %u hit / %u miss]",
               (mode & FMOD_CREATESTREAM) ? "stream " : "", silent ? "SILENT " : "",
               what, s->pcm.ms, s->pcm.rate, s->pcm.channels, mode,
               pcm_bytes_of(&s->pcm) / 1024, g_cache_hits, g_cache_miss);
  g_created++;
  return FMOD_OK;
}

/* g_played counts SUCCESSES, which cannot distinguish "the game stopped asking"
 * from "the game asked and we turned it down" -- and log154 and log159 both show
 * playSound flatlining exactly while the pool sits at 45 of 45. Count the calls
 * themselves, and both ways one can fail. */

static int Sys_playSound(void *self, void *sound, void *group, int paused, void **outch) {
  (void)self; (void)group;
  g_play_calls++;
  if (outch) *outch = NULL;
  if (!snd_valid(sound)) { g_play_badsnd++; return FMOD_ERR_INVALID_PARAM; }
  Snd *s = (Snd *)sound;

  audio_start();
  lock();
  Chan *c = chan_alloc();
  if (c) {
    c->snd     = s;
    c->pos     = 0.0;
    c->step    = (double)s->pcm.rate / (double)OUT_RATE;
    c->vol     = 1.0f;
    c->pan     = 0.0f;
    c->paused  = paused ? 1 : 0;
    c->playing = 1;
    c->mindist = 1.0f;                 /* FMOD defaults until the game says else */
    c->maxdist = 10000.0f;
    c->occl    = 0.0f;
    c->has_pos = 0;
    /* Claim the stream for this channel and start it from the top. pos is 0
     * above, so the window has to be 0 too -- and the decoder has to be rewound
     * with it, or a replayed track would decode from wherever the last one
     * stopped. Safe here: the feeder runs under this same lock. */
    if (s->st) {
      audio_mp3_stream_rewind(s->st->dec);
      audio_ring_reset(&s->st->ring);
      s->st->reader = c;
    }
  }
  unlock();
  if (!c) { g_play_nochan++; return FMOD_ERR_INVALID_PARAM; }
  if (outch) *outch = c;
  if (g_played < 24)
    log_printf("[snd] playSound -> chan %d (%u ms, paused=%d)",
               (int)(c - g_chan), s->pcm.ms, paused);
  g_played++;
  return FMOD_OK;
}

/* Real FMOD pre-allocates the channel pool in System::init, so getChannel(i)
 * yields a valid handle for any in-range index whether or not it is playing.
 * Requiring ->used here made InitChannels abort on its very first call and took
 * all of sound down with it (log106). Do not reintroduce that check. */
static int Sys_getChannel(void *self, int idx, void **out) {
  (void)self;
  if (!out) return FMOD_ERR_INVALID_PARAM;
  *out = NULL;
  if (idx < 0 || idx >= g_nchannels) return FMOD_ERR_INVALID_PARAM;
  *out = (void *)&g_chan[idx];
  return FMOD_OK;
}

/* ---- FMOD::Sound ---------------------------------------------------------- */
static int Snd_release(void *self) {
  if (!snd_valid(self)) return FMOD_ERR_INVALID_PARAM;
  Snd *s = (Snd *)self;
  lock();
  g_rel_calls++;
  for (int i = 0; i < g_nchannels; i++)
    if (g_chan[i].used && g_chan[i].snd == s) {
      if (g_chan[i].playing)     g_rel_kill_live++;   /* died with no END at all */
      if (g_chan[i].end_pending) g_rel_kill_pend++;   /* END built, never sent */
      g_chan[i].playing = 0; g_chan[i].used = 0;
      g_chan[i].end_pending = 0; g_chan[i].cb = NULL;   /* the source is gone */
    }
  cache_release(s->ent);          /* samples stay cached for the next request */
  s->ent = NULL;
  /* Detach the stream under the same lock the feeder runs under, so it cannot
   * be mid-decode on a Stream we are about to free. Streams are never cached:
   * each owns its decoder, ring and compressed bytes outright. */
  Stream *st = s->st;
  s->st = NULL;
  memset(&s->pcm, 0, sizeof s->pcm);
  s->used = 0;
  unlock();
  stream_close(st);               /* outside the lock: frees memory, hits hardware */
  return FMOD_OK;
}

static int Snd_getLength(void *self, unsigned *len, unsigned unit) {
  if (len) *len = 0;
  if (!snd_valid(self) || !len) return FMOD_ERR_INVALID_PARAM;
  Snd *s = (Snd *)self;
  *len = (unit & FMOD_TIMEUNIT_PCM) ? s->pcm.nsamples : s->pcm.ms;
  return FMOD_OK;
}

/* ---- FMOD::Channel / ChannelControl --------------------------------------- */
/* FMOD's `bool` is one byte; take char* so we write exactly one. */
static int Ch_isPlaying(void *self, char *b) {
  if (b) *b = 0;                                  /* ALWAYS write the out-param */
  if (!chan_valid(self) || !b) return FMOD_ERR_INVALID_PARAM;
  Chan *c = (Chan *)self;
  *b = (char)(c->used && c->playing);
  return FMOD_OK;
}
/* An explicit stop must NOT raise END. FModAudioSystem::StopChannel (+0x73cd1)
 * already calls ChannelInfo::Reset itself right after stop(), and a late END on
 * a looping voice would send HandleChannelEnd down its restart branch and bring
 * back the sound the game just silenced. */
static int Ch_stop(void *self) {
  if (!chan_valid(self)) return FMOD_ERR_INVALID_PARAM;
  lock();
  Chan *c = (Chan *)self;
  c->playing = 0;
  c->used    = 0;
  c->end_pending = 0;
  c->cb      = NULL;
  unlock();
  return FMOD_OK;
}
static int Ch_setPaused(void *self, int paused) {
  if (!chan_valid(self)) return FMOD_ERR_INVALID_PARAM;
  ((Chan *)self)->paused = paused ? 1 : 0;
  return FMOD_OK;
}
static int Ch_setVolume(void *self, uint32_t v) {          /* softfp float */
  if (!chan_valid(self)) return FMOD_ERR_INVALID_PARAM;
  Chan *c = (Chan *)self;
  float f = u2f(v);
  if (f < 0.0f) f = 0.0f; else if (f > 1.0f) f = 1.0f;
  c->vol = f;
  if (c->snd && c->snd->is3d) {
    g_vol_n3d++;
    g_vol_sum3d += f;
    if (f < g_vol_min3d) g_vol_min3d = f;
    if (f > g_vol_max3d) g_vol_max3d = f;
  } else {
    g_vol_n2d++;
    g_vol_sum2d += f;
    if (f < g_vol_min2d) g_vol_min2d = f;
    if (f > g_vol_max2d) g_vol_max2d = f;
  }
  return FMOD_OK;
}
static int Ch_setPan(void *self, uint32_t v) {             /* softfp float */
  if (!chan_valid(self)) return FMOD_ERR_INVALID_PARAM;
  float f = u2f(v);
  if (f < -1.0f) f = -1.0f; else if (f > 1.0f) f = 1.0f;
  ((Chan *)self)->pan = f;
  return FMOD_OK;
}
static int Ch_setFrequency(void *self, uint32_t v) {       /* softfp float */
  if (!chan_valid(self)) return FMOD_ERR_INVALID_PARAM;
  Chan *c = (Chan *)self;
  float hz = u2f(v);
  if (hz > 1.0f) c->step = (double)hz / (double)OUT_RATE;
  return FMOD_OK;
}
static int Ch_getPosition(void *self, unsigned *pos, unsigned unit) {
  if (pos) *pos = 0;
  if (!chan_valid(self) || !pos) return FMOD_ERR_INVALID_PARAM;
  Chan *c = (Chan *)self;
  unsigned frames = (unsigned)c->pos;
  unsigned rate = (c->snd && c->snd->pcm.rate) ? c->snd->pcm.rate : OUT_RATE;
  *pos = (unit & FMOD_TIMEUNIT_PCM) ? frames
                                    : (unsigned)((uint64_t)frames * 1000u / rate);
  return FMOD_OK;
}
static int Ch_setPosition(void *self, unsigned pos, unsigned unit) {
  if (!chan_valid(self)) return FMOD_ERR_INVALID_PARAM;
  Chan *c = (Chan *)self;
  if (!c->snd) return FMOD_ERR_INVALID_PARAM;
  unsigned rate = c->snd->pcm.rate ? c->snd->pcm.rate : OUT_RATE;
  lock();
  c->pos = (unit & FMOD_TIMEUNIT_PCM) ? (double)pos : (double)pos * rate / 1000.0;
  unlock();
  return FMOD_OK;
}
static int Ch_setCallback(void *self, chan_cb cb) {
  if (!chan_valid(self)) return FMOD_ERR_INVALID_PARAM;
  lock();
  ((Chan *)self)->cb = cb;
  unlock();
  return FMOD_OK;
}
static int Ch_setUserData(void *self, void *ud) {
  if (!chan_valid(self)) return FMOD_ERR_INVALID_PARAM;
  ((Chan *)self)->userdata = ud;
  return FMOD_OK;
}
static int Ch_getUserData(void *self, void **ud) {
  if (ud) *ud = NULL;
  if (!chan_valid(self) || !ud) return FMOD_ERR_INVALID_PARAM;
  *ud = ((Chan *)self)->userdata;
  return FMOD_OK;
}

/* FMOD_VECTOR is three floats and arrives BY POINTER, so unlike setVolume these
 * need no softfp shim -- only the scalar float entry points below do. */
typedef struct { float x, y, z; } FmodVec;

static int Ch_set3DAttributes(void *self, const FmodVec *pos, const FmodVec *vel) {
  (void)vel;                                  /* no doppler: nothing reads velocity */
  if (!chan_valid(self)) return FMOD_ERR_INVALID_PARAM;
  if (!pos) return FMOD_OK;
  Chan *c = (Chan *)self;
  lock();
  c->px = pos->x; c->py = pos->y; c->pz = pos->z;
  c->has_pos = 1;
  unlock();
  g_pos_sets++;
  return FMOD_OK;
}

/* Both parameters are floats in core registers (softfp caller). */
static int Ch_set3DMinMaxDistance(void *self, uint32_t mn, uint32_t mx) {
  if (!chan_valid(self)) return FMOD_ERR_INVALID_PARAM;
  Chan *c = (Chan *)self;
  float fmn = u2f(mn), fmx = u2f(mx);
  lock();
  if (fmn > 0.0f)   c->mindist = fmn;
  if (fmx > fmn)    c->maxdist = fmx;
  unlock();
  g_mm_sets++;
  return FMOD_OK;
}

static int Ch_set3DOcclusion(void *self, uint32_t direct, uint32_t reverb) {
  (void)reverb;                               /* no reverb bus to occlude */
  if (!chan_valid(self)) return FMOD_ERR_INVALID_PARAM;
  float d = u2f(direct);
  ((Chan *)self)->occl = (d < 0.0f) ? 0.0f : (d > 1.0f ? 1.0f : d);
  return FMOD_OK;
}

/* The listener's basis. right = forward x up, which is what a pan projects
 * onto; KOTOR hands us an already-orthonormal pair, but normalise anyway so a
 * denormal frame cannot turn into a silent divide. */
static int Sys_set3DListenerAttributes(void *self, int listener, const FmodVec *pos,
                                       const FmodVec *vel, const FmodVec *fwd,
                                       const FmodVec *up) {
  (void)self; (void)vel;
  if (listener != 0) return FMOD_OK;          /* single-listener game */
  /* Snapshot before doing anything else. log160 printed fwd=(0,0,0) up=(0,0,0)
   * next to "0 degenerate" and a perfectly good right vector, which is not a
   * contradiction: the print dereferenced the game's own vectors after the lock
   * was dropped, by which time it had reused them. The basis was always fine;
   * only the report was lying. */
  FmodVec f = {0,0,0}, u = {0,0,0};
  if (fwd) f = *fwd;
  if (up)  u = *up;
  lock();
  if (pos) { g_lis_px = pos->x; g_lis_py = pos->y; g_lis_pz = pos->z; }
  if (fwd && up) {
    /* FMOD is LEFT-handed unless the game passes FMOD_INIT_3D_RIGHTHANDED, and
     * the two orders give exactly opposite right vectors -- a silently mirrored
     * stereo image, which is the kind of thing nobody notices and everybody
     * feels. Left-handed wants up x forward; right-handed wants forward x up. */
    float rx, ry, rz;
#if AUDIO_3D_RIGHTHANDED
    rx = f.y * u.z - f.z * u.y;
    ry = f.z * u.x - f.x * u.z;
    rz = f.x * u.y - f.y * u.x;
#else
    rx = u.y * f.z - u.z * f.y;
    ry = u.z * f.x - u.x * f.z;
    rz = u.x * f.y - u.y * f.x;
#endif
    float len = sqrtf(rx * rx + ry * ry + rz * rz);
    if (len > 0.0001f) {
      g_lis_rx = rx / len; g_lis_ry = ry / len; g_lis_rz = rz / len;
      g_lis_basis_ok = 1;
    } else {
      g_lis_degenerate++;          /* zero fwd/up: keep the last good basis */
    }
  }
  unlock();
  /* Periodically, not once. The single first-call print in log159 caught the
   * one update guaranteed to be uninitialised and then went blind, which said
   * nothing about whether the basis is ever valid. */
  if ((g_lis_sets % 8192) == 0)
    log_printf("[snd] listener #%u: pos=(%.1f,%.1f,%.1f) fwd=(%.2f,%.2f,%.2f) "
               "up=(%.2f,%.2f,%.2f) -> right=(%.2f,%.2f,%.2f) basis=%s "
               "(%u degenerate) [%s]",
               g_lis_sets, g_lis_px, g_lis_py, g_lis_pz,
               f.x, f.y, f.z, u.x, u.y, u.z,
               g_lis_rx, g_lis_ry, g_lis_rz,
               g_lis_basis_ok ? "OK" : "NONE YET", g_lis_degenerate,
               AUDIO_3D_RIGHTHANDED ? "right-handed" : "left-handed");
  g_lis_sets++;
  return FMOD_OK;
}

/* OpenSLES interface IDs are data objects the engine dereferences by address;
 * a stable dummy address per IID is enough to satisfy relocation. */
static const uint32_t sl_iid_engine      = 0;
static const uint32_t sl_iid_play        = 0;
static const uint32_t sl_iid_volume      = 0;
static const uint32_t sl_iid_bufferqueue = 0;

static const so_default_dynlib audio_dynlib[] = {
  // ---- OpenSLES (libandroid_port.so imports these directly) ----
  { "slCreateEngine",     (uintptr_t)&fmod_stub },
  { "SL_IID_ENGINE",      (uintptr_t)&sl_iid_engine },
  { "SL_IID_PLAY",        (uintptr_t)&sl_iid_play },
  { "SL_IID_VOLUME",      (uintptr_t)&sl_iid_volume },
  { "SL_IID_BUFFERQUEUE", (uintptr_t)&sl_iid_bufferqueue },

  // ---- FMOD C API ----
  { "FMOD_System_Create",       (uintptr_t)&Sys_Create },
  { "FMOD_System_MixerSuspend", (uintptr_t)&fmod_stub },
  { "FMOD_System_MixerResume",  (uintptr_t)&fmod_stub },

  // ---- FMOD C++ API (name-mangled) ----
  { "_ZN4FMOD6System4initEijPv", (uintptr_t)&Sys_init },
  { "_ZN4FMOD6System6updateEv", (uintptr_t)&Sys_update },
  { "_ZN4FMOD6System7releaseEv", (uintptr_t)&fmod_stub },
  { "_ZN4FMOD6System9playSoundEPNS_5SoundEPNS_12ChannelGroupEbPPNS_7ChannelE", (uintptr_t)&Sys_playSound },
  { "_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE", (uintptr_t)&fmod_stub },
  { "_ZN4FMOD6System10getChannelEiPPNS_7ChannelE", (uintptr_t)&Sys_getChannel },
  { "_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE", (uintptr_t)&Sys_createSound },
  { "_ZN4FMOD6System13getNumDriversEPi", (uintptr_t)&Sys_getNumDrivers },
  { "_ZN4FMOD6System13setFileSystemEPF11FMOD_RESULTPKcPjPPvS5_EPFS1_S5_S5_EPFS1_S5_S5_jS4_S5_EPFS1_S5_jS5_EPFS1_P18FMOD_ASYNCREADINFOS5_ESI_i", (uintptr_t)&Sys_setFileSystem },
  { "_ZN4FMOD6System19setStreamBufferSizeEjj", (uintptr_t)&fmod_stub },
  { "_ZN4FMOD6System23set3DListenerAttributesEiPK11FMOD_VECTORS3_S3_S3_", (uintptr_t)&Sys_set3DListenerAttributes },
  { "_ZN4FMOD5Sound7releaseEv", (uintptr_t)&Snd_release },
  { "_ZN4FMOD5Sound9getLengthEPjj", (uintptr_t)&Snd_getLength },
  { "_ZN4FMOD5Sound11setUserDataEPv", (uintptr_t)&fmod_stub },
  { "_ZN4FMOD7Channel11getPositionEPjj", (uintptr_t)&Ch_getPosition },
  { "_ZN4FMOD7Channel11setPositionEjj", (uintptr_t)&Ch_setPosition },
  { "_ZN4FMOD7Channel11setPriorityEi", (uintptr_t)&fmod_stub },
  { "_ZN4FMOD7Channel12setFrequencyEf", (uintptr_t)&Ch_setFrequency },   // softfp
  { "_ZN4FMOD14ChannelControl4stopEv", (uintptr_t)&Ch_stop },
  { "_ZN4FMOD14ChannelControl6setPanEf", (uintptr_t)&Ch_setPan },        // softfp
  { "_ZN4FMOD14ChannelControl9isPlayingEPb", (uintptr_t)&Ch_isPlaying },
  { "_ZN4FMOD14ChannelControl9setPausedEb", (uintptr_t)&Ch_setPaused },
  { "_ZN4FMOD14ChannelControl9setVolumeEf", (uintptr_t)&Ch_setVolume },  // softfp
  { "_ZN4FMOD14ChannelControl11getUserDataEPPv", (uintptr_t)&Ch_getUserData },
  { "_ZN4FMOD14ChannelControl11setUserDataEPv", (uintptr_t)&Ch_setUserData },
  { "_ZN4FMOD14ChannelControl14set3DOcclusionEff", (uintptr_t)&Ch_set3DOcclusion },
  { "_ZN4FMOD14ChannelControl15set3DAttributesEPK11FMOD_VECTORS3_S3_", (uintptr_t)&Ch_set3DAttributes },
  { "_ZN4FMOD14ChannelControl19set3DMinMaxDistanceEff", (uintptr_t)&Ch_set3DMinMaxDistance },
  { "_ZN4FMOD14ChannelControl11setCallbackEPF11FMOD_RESULTP19FMOD_CHANNELCONTROL24FMOD_CHANNELCONTROL_TYPE33FMOD_CHANNELCONTROL_CALLBACK_TYPEPvS6_E", (uintptr_t)&Ch_setCallback },
};

const int audio_dynlib_size = sizeof(audio_dynlib);

const so_default_dynlib *audio_get_dynlib(void) {
  return audio_dynlib;
}
