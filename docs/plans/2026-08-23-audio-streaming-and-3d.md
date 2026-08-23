# Streaming Music Implementation Plan

Work through the tasks in order. Each ends with a build that passes and a commit, so a task can be reviewed or reverted on its own. Checkboxes track progress.

**Goal:** Play KOTOR's music and long ambience by decoding it incrementally, instead of replacing every long asset with timed silence.

**Architecture:** A long asset keeps its compressed MP3 bytes (~1.4 MB) plus a small PCM ring (128 KB), refilled from the audio thread as the mixer drains it, rather than being decoded whole to 14–24 MB. The ring is a standalone, host-testable unit with an injected fill callback; `audio_patch.c` binds one to a `sceAudiodec` handle held open for the life of the track. Short VO and effects keep the existing decode-whole-and-cache path untouched.

**Tech Stack:** C99, VitaSDK, `sceAudiodec` hardware MP3 decoder, CMake. Host tests are plain C compiled with system gcc.

**Spec:** `docs/specs/2026-08-23-audio-streaming-and-3d.md`

**Branch:** `audio-streaming` (already created; the spec commit `7ba4851` is on it)

## Global Constraints

- **Commit messages carry no trailers.** No `Co-Authored-By`, no URLs, no generated-by footer. Source comments cross-reference in plain prose, never in `[[bracketed]]` shorthand. This repo is public and this overrides any default footer your tooling wants to add.
- **Build command:** `touch loader/log.c && cmake --build build -j"$(nproc)"`. The `touch` is required or the `BUILD` timestamp baked into the log goes stale and you will debug a binary you did not build. Output is `build/KOTOR.vpk`.
- **Zero warnings.** The loader builds clean today; keep it that way.
- **`AUDIO_MP3_STREAM_MAX` must stay strictly below `AUDIO_MP3_DECODER_POOL`.** The `sceAudiodec` pool is fixed at first `InitLibrary` and cannot be resized later. Spare slots are what keep VO decoding while music streams.
- **Free with `big_free`, never `free`, for anything from `big_malloc`.** Only the game's imported `free` does the bigalloc address-range check; the loader's own calls do not.
- **`LOG_DIAGNOSTICS` stays `1`.** The log is the user support path.
- Verify probes actually made it into the binary with `strings build/KOTOR | grep`. A clean build is not evidence your log line exists.

---

### Task 1: Host-testable PCM ring

The ring is the one piece of this work that is pure arithmetic, and ring wraparound bugs produce intermittent audio glitches that a hardware log cannot localise. So it goes in its own file with no VitaSDK dependency and gets real tests that run on your machine in under a second.

This is a deliberate, small deviation from the spec, which described the ring as living inside `audio_patch.c`. Reasons: `audio_patch.c` is already 1294 lines, and a callback-per-refill (not per-sample) costs nothing measurable.

**Files:**
- Create: `loader/audio_ring.h`
- Create: `loader/audio_ring.c`
- Create: `tools/test_audio_ring.c`
- Modify: `CMakeLists.txt` (add `loader/audio_ring.c` to the `add_executable(KOTOR ...)` source list, around line 30)

**Interfaces:**
- Consumes: nothing.
- Produces: `AudioRing`, `AudioRingFill`, `audio_ring_init`, `audio_ring_retire`, `audio_ring_feed`, `audio_ring_frame`. Task 3 binds these to the MP3 decoder; Task 4 reads frames from the mixer.

- [ ] **Step 1: Write the header**

Create `loader/audio_ring.h`:

```c
/* audio_ring.h -- a ring of decoded PCM frames, filled on demand.
 *
 * For assets too large to hold decoded: music is 14-24 MB of PCM against a heap
 * shared with the game, so the whole waveform cannot be kept. The ring holds a
 * window of it instead, refilled from the audio thread as the mixer drains it.
 *
 * Frames are addressed by ABSOLUTE index -- frame 0 is the first frame of the
 * asset, and stays frame 0 for the life of the stream even after it has been
 * retired out of the window. That is what lets a mixer channel keep a single
 * monotonic play position with no rebasing.
 *
 * Deliberately free of any VitaSDK dependency: the decoder arrives as a
 * callback, so this file compiles and is tested on the host. See
 * tools/test_audio_ring.c.
 */
#ifndef AUDIO_RING_H
#define AUDIO_RING_H

#include <stdint.h>

/* Decode up to `frames` frames into `dst` (interleaved, `ch` int16 per frame).
 * Returns frames actually produced; 0 means nothing is available right now.
 * Set *eos_out to 1 when the source is exhausted and will never produce more. */
typedef unsigned (*AudioRingFill)(void *ctx, int16_t *dst, unsigned frames,
                                  int *eos_out);

typedef struct {
  int16_t *buf;      /* cap * ch int16 units; owned by the caller, not by us */
  unsigned cap;      /* capacity in frames */
  unsigned ch;       /* interleaved channels per frame */
  uint64_t base;     /* absolute index of the oldest frame still in the window */
  unsigned fill;     /* how many valid frames follow base */
  int      eos;      /* the fill callback has reported exhaustion */
} AudioRing;

/* `buf` must hold cap_frames * ch int16 units and outlive the ring. */
void audio_ring_init(AudioRing *ring, int16_t *buf, unsigned cap_frames,
                     unsigned ch);

/* Drop every frame before absolute index `consumed`. Safe to call with a value
 * behind base (no-op) or beyond the window (drops everything). */
void audio_ring_retire(AudioRing *ring, uint64_t consumed);

/* Top the window up, decoding at most `budget` frames this call. Returns frames
 * added. Bounding the budget is what stops one refill from overrunning the
 * audio thread's output deadline. */
unsigned audio_ring_feed(AudioRing *ring, AudioRingFill fn, void *ctx,
                         unsigned budget);

/* Fetch absolute frame `i` as two floats. Returns 0 if `i` is outside the
 * decoded window -- either retired already, or not decoded yet (an underrun).
 * Mono sources are duplicated to both outputs. */
int audio_ring_frame(const AudioRing *ring, uint64_t i, float *l, float *r);

#endif
```

- [ ] **Step 2: Write the failing tests**

Create `tools/test_audio_ring.c`. This is a host test, not part of the VPK build:

```c
/* test_audio_ring.c -- host tests for the PCM ring. Not built into the VPK.
 *
 * Build and run:
 *   gcc -std=c99 -Wall -Wextra -Werror -I loader \
 *       -o /tmp/test_audio_ring tools/test_audio_ring.c loader/audio_ring.c \
 *   && /tmp/test_audio_ring
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "audio_ring.h"

/* A synthetic source: frame N holds the sample value N, so any frame read back
 * proves both that the data survived the wrap and that it landed at the right
 * absolute index. */
typedef struct {
  uint64_t next;      /* absolute index of the next frame to produce */
  uint64_t total;     /* frames before the source is exhausted */
  unsigned per_call;  /* cap on frames produced per call, 0 = no cap */
  unsigned ch;
  unsigned calls;
} FakeSrc;

static unsigned fake_fill(void *ctx, int16_t *dst, unsigned frames, int *eos_out) {
  FakeSrc *s = (FakeSrc *)ctx;
  s->calls++;
  if (s->next >= s->total) { *eos_out = 1; return 0; }
  unsigned want = frames;
  if (s->per_call && want > s->per_call) want = s->per_call;
  if ((uint64_t)want > s->total - s->next) want = (unsigned)(s->total - s->next);
  for (unsigned i = 0; i < want; i++)
    for (unsigned c = 0; c < s->ch; c++)
      dst[i * s->ch + c] = (int16_t)((s->next + i) & 0x7fff);
  s->next += want;
  if (s->next >= s->total) *eos_out = 1;
  return want;
}

static void test_init_is_empty(void) {
  int16_t buf[8];
  AudioRing r;
  audio_ring_init(&r, buf, 8, 1);
  assert(r.base == 0 && r.fill == 0 && r.eos == 0);
  float l, rr;
  assert(audio_ring_frame(&r, 0, &l, &rr) == 0);
  printf("ok init_is_empty\n");
}

static void test_feed_fills_and_reads_back(void) {
  int16_t buf[8];
  AudioRing r;
  audio_ring_init(&r, buf, 8, 1);
  FakeSrc s = { 0, 1000, 0, 1, 0 };
  unsigned got = audio_ring_feed(&r, fake_fill, &s, 64);
  assert(got == 8);
  assert(r.base == 0 && r.fill == 8);
  for (unsigned i = 0; i < 8; i++) {
    float l, rr;
    assert(audio_ring_frame(&r, i, &l, &rr) == 1);
    assert(l == (float)i && rr == (float)i);
  }
  float l, rr;
  assert(audio_ring_frame(&r, 8, &l, &rr) == 0);   /* not decoded yet */
  printf("ok feed_fills_and_reads_back\n");
}

static void test_budget_bounds_one_feed(void) {
  int16_t buf[64];
  AudioRing r;
  audio_ring_init(&r, buf, 64, 1);
  FakeSrc s = { 0, 1000, 0, 1, 0 };
  unsigned got = audio_ring_feed(&r, fake_fill, &s, 10);
  assert(got == 10 && r.fill == 10);
  printf("ok budget_bounds_one_feed\n");
}

static void test_retire_advances_base(void) {
  int16_t buf[8];
  AudioRing r;
  audio_ring_init(&r, buf, 8, 1);
  FakeSrc s = { 0, 1000, 0, 1, 0 };
  audio_ring_feed(&r, fake_fill, &s, 64);
  audio_ring_retire(&r, 3);
  assert(r.base == 3 && r.fill == 5);
  float l, rr;
  assert(audio_ring_frame(&r, 2, &l, &rr) == 0);   /* retired */
  assert(audio_ring_frame(&r, 3, &l, &rr) == 1 && l == 3.0f);
  printf("ok retire_advances_base\n");
}

static void test_retire_behind_base_is_noop(void) {
  int16_t buf[8];
  AudioRing r;
  audio_ring_init(&r, buf, 8, 1);
  FakeSrc s = { 0, 1000, 0, 1, 0 };
  audio_ring_feed(&r, fake_fill, &s, 64);
  audio_ring_retire(&r, 5);
  audio_ring_retire(&r, 2);                         /* behind base */
  assert(r.base == 5 && r.fill == 3);
  printf("ok retire_behind_base_is_noop\n");
}

static void test_retire_past_window_empties(void) {
  int16_t buf[8];
  AudioRing r;
  audio_ring_init(&r, buf, 8, 1);
  FakeSrc s = { 0, 1000, 0, 1, 0 };
  audio_ring_feed(&r, fake_fill, &s, 64);
  audio_ring_retire(&r, 999);
  assert(r.fill == 0 && r.base == 8);
  printf("ok retire_past_window_empties\n");
}

/* The one that matters: data written across the wrap must read back at the
 * right absolute index. */
static void test_wraparound(void) {
  int16_t buf[8];
  AudioRing r;
  audio_ring_init(&r, buf, 8, 1);
  FakeSrc s = { 0, 1000, 0, 1, 0 };
  audio_ring_feed(&r, fake_fill, &s, 64);           /* frames 0..7 */
  audio_ring_retire(&r, 6);                         /* keep 6,7 */
  unsigned got = audio_ring_feed(&r, fake_fill, &s, 64);
  assert(got == 6);                                 /* frames 8..13 */
  assert(r.base == 6 && r.fill == 8);
  for (uint64_t i = 6; i < 14; i++) {
    float l, rr;
    assert(audio_ring_frame(&r, i, &l, &rr) == 1);
    assert(l == (float)i);
  }
  printf("ok wraparound\n");
}

static void test_stereo_interleave(void) {
  int16_t buf[8 * 2];
  AudioRing r;
  audio_ring_init(&r, buf, 8, 2);
  FakeSrc s = { 0, 1000, 0, 2, 0 };
  audio_ring_feed(&r, fake_fill, &s, 64);
  float l, rr;
  assert(audio_ring_frame(&r, 4, &l, &rr) == 1);
  assert(l == 4.0f && rr == 4.0f);
  printf("ok stereo_interleave\n");
}

static void test_eos_is_sticky_and_stops_feeding(void) {
  int16_t buf[64];
  AudioRing r;
  audio_ring_init(&r, buf, 64, 1);
  FakeSrc s = { 0, 5, 0, 1, 0 };                    /* only 5 frames exist */
  unsigned got = audio_ring_feed(&r, fake_fill, &s, 64);
  assert(got == 5 && r.eos == 1 && r.fill == 5);
  unsigned calls = s.calls;
  got = audio_ring_feed(&r, fake_fill, &s, 64);
  assert(got == 0);
  assert(s.calls == calls);                         /* did not call again */
  printf("ok eos_is_sticky_and_stops_feeding\n");
}

/* eos must NOT mean "nothing left to play": the window can still be full. This
 * is what stops the last 0.7s being cut off every track. */
static void test_eos_leaves_window_readable(void) {
  int16_t buf[64];
  AudioRing r;
  audio_ring_init(&r, buf, 64, 1);
  FakeSrc s = { 0, 5, 0, 1, 0 };
  audio_ring_feed(&r, fake_fill, &s, 64);
  assert(r.eos == 1);
  for (unsigned i = 0; i < 5; i++) {
    float l, rr;
    assert(audio_ring_frame(&r, i, &l, &rr) == 1);
  }
  printf("ok eos_leaves_window_readable\n");
}

/* A source that returns 0 without eos means "nothing right now" -- feed must
 * give up for this call rather than spin. */
static void test_short_source_does_not_spin(void) {
  int16_t buf[64];
  AudioRing r;
  audio_ring_init(&r, buf, 64, 1);
  FakeSrc s = { 0, 1000, 4, 1, 0 };                 /* 4 frames per call */
  unsigned got = audio_ring_feed(&r, fake_fill, &s, 64);
  assert(got == 64);                                /* still fills, in pieces */
  assert(s.calls == 16);                            /* 64 / 4, no spinning */
  assert(r.fill == 64);
  printf("ok short_source_does_not_spin\n");
}

int main(void) {
  test_init_is_empty();
  test_feed_fills_and_reads_back();
  test_budget_bounds_one_feed();
  test_retire_advances_base();
  test_retire_behind_base_is_noop();
  test_retire_past_window_empties();
  test_wraparound();
  test_stereo_interleave();
  test_eos_is_sticky_and_stops_feeding();
  test_eos_leaves_window_readable();
  test_short_source_does_not_spin();
  printf("\nall audio_ring tests passed\n");
  return 0;
}
```

- [ ] **Step 3: Run the tests to verify they fail**

```bash
gcc -std=c99 -Wall -Wextra -Werror -I loader \
    -o /tmp/test_audio_ring tools/test_audio_ring.c loader/audio_ring.c \
&& /tmp/test_audio_ring
```

Expected: FAIL — `loader/audio_ring.c: No such file or directory`.

- [ ] **Step 4: Write the implementation**

Create `loader/audio_ring.c`:

```c
/* audio_ring.c -- see audio_ring.h for what this is and why it has no VitaSDK
 * dependency. */
#include <stddef.h>
#include "audio_ring.h"

void audio_ring_init(AudioRing *ring, int16_t *buf, unsigned cap_frames,
                     unsigned ch) {
  ring->buf  = buf;
  ring->cap  = cap_frames;
  ring->ch   = ch;
  ring->base = 0;
  ring->fill = 0;
  ring->eos  = 0;
}

void audio_ring_retire(AudioRing *ring, uint64_t consumed) {
  if (consumed <= ring->base) return;
  uint64_t drop = consumed - ring->base;
  if (drop >= (uint64_t)ring->fill) {
    ring->base += ring->fill;
    ring->fill  = 0;
  } else {
    ring->base += drop;
    ring->fill -= (unsigned)drop;
  }
}

unsigned audio_ring_feed(AudioRing *ring, AudioRingFill fn, void *ctx,
                         unsigned budget) {
  unsigned produced = 0;
  while (ring->fill < ring->cap && budget && !ring->eos) {
    /* Frames are written strictly in order, so absolute frame f always lives at
     * f % cap. The only complication is the wrap: write at most as far as the
     * end of the buffer this time round and let the loop come back for the
     * remainder. */
    unsigned w      = (unsigned)((ring->base + ring->fill) % ring->cap);
    unsigned room   = ring->cap - ring->fill;
    unsigned contig = ring->cap - w;
    unsigned chunk  = room < contig ? room : contig;
    if (chunk > budget) chunk = budget;

    int eos = 0;
    unsigned got = fn(ctx, ring->buf + (size_t)w * ring->ch, chunk, &eos);
    if (got > chunk) got = chunk;      /* never trust a callback's own count */

    ring->fill += got;
    produced   += got;
    budget     -= got;

    if (eos) { ring->eos = 1; break; }
    if (!got) break;                   /* nothing available right now */
  }
  return produced;
}

int audio_ring_frame(const AudioRing *ring, uint64_t i, float *l, float *r) {
  if (i < ring->base || i >= ring->base + (uint64_t)ring->fill) return 0;
  const int16_t *p = ring->buf + (size_t)(i % ring->cap) * ring->ch;
  if (ring->ch == 1) { *l = (float)p[0]; *r = (float)p[0]; }
  else               { *l = (float)p[0]; *r = (float)p[1]; }
  return 1;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
gcc -std=c99 -Wall -Wextra -Werror -I loader \
    -o /tmp/test_audio_ring tools/test_audio_ring.c loader/audio_ring.c \
&& /tmp/test_audio_ring
```

Expected: every `ok ...` line, then `all audio_ring tests passed`.

- [ ] **Step 6: Add to the VPK build**

In `CMakeLists.txt`, find the `add_executable(KOTOR` list near line 30 and add `loader/audio_ring.c` alongside the other `loader/*.c` entries, keeping whatever ordering convention is already there.

- [ ] **Step 7: Verify the VPK still builds**

```bash
touch loader/log.c && cmake --build build -j"$(nproc)" 2>&1 | tail -5
```

Expected: `Built target KOTOR.vpk-vpk`, zero warnings.

- [ ] **Step 8: Commit**

```bash
git add loader/audio_ring.h loader/audio_ring.c tools/test_audio_ring.c CMakeLists.txt
git commit -m "Add a decoded-PCM ring, with tests that run off-target

Music cannot be held decoded -- 14-24 MB against a heap shared with the game --
so it has to be kept as a window refilled while it plays. The window logic is
pure arithmetic over absolute frame indices, and the failure it invites is a
wraparound bug: intermittent, position-dependent, and impossible to localise
from a hardware log.

So it lives in its own file with the decoder arriving as a callback, which
means it compiles and is tested on the host in under a second. The tests cover
the wrap, retiring, budget bounds, mono and stereo, and the case that would
otherwise cut the end off every track: end-of-stream must not imply the window
is empty."
```

---

### Task 2: Incremental MP3 decoder

Port the incremental decoder from the shelved patch, which applies cleanly to today's `audio_mp3.c`. This cannot be host-tested — it is `sceAudiodec` all the way down — so verification is a clean build plus symbol presence.

**Files:**
- Modify: `loader/audio_mp3.h` (add pool constants and the streaming API declarations)
- Modify: `loader/audio_mp3.c:39` (`totalStreams`), and append the implementation

**Interfaces:**
- Consumes: existing `audio_mp3_probe`, `find_sync`, `rate_of`, `MP3_MAX_SAMPLES`, `MP3_MAX_PCM` (all already in `audio_mp3.c`).
- Produces: `AudioMp3Stream` (opaque), `audio_mp3_stream_open`, `audio_mp3_stream_read`, `audio_mp3_stream_eos`, `audio_mp3_stream_rewind`, `audio_mp3_stream_close`, and the constants `AUDIO_MP3_DECODER_POOL` (4), `AUDIO_MP3_STREAM_MAX` (2).

- [ ] **Step 1: Add the pool constants and API to the header**

In `loader/audio_mp3.h`, after the `AudioPcm` typedef, add:

```c
/* Concurrent sceAudiodec MP3 handles the library is initialised with. This is a
 * HARD POOL -- CreateDecoder fails 0x807F0007 past it -- and it is fixed at the
 * first InitLibrary call, so it must cover every decoder alive at once:
 * AUDIO_MP3_STREAM_MAX long-lived streaming decoders, plus spares for the short
 * synchronous decodes (VO and effects) that create and delete one per call.
 *
 * This went unnoticed for months at totalStreams = 1, because the whole-asset
 * path creates a decoder, decodes, and deletes it before returning -- never two
 * alive. Streaming holds one open for the life of a track, and with a pool of 1
 * the first music track took the only slot and every later decode failed: voices
 * became substituted silence and dialogue raced, because the engine's
 * IsPlaying() then reports nothing playing.
 *
 * STREAM_MAX must stay strictly below DECODER_POOL so a spare always exists.
 * SDK ceiling is SCE_AUDIODEC_MP3_MAX_NSTREAMS (6). */
#define AUDIO_MP3_DECODER_POOL 4
#define AUDIO_MP3_STREAM_MAX   2
```

And at the end of the header, before `#endif`:

```c
/* ---- incremental decoding -------------------------------------------------
 * For assets too large to hold decoded (music is ~15 MB of PCM). The decoder
 * stays open and frames are pulled on demand, so the cost is the compressed
 * bytes plus a small ring instead of the whole waveform, and starting a track
 * costs no decode stall.
 *
 * The caller owns `data` and MUST keep it alive and unmoved until
 * audio_mp3_stream_close -- sceAudiodec reads the elementary stream in place. */
typedef struct AudioMp3Stream AudioMp3Stream;

/* Open over an in-memory MP3 asset without decoding it. `fmt` (optional)
 * receives rate/channels/nsamples/ms exactly as the whole-asset path would
 * report them, with fmt->pcm NULL -- the game reads getLength for its own
 * pacing, so a stream must not report a different length than a decoded copy of
 * the same asset. Returns NULL if the asset is not decodable. */
AudioMp3Stream *audio_mp3_stream_open(const void *data, unsigned len, AudioPcm *fmt);

/* Decode up to `frames` frames into `dst` (interleaved, fmt->channels per
 * frame). Returns frames actually produced; short or 0 at end of stream. */
unsigned audio_mp3_stream_read(AudioMp3Stream *s, int16_t *dst, unsigned frames);

/* True once the stream is exhausted and nothing is left buffered. */
int  audio_mp3_stream_eos(const AudioMp3Stream *s);

/* Restart from the first frame, for looping music. */
void audio_mp3_stream_rewind(AudioMp3Stream *s);

void audio_mp3_stream_close(AudioMp3Stream *s);
```

`<stdint.h>` is already included at the top of this header, so `int16_t` resolves; no include change needed.

- [ ] **Step 2: Size the decoder pool**

In `loader/audio_mp3.c`, replace line 39:

```c
  p.mp3.totalStreams = 1;
```

with:

```c
  p.mp3.totalStreams = AUDIO_MP3_DECODER_POOL;
```

This runs once. A second call returns `0x807F0002` ("already initialised") and the value cannot be changed afterwards, so it has to be right the first time.

- [ ] **Step 3: Append the streaming implementation**

At the end of `loader/audio_mp3.c`, after `audio_pcm_free`:

```c
/* ---- incremental decoding -------------------------------------------------
 * audio_mp3_decode above turns a whole asset into one PCM block. That is right
 * for the short clips the game fires constantly, and the cache above it makes
 * repeats free. It is wrong for music: a track is ~15 MB decoded against a heap
 * shared with the game, which is why oversized streams used to be replaced with
 * silence and the score was never audible.
 *
 * Streaming keeps the decoder open and pulls frames on demand instead. The
 * elementary stream must stay alive and unmoved for the lifetime of the handle;
 * the caller owns it. */
struct AudioMp3Stream {
  SceAudiodecCtrl ctrl;
  SceAudiodecInfo info;
  const unsigned char *d;
  unsigned len, start, pos;
  unsigned ch, rate;
  int      created, eos, errs;
  /* One decode call emits a whole granule pair, usually more than the caller
   * asked for; the remainder waits here rather than being decoded twice. */
  int16_t  carry[MP3_MAX_SAMPLES * 2];
  unsigned carry_n, carry_off;             /* in int16 units */
};

AudioMp3Stream *audio_mp3_stream_open(const void *data, unsigned len, AudioPcm *fmt) {
  if (!data || !len) return NULL;
  if (!audio_mp3_init_library()) return NULL;   /* idempotent; sizes the pool */

  /* Reuse the header walk so duration/rate/channels match what the non-stream
   * path would have reported. */
  AudioPcm probe;
  if (!audio_mp3_probe(data, len, &probe)) return NULL;

  const unsigned char *d = (const unsigned char *)data;
  int off = find_sync(d, len);
  if (off < 0) return NULL;

  unsigned ver  = (d[off + 1] >> 3) & 3;
  unsigned sri  = (d[off + 2] >> 2) & 3;
  unsigned cmod = (d[off + 3] >> 6) & 3;
  unsigned ch   = (cmod == 3) ? 1 : 2;
  unsigned rate = rate_of(ver, sri);
  if (!rate) return NULL;

  AudioMp3Stream *s = (AudioMp3Stream *)calloc(1, sizeof *s);
  if (!s) return NULL;

  s->d = d; s->len = len; s->start = (unsigned)off; s->pos = (unsigned)off;
  s->ch = ch; s->rate = rate;

  s->info.mp3.size    = sizeof(SceAudiodecInfoMp3);
  s->info.mp3.ch      = ch;
  s->info.mp3.version = ver;
  s->ctrl.size        = sizeof s->ctrl;
  s->ctrl.pInfo       = &s->info;          /* must point at storage we keep */
  s->ctrl.maxEsSize   = SCE_AUDIODEC_MP3_MAX_ES_SIZE;
  s->ctrl.maxPcmSize  = MP3_MAX_PCM;
  s->ctrl.wordLength  = SCE_AUDIODEC_WORD_LENGTH_16BITS;

  int r = sceAudiodecCreateDecoder(&s->ctrl, SCE_AUDIODEC_TYPE_MP3);
  if (r < 0) {
    log_printf("[snd] stream CreateDecoder failed 0x%08X (ch=%u ver=%u rate=%u)",
               (unsigned)r, ch, ver, rate);
    free(s);
    return NULL;
  }
  s->created = 1;

  if (fmt) {
    fmt->pcm      = NULL;                  /* streaming: no flat buffer */
    fmt->nsamples = probe.nsamples;
    fmt->channels = ch;
    fmt->rate     = rate;
    fmt->ms       = probe.ms;
  }
  return s;
}

unsigned audio_mp3_stream_read(AudioMp3Stream *s, int16_t *dst, unsigned frames) {
  if (!s || !dst || !frames || !s->created) return 0;
  unsigned want = frames * s->ch;          /* int16 units */
  unsigned got = 0;

  while (got < want) {
    if (s->carry_off < s->carry_n) {       /* drain leftovers first */
      unsigned take = s->carry_n - s->carry_off;
      if (take > want - got) take = want - got;
      memcpy(dst + got, s->carry + s->carry_off, take * sizeof(int16_t));
      s->carry_off += take;
      got += take;
      continue;
    }
    if (s->eos) break;

    s->ctrl.pEs           = (SceUInt8 *)(s->d + s->pos);
    s->ctrl.inputEsSize   = 0;
    s->ctrl.pPcm          = s->carry;
    s->ctrl.outputPcmSize = 0;
    unsigned avail = (s->pos < s->len) ? s->len - s->pos : 0;
    if (avail < 4) { s->eos = 1; break; }
    s->ctrl.maxEsSize = avail < SCE_AUDIODEC_MP3_MAX_ES_SIZE
                          ? avail : SCE_AUDIODEC_MP3_MAX_ES_SIZE;

    int r = sceAudiodecDecode(&s->ctrl);
    if (r < 0 || s->ctrl.inputEsSize == 0) {
      /* Same resync rule as the whole-asset path: a few bad frames at an edge
       * are normal, a wall of them means the stream is finished or broken. */
      if (++s->errs > 64) { s->eos = 1; break; }
      s->pos++;
      continue;
    }
    s->carry_n   = s->ctrl.outputPcmSize / sizeof(int16_t);
    s->carry_off = 0;
    s->pos      += s->ctrl.inputEsSize;
    if (!s->carry_n && s->pos >= s->len) { s->eos = 1; break; }
  }
  return got / s->ch;
}

int audio_mp3_stream_eos(const AudioMp3Stream *s) {
  return !s || (s->eos && s->carry_off >= s->carry_n);
}

void audio_mp3_stream_rewind(AudioMp3Stream *s) {
  if (!s) return;
  s->pos = s->start;
  s->carry_n = s->carry_off = 0;
  s->eos  = 0;
  s->errs = 0;
}

void audio_mp3_stream_close(AudioMp3Stream *s) {
  if (!s) return;
  if (s->created) sceAudiodecDeleteDecoder(&s->ctrl);
  free(s);
}
```

- [ ] **Step 4: Build and verify**

```bash
touch loader/log.c && cmake --build build -j"$(nproc)" 2>&1 | tail -5
```

Expected: `Built target KOTOR.vpk-vpk`, zero warnings.

- [ ] **Step 5: Verify the new probe string reached the binary**

```bash
strings build/KOTOR | grep 'stream CreateDecoder failed'
```

Expected: one match. A clean build is not evidence your log line exists — if this is empty, the code was compiled out or the file is not in the build.

- [ ] **Step 6: Commit**

```bash
git add loader/audio_mp3.c loader/audio_mp3.h
git commit -m "Decode MP3 incrementally, and size the decoder pool for it

The whole-asset decoder is right for the short clips the game fires constantly
and wrong for music, which is 14-24 MB of PCM against a heap shared with the
game. This adds a decoder that stays open and yields frames on demand, so a
track costs its compressed bytes plus a window, and starting one costs no
decode stall.

totalStreams goes from 1 to 4 in the same change, because it has to. It is a
hard pool, not a hint, and it is fixed at the first InitLibrary call. Holding
one decoder open for the life of a track is exactly what starved every later
decode when this was last attempted on hardware: voices became substituted
silence and dialogue raced, because the engine's IsPlaying() reports nothing
playing when the source never initialises. Streams are separately capped at 2
so a spare slot always remains for the short synchronous decodes."
```

---

### Task 3: Stream lifecycle in the audio backend

Bind a ring to a decoder, own the compressed bytes, and tear all three down safely. No mixer changes yet — after this task a `Stream` can be created and destroyed but nothing plays one.

**Files:**
- Modify: `loader/audio_patch.c` (constants near line 123; `Snd` struct near line 141; new stream section after `cache_release` near line 260; `Snd_release` near line 1025)
- Modify: `loader/config.h` (new flag)

**Interfaces:**
- Consumes: `AudioRing` and friends from Task 1; `AudioMp3Stream` and friends from Task 2; existing `big_malloc`/`big_free` from `bigalloc.h`; existing `lock()`/`unlock()`.
- Produces: `Stream` struct with field `AudioRing ring`; `stream_open(void *src_owned, const void *es, unsigned len, AudioPcm *fmt, int loop) -> Stream *`; `stream_close(Stream *)`; `stream_fill_cb`; counters `g_stream_decoders`, `g_streams_open`, `g_stream_underruns`; `Snd` gains field `Stream *st`. Task 4 reads `s->st`; Task 5 calls `stream_open`.

- [ ] **Step 1: Add the config flag**

In `loader/config.h`, near the other audio flags (around line 346), add:

```c
// Stream long audio assets instead of replacing them with timed silence.
//
// Music and long ambience decode to 14-24 MB of PCM, against a heap shared with
// the game -- a 15 MB decode is what crashed a session mid-area once already.
// Above STREAM_PCM_MAX an asset is now streamed: its compressed bytes stay
// resident and a small ring of decoded PCM is refilled as it plays, so a track
// costs about 1.5 MB and starts with no decode stall.
//
// Set to 0 to restore the previous behaviour exactly, where every such asset
// became a correctly-timed silent placeholder and the score was never audible.
#define AUDIO_STREAM_LONG_ASSETS 1
```

- [ ] **Step 2: Include the ring and add stream constants**

In `loader/audio_patch.c`, add to the include block near the top (after `#include "audio_mp3.h"`):

```c
#include "audio_ring.h"
```

Then, immediately after the `STREAM_PCM_MAX` definition (near line 123), add:

```c
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
} Stream;
```

- [ ] **Step 3: Add the `st` field to `Snd`**

In the `Snd` struct (near line 141), add a field. The existing struct is:

```c
typedef struct {
  int       used;
  int       is3d;                          /* created with FMOD_3D: attenuates */
  PcmEntry *ent;                           /* owns a reference */
  AudioPcm  pcm;                           /* borrowed copy of ent->pcm */
} Snd;
```

Make it:

```c
typedef struct {
  int       used;
  int       is3d;                          /* created with FMOD_3D: attenuates */
  PcmEntry *ent;                           /* owns a reference */
  AudioPcm  pcm;                           /* borrowed copy of ent->pcm */
  Stream   *st;                            /* non-NULL => streamed, pcm.pcm NULL */
} Snd;
```

- [ ] **Step 4: Add the stream section**

In `loader/audio_patch.c`, after `cache_release` (near line 260) and before the mixer section, add:

```c
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
  int live;
  lock();
  live = g_stream_decoders;
  unlock();
  if (live >= AUDIO_MP3_STREAM_MAX) {
    log_printf("[snd] stream decoder cap reached (%d of pool %d) -- not streaming this one",
               live, AUDIO_MP3_DECODER_POOL);
    return NULL;
  }

  Stream *st = (Stream *)calloc(1, sizeof *st);
  if (!st) return NULL;

  st->dec = audio_mp3_stream_open(es, len, fmt);
  if (!st->dec) { free(st); return NULL; }

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
  g_stream_decoders++;
  g_streams_open++;
  unlock();
  return st;
}

/* Caller must hold NO lock, and must already have detached every channel and
 * cleared the owning Snd's `st` under the lock -- see Snd_release. */
static void stream_close(Stream *st) {
  if (!st) return;
  if (st->dec) {
    audio_mp3_stream_close(st->dec);
    st->dec = NULL;
    lock();
    g_stream_decoders--;                   /* the handle went back to the pool */
    unlock();
  }
  big_free(st->ring_buf);
  big_free(st->src);                       /* big_malloc'd by Sys_createSound */
  free(st);
}
```

Note the two `big_free` calls. `st->src` comes from `big_malloc` in `Sys_createSound` and a 1.4 MB music asset always comes from the bigalloc pool; plain `free` on a pool pointer is wrong, because only the game's imported `free` does the address-range check.

- [ ] **Step 5: Close the stream in `Snd_release`**

The current function ends:

```c
  cache_release(s->ent);          /* samples stay cached for the next request */
  s->ent = NULL;
  memset(&s->pcm, 0, sizeof s->pcm);
  s->used = 0;
  unlock();
  return FMOD_OK;
}
```

Replace that tail with:

```c
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
```

Leave the existing `g_rel_calls` / `g_rel_kill_live` / `g_rel_kill_pend` accounting above it exactly as it is.

- [ ] **Step 6: Build and verify**

```bash
touch loader/log.c && cmake --build build -j"$(nproc)" 2>&1 | tail -5
strings build/KOTOR | grep 'stream decoder cap reached'
```

Expected: `Built target KOTOR.vpk-vpk` with zero warnings, and one match for the probe string.

- [ ] **Step 7: Commit**

```bash
git add loader/audio_patch.c loader/config.h
git commit -m "Give a streamed sound a lifetime: decoder, ring and bytes

A streamed Sound owns three things that must die together and in the right
order -- a sceAudiodec handle out of a fixed pool, the ring it fills, and the
compressed bytes the decoder reads in place. Detaching happens under the mixer
lock, which is the lock the feeder runs under, so a stream can never be freed
while it is mid-decode. Freeing happens outside it, because it touches hardware.

The compressed bytes and the ring are both released with big_free. They come
from big_malloc and at 1.4 MB always land in the pool, and only the game's
imported free does the address-range check that tells pool pointers from
newlib's -- the loader's own calls do not.

Behind AUDIO_STREAM_LONG_ASSETS so a bad hardware session is one flag from the
previous behaviour."
```

---

### Task 4: Mix from the ring

Make streamed sounds audible. This is where the end-of-stream condition matters most.

**Files:**
- Modify: `loader/audio_patch.c` — `mix_grain` (near line 390)

**Interfaces:**
- Consumes: `Stream`, `stream_fill_cb`, `FEED_MAX_FRAMES`, `g_stream_underruns` (Task 3); `audio_ring_retire`, `audio_ring_feed`, `audio_ring_frame` (Task 1); existing `chan_3d_gain`, `chan_finish`.
- Produces: nothing new; streamed channels now contribute to `g_acc`.

- [ ] **Step 1: Add the refill pass**

`mix_grain` currently opens:

```c
static void mix_grain(void) {
  memset(g_acc, 0, sizeof g_acc);

  lock();
  for (int c = 0; c < g_nchannels; c++) {
```

Insert the refill between `lock();` and the channel loop:

```c
static void mix_grain(void) {
  memset(g_acc, 0, sizeof g_acc);

  lock();

  /* Refill streams before mixing, retiring only what every channel reading them
   * has already passed. A stream with no live reader is still topped up, so it
   * is ready the moment the game unpauses it. The feeder runs under this lock
   * on purpose: it is what makes closing a stream from Snd_release safe.
   * FEED_MAX_FRAMES is what keeps the hold time short. */
  for (int i = 0; i < MAX_SOUNDS; i++) {
    Snd *s = &g_snd[i];
    if (!s->used || !s->st) continue;
    uint64_t consumed = (uint64_t)-1;
    for (int c = 0; c < g_nchannels; c++) {
      Chan *ch = &g_chan[c];
      if (ch->used && ch->playing && ch->snd == s) {
        uint64_t p = (uint64_t)ch->pos;
        if (p < consumed) consumed = p;
      }
    }
    if (consumed != (uint64_t)-1) audio_ring_retire(&s->st->ring, consumed);
    audio_ring_feed(&s->st->ring, stream_fill_cb, s->st, FEED_MAX_FRAMES);
  }

  for (int c = 0; c < g_nchannels; c++) {
```

- [ ] **Step 2: Add the streamed channel branch**

The channel loop currently begins:

```c
    Chan *ch = &g_chan[c];
    if (!ch->used || !ch->playing || ch->paused || !ch->snd) continue;
    Snd *s = ch->snd;
    const int16_t *src = s->pcm.pcm;
```

Insert the streamed branch immediately after `Snd *s = ch->snd;`, **before** `const int16_t *src`. Order matters: a streamed `Snd` has `pcm.pcm == NULL`, so if this branch came later every stream would be mistaken for a silent placeholder and play nothing.

```c
    Snd *s = ch->snd;

    if (s->st) {                      /* streamed: read the decoded window */
      AudioRing *ring = &s->st->ring;
      float pan;
      float g3 = chan_3d_gain(ch, &pan);
      float v  = ch->vol * g3;
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
            chan_finish(ch);
            break;
          }
          ch->pos += ch->step;
          g_stream_underruns++;
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
```

`chan_finish`, not a bare `ch->playing = 0`. `chan_finish` sets `end_pending`, which is what causes `System::update` to deliver the END callback. The engine caches playback state and the only writer of that flag is its own `HandleChannelEnd`, reached only from that callback — a stream that ends without it leaves the game believing the track is still playing, and its music playlist stalls.

- [ ] **Step 3: Build and verify**

```bash
touch loader/log.c && cmake --build build -j"$(nproc)" 2>&1 | tail -5
```

Expected: `Built target KOTOR.vpk-vpk`, zero warnings.

- [ ] **Step 4: Commit**

```bash
git add loader/audio_patch.c
git commit -m "Mix streamed sounds from the decoded window

Refill runs before the channel loop and retires only what every channel reading
a stream has already passed, so two channels on one track cannot pull the window
out from under each other. Streams with no live reader are still topped up, so
an unpause is instant.

The streamed branch has to come before the silent-placeholder check, because a
streamed Sound also has a NULL pcm pointer and would otherwise be mistaken for
silence -- which is the bug this whole change exists to remove.

A finished stream calls chan_finish rather than clearing `playing`, so the END
callback is still delivered; the engine's music playlist advances on that
callback and nothing else. And a stream finishes only when the decoder is done
AND the window is drained: end-of-file is reached up to 0.7 s of audio before
the last frame is mixed, so testing eos alone would clip the end of every track."
```

---

### Task 5: Stream instead of silencing

Wire streaming into `Sys_createSound`, at the exact point where it currently gives up and returns timed silence.

**Files:**
- Modify: `loader/audio_patch.c` — FMOD mode constants (near line 570), `Sys_createSound` (near line 855)

**Interfaces:**
- Consumes: `stream_open`, `stream_close` (Task 3); `AUDIO_STREAM_LONG_ASSETS` (Task 3); existing `snd_alloc`, `big_free`.
- Produces: `createSound STREAMING` log line. This is the task that makes music audible.

- [ ] **Step 1: Add the loop mode constant**

`FMOD_CREATESTREAM` (line 618), `FMOD_OPENMEMORY` (619) and `FMOD_3D` (621) already exist; `FMOD_LOOP_NORMAL` does not. Add it alongside them:

```c
#define FMOD_LOOP_NORMAL   0x00000002
```

- [ ] **Step 2: Try streaming before falling back to silence**

In `Sys_createSound`, the current block is:

```c
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
```

Replace the body of the inner `if` so streaming is attempted first:

```c
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
```

The `return FMOD_OK` deliberately skips the `big_free(owned)` further down: ownership has transferred to the `Stream`, which frees it in `stream_close`.

- [ ] **Step 3: Build and verify the probe strings**

```bash
touch loader/log.c && cmake --build build -j"$(nproc)" 2>&1 | tail -5
strings build/KOTOR | grep -E 'createSound STREAMING|NOT streamable'
```

Expected: `Built target KOTOR.vpk-vpk` with zero warnings, and both strings present.

- [ ] **Step 4: Commit**

```bash
git add loader/audio_patch.c
git commit -m "Stream long assets instead of silencing them

This is the change the rest was for. Where createSound previously saw a track
too large to decode and handed back a correctly-timed silent placeholder, it now
opens a stream over the bytes it already read and returns immediately.

Streaming needs to own the compressed bytes, because the decoder reads them in
place for the life of the handle -- so only the path that allocated them is
eligible, which is exactly the music and long-ambience path. Sounds created from
memory are short effects that already decode whole and cache well, and never
reach here.

Timed silence stays as the fallback for an asset that cannot be streamed. It was
the right holding action when the alternative was an allocation the heap could
not take, and it still keeps the game's pacing rather than sending it into a
retry loop."
```

---

### Task 6: The gain census

Instrumentation, not a fix. With mirroring and the NULL listener orientation both ruled out statically, the remaining hypothesis for bad 3D positioning is that 3D sources are simply mixed too quietly to localise. Measure it so the balance work can be designed rather than guessed.

**Files:**
- Modify: `loader/audio_patch.c` — counters near line 343, `Ch_setVolume` (near line 1085), `mix_grain`, the stats line (near line 930)

**Interfaces:**
- Consumes: existing `Chan`, `Snd`, `g_g3_*` counters.
- Produces: extra fields on the `[snd] stats` line. Nothing depends on this.

- [ ] **Step 1: Add the counters**

Near the existing `g_g3_*` declarations (around line 343):

```c
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
```

- [ ] **Step 2: Census `setVolume`**

`Ch_setVolume` currently reads:

```c
static int Ch_setVolume(void *self, uint32_t v) {          /* softfp float */
  if (!chan_valid(self)) return FMOD_ERR_INVALID_PARAM;
  float f = u2f(v);
  if (f < 0.0f) f = 0.0f; else if (f > 1.0f) f = 1.0f;
  ((Chan *)self)->vol = f;
  return FMOD_OK;
}
```

Make it:

```c
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
```

- [ ] **Step 3: Census what reaches the bus**

In `mix_grain`, both the streamed branch and the non-streamed branch compute `v`. In each, immediately after `float v = ch->vol * g3;` (streamed branch) and after the equivalent line in the existing branch, add:

```c
      if (s->is3d) { g_mix_n3d++; g_mix_sum3d += v; }
      else         { g_mix_n2d++; g_mix_sum2d += v; }
```

In the existing non-streamed branch the gain lines currently read:

```c
    float pan;
    float g3 = chan_3d_gain(ch, &pan);
    float v  = ch->vol * g3;
```

so the census goes directly beneath that third line. This runs once per playing channel per grain — about 45 times per 21 ms at worst, which is nothing.

- [ ] **Step 4: Report it**

In the `[snd] stats` log call, append to the format string, immediately before the trailing `"chans %d used / %d playing / %d endPending of %d"`:

```
"vol2d n=%u min=%.3f max=%.3f avg=%.3f, vol3d n=%u min=%.3f max=%.3f avg=%.3f, "
"bus2d avg=%.3f over %u, bus3d avg=%.3f over %u, "
"streams %u opened / %d live / %u underruns, "
```

and the matching arguments before the `nused, nplaying, npend, g_nchannels` tail:

```c
                 g_vol_n2d, g_vol_min2d, g_vol_max2d,
                 g_vol_n2d ? g_vol_sum2d / (float)g_vol_n2d : 0.0f,
                 g_vol_n3d, g_vol_min3d, g_vol_max3d,
                 g_vol_n3d ? g_vol_sum3d / (float)g_vol_n3d : 0.0f,
                 g_mix_n2d ? g_mix_sum2d / (float)g_mix_n2d : 0.0f, g_mix_n2d,
                 g_mix_n3d ? g_mix_sum3d / (float)g_mix_n3d : 0.0f, g_mix_n3d,
                 g_streams_open, g_stream_decoders, g_stream_underruns,
```

Keep the argument order exactly matching the format string; this call already has many arguments and a mismatch will not be caught by the compiler if the types happen to line up.

- [ ] **Step 5: Record the verified handedness in `config.h`**

The spec requires this and it is the only code change the 3D dead-ends produce. `AUDIO_3D_RIGHTHANDED` is correct at 0, but its comment still reads as a guess and invites someone to flip it. In `loader/config.h`, replace the last line of that comment block — "Set to 1 if positional audio comes out mirrored." — with:

```c
// VERIFIED, do not flip on a hunch: FModAudioSystem::InitSystem passes flags=0
// to FMOD::System::init (an immediate `movs r2, #0` before the call), so
// FMOD_INIT_3D_RIGHTHANDED is not set and the game is left-handed. If the
// stereo image ever sounds mirrored, the cause is elsewhere.
#define AUDIO_3D_RIGHTHANDED 0
```

- [ ] **Step 6: Build and verify the probe reached the binary**

```bash
touch loader/log.c && cmake --build build -j"$(nproc)" 2>&1 | tail -5
strings build/KOTOR | grep 'vol2d n='
```

Expected: `Built target KOTOR.vpk-vpk` with zero warnings, and one match.

- [ ] **Step 7: Commit**

```bash
git add loader/audio_patch.c loader/config.h
git commit -m "Measure what volume actually reaches the bus

Two theories for bad 3D positioning died against static evidence: the game
passes flags=0 to System::init so the stereo image is not mirrored, and the zero
listener orientations in the logs are NULL pointers, which FMOD defines as
leave-this-alone. What is left is audibility rather than geometry -- a source
mixed at a few percent cannot be localised however correct its pan is.

That is a hypothesis, and the mixer computes vol * 3D gain while nothing has
ever logged vol. So census both, split 2D against 3D. The discriminator is
whether the 3D volumes vary at all: a spread means the game attenuates by
distance itself and we are doubling it, a single constant means it expects
FMOD's 3D system to supply the rest and we are taking its base literally. Those
want opposite fixes, so the next change should be designed from the numbers
rather than from a guess.

Stream counters ride along on the same line."
```

---

### Task 7: Hardware verification

The first six tasks produce a binary that builds. This one finds out whether it works. Nothing here is optional — the whole point of the design was to earn one good hardware session.

**Files:**
- Modify: `docs/specs/2026-08-23-audio-streaming-and-3d.md` (record the outcome)

- [ ] **Step 1: Confirm the branch and build the shipping VPK**

```bash
git branch --show-current    # must be audio-streaming
touch loader/log.c && cmake --build build -j"$(nproc)" 2>&1 | tail -3
git branch --show-current    # print it again: builds have been made on the wrong branch before
```

- [ ] **Step 2: Install and play**

Install `build/KOTOR.vpk`. Play for at least 20 minutes, deliberately covering:
- an area with music (Taris upper city),
- at least one combat encounter and its end, to exercise the battle-to-area music transition,
- at least one area transition,
- some dialogue, to confirm VO did not regress.

Then retrieve `ux0:data/kotor/log.txt`.

- [ ] **Step 3: Check the failure that matters first**

```bash
grep -c '0x807F0007' log.txt
```

Expected: **0**. Any match means the decoder pool is exhausted and VO is being starved — the exact regression this design is built to avoid. If this is non-zero, stop and reduce `AUDIO_MP3_STREAM_MAX`, or raise `AUDIO_MP3_DECODER_POOL` toward its ceiling of 6.

- [ ] **Step 4: Check that music is actually streaming**

```bash
grep -c 'createSound STREAMING' log.txt      # expect: many
grep -c 'createSound stream SILENT' log.txt  # expect: 0, or only genuine decode failures
grep -c 'NOT streamable' log.txt             # expect: 0
grep 'stream decoder cap reached' log.txt    # expect: nothing, or rare
```

- [ ] **Step 5: Check nothing else regressed**

```bash
grep 'snd] stats' log.txt | tail -1
```

On that line confirm: `0 decode fail`, `0 badsnd`, `0 nochan`, and `underruns` low relative to grains. Then:

```bash
grep 'heap]' log.txt | tail -2      # headroom near log163's ~37 MB free
grep 'big]'  log.txt | tail -2      # 0 segment fails
```

- [ ] **Step 6: Read off the census**

From the last `snd] stats` line, record `vol3d min/max`. If `min == max` the game passes a constant and expects FMOD to supply 3D gain; if they differ, the game is attenuating and we are doubling it. These want opposite fixes — this number is the whole input to the next piece of work.

- [ ] **Step 7: Record the outcome in the spec**

Append a "Result" section to `docs/specs/2026-08-23-audio-streaming-and-3d.md` giving: whether music was audible, the counts from steps 3–5, the census figures from step 6, and whether the post-battle music delay survived. Say plainly what did not work, if anything did not.

- [ ] **Step 8: Commit**

```bash
git add docs/specs/2026-08-23-audio-streaming-and-3d.md
git commit -m "Record what the hardware said about streaming music"
```

---

## Notes for whoever executes this

- **Print the branch before and after every build.** Builds have gone to the wrong branch on this project before.
- **`strings build/KOTOR | grep` after adding any log line.** A clean build is not evidence your probe is in the binary.
- **Do not raise `STREAM_PCM_MAX`.** It is not a tuning knob; it is the boundary between the decode-whole path and the streaming path, and raising it puts 15 MB decodes back into a shared arena that has crashed on exactly that before.
- **Do not "fix" the balance in this branch.** It is deliberately deferred until music is in the mix and the census has reported. Tuning it now means tuning against a bus that is about to change.
