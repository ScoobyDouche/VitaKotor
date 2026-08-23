# Streaming music, and two 3D corrections that cost nothing

Status: design, approved 2026-08-23. Supersedes the shelved
`backups/audio-streaming-wip-2026-08-01.patch`, which is the starting point for
the streaming half and is described here where it differs from what we should
build.

## The problem

Every long audio asset in the game plays as silence. In log163 — a 33-minute
session across four Taris areas — 26 of 272 `createSound` stream calls produced
a silent placeholder instead of audio:

```
[snd] stream too large: id=1 would need 15125 KB (cap 6144 KB, in use 0 KB)
      -- playing 87805 ms of SILENCE
```

Those 26 are every music track and every long ambience bed in the session. The
other 246 are short VO and effects under the cap, which decode normally. So the
score has never been audible on hardware, and neither have the long
environmental loops.

The cause is `STREAM_PCM_MAX`, 6 MB in `loader/audio_patch.c`. A music track is
1.3–1.4 MB of MP3 that decodes to 14–24 MB of PCM. `Sys_createSound` probes the
asset, sees the decoded size exceed the cap, and hands back a correctly-timed
silent `Sound` rather than attempt the allocation. That was the right call when
it was made — a 15 MB decode into the shared arena is exactly what crashed
log142 — but it was always a holding action.

Note for anyone reading the logs: the `stream too large` line is capped at 12
occurrences by `g_overbudget < 12`, while `createSound stream SILENT` is not.
Counting the former undercounts the problem by half. Count `createSound stream
SILENT`.

Two further complaints from hardware, both about the mix rather than about
silence: the balance is wrong (ambience sitting over footsteps and dialogue),
and 3D positioning does not sound right. This spec fixes what can be fixed
without guessing, and instruments the rest.

## What we are building

Two changes, in one build.

1. **Incremental stream decode.** Long assets keep their compressed bytes and a
   small PCM ring, refilled from the audio thread, instead of being decoded
   whole or replaced with silence.
2. **A gain census.** Log what `setVolume` actually delivers for 2D versus 3D
   channels, and what each channel contributes after the 3D gain is applied.

A third change was planned and then dropped: deriving the 3D handedness from
the FMOD init flags rather than from a constant. Disassembly settled it instead
— see "Two 3D theories, both dead" below. Neither of the two leading
explanations for "3D positioning sounds wrong" survives contact with the
evidence, which is why the census is now the whole 3D deliverable rather than a
supporting measurement.

Balance is deliberately *not* in this build. Tuning it now means tuning against
a bus that has no music in it, and the whole point of change 1 is to put music
in the bus. The census exists so the next build can be designed from
measurements rather than from a hypothesis.

## 1. Incremental stream decode

### Approach

Rejected first, because both alternatives look simpler:

- **Raise `STREAM_PCM_MAX` and let `bigalloc` serve the 15 MB.** The pool
  peaked at 31 of 32 MB in log163 with 297 allocations already falling back to
  malloc, so the room is not there. Worse, a whole-track decode blocks the
  calling thread for seconds at *every* track change, which would show up as
  music arriving late after a fight — a symptom already reported. Raising the
  cap would make it worse.
- **Chunked decode into one growable buffer.** No ring, no wraparound, mixer
  almost unchanged — but it still ends up holding the whole 15 MB, which is the
  log142 crash with extra steps.

So: hold the compressed elementary stream (~1.4 MB, already allocated by
`Sys_createSound` as `owned`) plus a ring of decoded PCM, and refill the ring
from the audio thread as the mixer drains it. A track costs ~1.5 MB instead of
15 MB, and starting one costs no decode stall at all.

### The decoder

`loader/audio_mp3.c` gains an incremental API alongside `audio_mp3_decode`. The
shelved patch's version of this applies to today's tree cleanly and its design
is sound; take it as written:

```c
typedef struct AudioMp3Stream AudioMp3Stream;

AudioMp3Stream *audio_mp3_stream_open(const void *data, unsigned len, AudioPcm *fmt);
unsigned        audio_mp3_stream_read(AudioMp3Stream *s, int16_t *dst, unsigned frames);
int             audio_mp3_stream_eos(const AudioMp3Stream *s);
void            audio_mp3_stream_rewind(AudioMp3Stream *s);
void            audio_mp3_stream_close(AudioMp3Stream *s);
```

`audio_mp3_stream_open` reuses `audio_mp3_probe`, `find_sync` and `rate_of` so
that duration, rate and channel count match what the whole-decode path would
have reported — the game reads `getLength` for its own pacing, and a stream
that reports a different length than a decoded copy of the same asset would
desynchronise it. It fills `fmt` with `pcm = NULL`, which is what marks a
`Snd` as streamed.

The caller owns `data` and must keep it alive and unmoved until
`audio_mp3_stream_close`. `sceAudiodec` reads the elementary stream in place.

`audio_mp3_stream_read` drains a carry buffer first, because one
`sceAudiodecDecode` emits a whole granule pair (up to 1152 frames) and the
caller usually wants fewer; the remainder waits in `carry` rather than being
decoded twice. It uses the same resync rule as the whole-asset path — a few bad
frames at an edge are normal, 64 in a row means the stream is finished or
broken.

### The decoder pool

This is the part that has already caused one hardware regression, so it gets
stated plainly.

`sceAudiodecInitLibrary` takes `p.mp3.totalStreams`, currently **1**. That is a
hard pool, not a hint: the next `sceAudiodecCreateDecoder` fails `0x807F0007`.
It went unnoticed for months because `audio_mp3_decode` creates a decoder,
decodes, and deletes it before returning — never two alive at once.

Streaming breaks that invariant by design, and when the shelved patch first did
so on hardware (log130) the single slot went to the first music track and every
later decode failed. VO became substituted silence and dialogue raced, because
`CExoStreamingSoundSource::IsPlaying()` then reports nothing playing.

So:

```c
#define AUDIO_MP3_DECODER_POOL 4   /* SDK ceiling is SCE_AUDIODEC_MP3_MAX_NSTREAMS (6) */
#define AUDIO_MP3_STREAM_MAX   2   /* strictly below the pool: spares stay free */
```

`AUDIO_MP3_STREAM_MAX` must stay strictly below `AUDIO_MP3_DECODER_POOL` so
that a spare slot always exists for the short synchronous decodes. Two
concurrent streams covers what the game actually does — log163 never has more
than two long assets alive at once (an area track plus an ambience bed) — and
leaves two spares. `stream_open` refuses past the cap and the caller falls back
to timed silence, so even a leaked stream can never starve VO the way log130
did.

`sceAudiodecInitLibrary` runs once. A second call returns `0x807F0002`
("already initialised") and the value cannot be changed afterwards, so it has
to be right the first time.

### The ring

```c
#define RING_FRAMES      32768u   /* 0.74 s at 44.1 kHz; 128 KB at stereo */
#define FEED_MAX_FRAMES  4096u    /* per-grain refill bound */

typedef struct {
  AudioMp3Stream *dec;
  void     *src;       /* owned compressed ES; the decoder reads from it */
  int16_t  *ring;      /* RING_FRAMES * ch, interleaved */
  unsigned  ch;
  uint64_t  base;      /* absolute frame index of the oldest valid frame */
  unsigned  fill;      /* valid frames from base */
  int       loop, eos;
} Stream;
```

Frames are written sequentially, so absolute frame `f` always lives at
`f % RING_FRAMES` and a refill is at most two `memcpy`-shaped chunks either
side of the wrap.

`FEED_MAX_FRAMES` bounds how much one grain may decode. The mixer consumes
`OUT_GRAIN` (1024) frames per grain; 4096 keeps the ring ahead with room to
recover after a stall, without letting a single refill overrun the output
deadline.

`Snd` gains `Stream *st`, non-NULL exactly when `pcm.pcm` is NULL and the sound
is streamed.

### Locking

The ring is written only by `stream_feed` and read only by `mix_grain`, both on
the audio thread, so the ring itself needs no lock of its own. The existing
mixer lock is still held across both, because channel positions are shared with
the game's threads — and holding it across the refill is what makes closing a
stream from `Snd_release` safe: the feeder can never be mid-decode on a `Stream`
that is being freed. `FEED_MAX_FRAMES` is what keeps that hold time short.

`g_stream_decoders` is mutated under the mixer lock in all cases. The shelved
patch decremented it inside `stream_close`, which it then called *outside* the
lock — a race on the counter that guards the pool. Detach and decrement under
the lock; free afterwards.

### Integration points in `audio_patch.c`

**`mix_grain`** — refill first, then mix. For each live streamed `Snd`, compute
the minimum `pos` across every playing channel reading it and feed from there;
a stream with no live reader is still topped up so it is ready the moment the
game unpauses it. Then, in the per-channel loop, the streamed branch must come
**before** the `if (!src)` silent-placeholder branch, or every stream will be
mistaken for a placeholder and play silence.

**End of stream — the one place the shelved patch is wrong.** Its streamed
branch ends a finished channel with `ch->playing = 0`. That predates commit
`ee790b8`, which introduced `chan_finish`/`chan_retire` and the END callback the
engine relies on. `FModAudioSystem` caches playback state and the sole writer of
that flag is `HandleChannelEnd`, reached only from the END callback. A stream
that clears `playing` without setting `end_pending` never delivers END, so the
game never learns the track finished and its music playlist stalls — which is
very likely the reported "music after a battle comes in late". Use
`chan_finish(ch)`, exactly as the non-streamed path does.

**Underrun** — outside the decoded window but not at end of stream, stay silent
for that sample and keep `ch->pos` advancing, so timing does not drift.

**`Sys_createSound`** — where it currently falls through to timed silence, try
`stream_open` first. Streaming needs `owned` (we must be the ones who allocated
the elementary stream, since the decoder reads it for the life of the handle),
so the `FMOD_OPENMEMORY` path is not eligible and does not need to be: those are
short effects already in memory. If `stream_open` refuses — pool cap, or a
non-decodable asset — fall back to the existing timed silence, which at least
keeps the game's pacing and stops the retry loop.

Streams are never inserted into the PCM cache. Each owns its decoder, ring and
compressed bytes outright.

**`Snd_release`** — detach channels as it does now (preserving the existing
`g_rel_kill_live` / `g_rel_kill_pend` accounting), then, still under the lock,
take `Stream *st = s->st; s->st = NULL;` and decrement `g_stream_decoders`.
Call `stream_close(st)` after unlocking; it only frees memory and deletes the
decoder.

**`stream_close` must free `src` with `big_free`, not `free`.** The shelved
patch uses plain `free`, and `Sys_createSound` allocates `owned` with
`big_malloc`. A 1.4 MB music asset is well above `BIGALLOC_MIN_BYTES`, so it
comes from the pool every time, and `free` on a pool pointer is wrong. Only the
game's imported `free` does the address-range check; the loader's own calls do
not.

**Unchanged and worth confirming:** `Snd_getLength` and `Ch_getPosition` both
read `s->pcm` (`nsamples` / `ms` / `rate`), which the stream path fills from
`fmt`. Both work for streams with no edit.

### What does not change

`STREAM_PCM_MAX` keeps its value and its meaning, but its meaning narrows: it
is now the ceiling for the *decode-whole* path, above which an asset is
streamed instead of silenced. Short VO and ambience stay on the whole-decode
path so they can be cached and shared, which is what the 246-of-272 cache hit
rate in log163 depends on.

## 2. Two 3D theories, both dead

Recorded here so neither gets re-investigated. Both were plausible, and both
were settled without hardware.

### The stereo image is not mirrored

`AUDIO_3D_RIGHTHANDED` in `config.h` is a hand-set constant, and the suspicion
was that it guesses wrong at something the game states on every boot. FMOD is
left-handed unless the game passes `FMOD_INIT_3D_RIGHTHANDED` to
`System::init`, and the two conventions produce exactly opposite right vectors
— a mirrored stereo image, inaudible as a defect and wrong every time.

`FModAudioSystem::InitSystem` in `libandroid_port.so` settles it:

```
72ca6:  ldr   r1, [r4, #4]     ; r1 = m_system
72ca8:  add.w r5, r8, #5       ; maxchannels = arg + 5
72cae:  movs  r2, #0           ; flags
72cb0:  movs  r3, #0           ; extradriverdata
72cb2:  mov   r0, r1           ; this
72cb4:  mov   r1, r5
72cb6:  blx   FMOD::System::init
```

`flags` is an immediate zero — `FMOD_INIT_NORMAL` — so no handedness bit is set
whichever FMOD generation's value applies. The game is left-handed and
`AUDIO_3D_RIGHTHANDED 0` is already correct. (`maxchannels = arg + 5`
independently matches the existing comment in `audio_patch.c`, confirming this
is the right call site.)

Action: none, beyond amending the `config.h` comment to record that the flag
word was read as 0, so the constant is documented as verified rather than
guessed.

### The NULL listener orientation is correct behaviour

A third of the listener updates in log163 print `fwd=(0.00,0.00,0.00)
up=(0.00,0.00,0.00)`, which looks alarming and is not. `g_lis_degenerate` is 0
across the whole session, so the basis computation was never reached with zero
vectors — the `if (fwd && up)` guard was simply false, meaning the game passed a
NULL orientation pointer. Real FMOD treats NULL as "leave this attribute
alone", so keeping the previous basis is correct. No change.

With both of these gone, the remaining explanation for the reported positioning
problem is audibility rather than geometry: a source mixed at a few percent of
full scale cannot be localised no matter how correct its pan is. That is what
the census below is for.

## 3. The gain census

The mixer computes `v = ch->vol * g3`, and nothing logs `ch->vol`. The
hypothesis worth testing is that the game passes a small constant to
`setVolume` for 3D sounds — the shelved patch measured `0.0512`, never varying
with distance — because real FMOD's 3D system was expected to supply the rest.
If so, 3D sources sit near 5% of full scale while 2D UI and the ambient bed sit
at 85–100%, which would produce both reported symptoms at once: ambience
drowning everything, and positioning that cannot be heard well enough to
localise.

That is a hypothesis. It needs measurement, not a fix invented around it. Add
to the periodic `[snd] stats` line:

- `setVolume` distribution, bucketed, split 2D versus 3D: count, min, max, mean.
- The post-`g3` contribution per channel, same split — what actually reaches the
  bus.
- Whether the 3D volumes vary at all with distance, which is the discriminator
  between "the game expects FMOD to attenuate" and "the game attenuates and we
  are doubling it".

Fold these into the existing stats line rather than adding a new one. It is
already emitted every 128 `createSound` calls, which is frequent enough to
watch the numbers move and rare enough not to cost frames.

## Configuration

New flags in `loader/config.h`, so a hardware regression can be bisected
without a rebuild cycle per suspect:

- `AUDIO_STREAM_LONG_ASSETS` (default 1) — 0 restores the timed-silence
  behaviour of v0.1.11 exactly.

The census is diagnostics and rides under the existing `LOG_DIAGNOSTICS`, which
stays 1 in shipped builds.

## How we will know it worked

On hardware, in one session:

- `createSound stream SILENT` count drops to zero, or to only assets that
  genuinely fail to decode. `createSound STREAMING` lines appear in its place
  with plausible durations.
- Music is audible, and changes when the area or combat state changes.
- No `CreateDecoder failed 0x807F0007` anywhere in the log. This is the log130
  regression and it is the single thing most worth grepping for first.
- `decode fail` stays 0 and the `playSound N calls / 0 badsnd / 0 nochan` line
  stays clean — proof that streaming has not starved the short decodes.
- Heap and pool telemetry hold: arena headroom stays near log163's ~37 MB free
  rather than collapsing, and `[big]` shows no `segment fails`.
- The census reports a 2D/3D volume split we can design the balance fix from.

The post-battle music delay is expected to improve or resolve, for two reasons:
END is now delivered for streams (see above), and track changes no longer stall
the calling thread on a multi-second decode. If it survives both, it is the
game's own music manager and we will have the END accounting to prove it.

## Risks

**The decoder pool is set once per boot.** If 4 turns out to be wrong we cannot
change it at runtime; it needs a rebuild. Mitigated by keeping
`AUDIO_MP3_STREAM_MAX` strictly below it and refusing past the cap.

**The audio thread now does work with a deadline.** A refill that overruns
produces an underrun, which is audible as a dropout rather than as a crash.
`FEED_MAX_FRAMES` bounds it; the underrun path keeps the clock moving so a
dropout cannot turn into drift.

**Streaming holds the elementary stream for the life of the track.** That is
~1.4 MB from the bigalloc pool, which peaked at 31 of 32 MB in log163. Two
concurrent streams is ~3 MB. Worth watching in the `[big]` line; if the pool is
the binding constraint, its size is a separate conversation from this one.
