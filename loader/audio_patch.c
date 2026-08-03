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

#include "audio_patch.h"
#include "audio_mp3.h"
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
#define MAX_SOUNDS   128

/* Streams decode whole: a 4-minute track at 32 kHz mono is ~15 MB of PCM. The
 * heap is 192 MB and shared with the game, so cap total decoded audio and refuse
 * politely past it rather than failing an allocation somewhere unrelated. */
#define PCM_BUDGET_BYTES (40u * 1024u * 1024u)
/* Per-stream ceiling. A whole 88 s track at 44100 stereo is ~15 MB; anything
 * above this becomes timed silence rather than a multi-second stall plus an
 * allocation the heap cannot take. Short VO/ambient (~1.3 MB) is unaffected. */
#define STREAM_PCM_MAX   (6u * 1024u * 1024u)

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
  PcmEntry *ent;                           /* owns a reference */
  AudioPcm  pcm;                           /* borrowed copy of ent->pcm */
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
  while (g_pcm_bytes + bytes > PCM_BUDGET_BYTES)
    if (!cache_evict_one()) return NULL;

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

/* ---- mixer ---------------------------------------------------------------- */
static int32_t g_acc[OUT_GRAIN * OUT_CH];
static int16_t g_out[OUT_GRAIN * OUT_CH];

static void mix_grain(void) {
  memset(g_acc, 0, sizeof g_acc);

  lock();
  for (int c = 0; c < g_nchannels; c++) {
    Chan *ch = &g_chan[c];
    if (!ch->used || !ch->playing || ch->paused || !ch->snd) continue;
    Snd *s = ch->snd;
    const int16_t *src = s->pcm.pcm;
    unsigned n = s->pcm.nsamples, sch = s->pcm.channels;
    if (!n) { chan_finish(ch); continue; }
    if (!src) {                       /* silent placeholder: keep time, emit nothing */
      ch->pos += ch->step * (double)OUT_GRAIN;
      if (ch->pos >= (double)n) chan_finish(ch);
      continue;
    }

    /* pan -1 = hard left, +1 = hard right */
    float gl = ch->vol * (ch->pan <= 0.0f ? 1.0f : 1.0f - ch->pan);
    float gr = ch->vol * (ch->pan >= 0.0f ? 1.0f : 1.0f + ch->pan);

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

  for (int i = 0; i < OUT_GRAIN * OUT_CH; i++) {
    int32_t v = g_acc[i];
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
  if (oldest_done) { g_steals++;      return chan_take(oldest_done); }
  if (oldest_live) { g_steals_live++; return chan_take(oldest_live); }
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
#define FMOD_CREATESTREAM  0x00000080
#define FMOD_OPENMEMORY    0x00000800

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
  g_nchannels = (maxch > 0 && maxch < MAX_CHANNELS) ? maxch : MAX_CHANNELS;
  if (maxch > MAX_CHANNELS)
    log_printf("[snd] WARNING: game asked for %d channels, pool is %d -- "
               "getChannel will fail past the end and abort InitChannels",
               maxch, MAX_CHANNELS);
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
  for (int i = 0; i < g_nchannels; i++) {
    Chan *c = &g_chan[i];
    lock();
    chan_cb cb = c->cb;
    int fire = c->end_pending && cb;
    if (fire) c->end_pending = 0;
    unlock();
    if (fire) {                          /* never under the lock */
      g_ends_fired++;
      cb(c, FMOD_CHANNELCONTROL_CHANNEL, FMOD_CHANNELCONTROL_CALLBACK_END, NULL, NULL);
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
    owned = malloc(want);
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
      free(owned);
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
      free(owned);
      return FMOD_ERR_FILE_NOTFOUND;
    }
    if (ex_off >= flen) { bad_add(name); free(owned); return FMOD_ERR_INVALID_PARAM; }
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
  if (ent) { g_cache_hits++; free(owned); goto have_pcm; }
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
        pcm = est;                                  /* est.pcm is already NULL */
        ok = silent = 1;
        if (g_overbudget < 12)
          log_printf("[snd] stream too large: id=%.32s would need %u KB "
                     "(cap %u KB, in use %u KB) -- playing %u ms of SILENCE",
                     name, need / 1024, STREAM_PCM_MAX / 1024,
                     g_pcm_bytes / 1024, est.ms);
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
    free(owned);
    return FMOD_ERR_FILE_NOTFOUND;
  }
  free(owned);

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
    if ((++n & 127) == 0)
      log_printf("[snd] stats: %u createSound, cache %u hit / %u miss, "
                 "%u KB PCM held, %u decode fail, voices reused %u/%u (done/live), "
                 "%u END sent over %u updates",
                 n, g_cache_hits, g_cache_miss, g_pcm_bytes / 1024, g_missing,
                 g_steals, g_steals_live, g_ends_fired, g_updates);
  }

  lock();
  Snd *s = snd_alloc();
  unlock();
  if (!s) { lock(); cache_release(ent); unlock(); return FMOD_ERR_INVALID_PARAM; }
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

static int Sys_playSound(void *self, void *sound, void *group, int paused, void **outch) {
  (void)self; (void)group;
  if (outch) *outch = NULL;
  if (!snd_valid(sound)) return FMOD_ERR_INVALID_PARAM;
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
  }
  unlock();
  if (!c) return FMOD_ERR_INVALID_PARAM;
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
  for (int i = 0; i < g_nchannels; i++)
    if (g_chan[i].used && g_chan[i].snd == s) {
      g_chan[i].playing = 0; g_chan[i].used = 0;
      g_chan[i].end_pending = 0; g_chan[i].cb = NULL;   /* the source is gone */
    }
  cache_release(s->ent);          /* samples stay cached for the next request */
  s->ent = NULL;
  memset(&s->pcm, 0, sizeof s->pcm);
  s->used = 0;
  unlock();
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
  float f = u2f(v);
  if (f < 0.0f) f = 0.0f; else if (f > 1.0f) f = 1.0f;
  ((Chan *)self)->vol = f;
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
  { "_ZN4FMOD6System23set3DListenerAttributesEiPK11FMOD_VECTORS3_S3_S3_", (uintptr_t)&fmod_stub },
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
  { "_ZN4FMOD14ChannelControl14set3DOcclusionEff", (uintptr_t)&fmod_stub },
  { "_ZN4FMOD14ChannelControl15set3DAttributesEPK11FMOD_VECTORS3_S3_", (uintptr_t)&fmod_stub },
  { "_ZN4FMOD14ChannelControl19set3DMinMaxDistanceEff", (uintptr_t)&fmod_stub },
  { "_ZN4FMOD14ChannelControl11setCallbackEPF11FMOD_RESULTP19FMOD_CHANNELCONTROL24FMOD_CHANNELCONTROL_TYPE33FMOD_CHANNELCONTROL_CALLBACK_TYPEPvS6_E", (uintptr_t)&Ch_setCallback },
};

const int audio_dynlib_size = sizeof(audio_dynlib);

const so_default_dynlib *audio_get_dynlib(void) {
  return audio_dynlib;
}
