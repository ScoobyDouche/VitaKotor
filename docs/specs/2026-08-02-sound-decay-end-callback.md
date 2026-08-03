# Sound decays to silence, then the game falls over

**Status:** root cause identified by disassembly and confirmed against log139;
fix implemented, **not yet verified on hardware**.
**Date:** 2026-08-02

The report: sound thins out the longer you play, and eventually even dialogue
goes quiet. log139 is the first capture that ran long enough (21m 49s) to show
where it ends up — the app dies.

## What the log shows

| t (s) | event |
|---|---|
| 25 | first stream created |
| 1088 | `out of virtual handles for ux0:data/kotor/main.obb`, and 170 more after |
| 1164 | 8832 `createSound`, 25 MB of PCM held, **0 decode failures** |
| 1307 | `OUT OF MEMORY: 1334 KB for 1 ch @ 32000 Hz` — the heap is gone |
| 1307+ | every VO becomes `decode failed, substituting 9360 ms silence` |
| 1309 | log ends mid-line |

Two exhaustions, roughly 220s apart, and the audible symptom is the second one:
once malloc cannot find 1.3 MB, every voice line decodes to nothing and the game
plays timed silence instead. Nothing is wrong with the assets or the mixer.

The file-handle side has the clearer fingerprint. `FModAudioSystem::CloseStream`
was reached 144 times for 216 streams the game opened. 72 leaked `SDL_RWops` on
`main.obb`, with open handles climbing 6 -> 12 -> 22 -> 47 across the run. The
game only closes a stream once it believes that stream stopped playing.

So: why does the game never believe anything stops playing?

## The game does not ask FMOD whether a sound is playing

This is the part that had been assumed the other way round. From
`libandroid_port.so`:

```
FModAudioSystem::PlaySound        +0x735a1
    ...
    73682: blx  ChannelControl::setUserData(chan, channelInfo)
    7368e: blx  ChannelControl::setCallback(chan, FModAudioSystem::ChannelCallback)

FModAudioSystem::ChannelCallback  +0x73775
    73792: blx  ChannelControl::getUserData -> channelInfo
    7379a: cmp  r4, #0                  ; r4 = callbacktype
    737ac: blx  FModAudioSystem::HandleChannelEnd(channelInfo)   ; only when == 0 (END)

FModAudioSystem::HandleChannelEnd +0x74a75
    74c30: (non-looping branch)
    74c3e: str  r0, [r5, #28]           ; r0 = 1 -- the ONLY writer of ChannelInfo+0x1c
    74ab4: (looping branch) playSound again, restore priority/freq/volume/3D/pan

FModAudioSystem::GetIsChannelPlaying +0x746a5
    746ec: ldr  r0, [r0, #28]
    746ee: clz  r0, r0
    746f2: lsr  r0, r0, #5              ; "playing" == (ChannelInfo+0x1c == 0)
```

`GetIsChannelPlaying` never calls into FMOD. It reads a cached flag, and the only
thing that ever sets that flag is the END callback.

`FMOD::ChannelControl::setCallback` was bound to a no-op stub. Therefore
`ChannelInfo+0x1c` stayed zero for the life of the process, and **every sound the
game had ever started remained "playing" forever.** That single fact produces all
of it:

- channel slots are never recycled, so `playSound` dries up after roughly two
  minutes of play — the original "sounds get sparser" complaint;
- streams are never closed, because the engine closes a stream only after it
  observes it stop — the handle leak and the OBB open failures;
- the leaked channel and stream sources accumulate until the heap is full — the
  OOM, the universal silence, and the crash;
- looping ambience plays exactly once, because the loop restart lives in
  `HandleChannelEnd`'s other branch.

It also explains an earlier dead end. `FMOD::ChannelControl::isPlaying` was
carefully implemented and then observed to be called *zero* times in 144s. That
was not a sign the game had stopped caring about audio; it was the clue that the
game reads its own flag and we were never setting it.

## Fix

`FMOD::ChannelControl::setCallback` now records the callback per channel, the
mixer flags a voice when it runs out of samples, and `FMOD::System::update`
delivers one END per completed voice.

Delivery point matters. Real FMOD dispatches channel callbacks from
`System::update`, on the caller's thread, precisely because a callback may
re-enter the API — and `HandleChannelEnd` does exactly that, calling `playSound`,
`setUserData` and `setCallback` straight back into us to restart a loop. The
companion calls `System::update` from `FModAudioSystem::UpdateSystem` (+0x72ecd),
which `libKOTOR` drives from its per-frame audio update: the same thread that
calls `playSound`, and never with our mixer lock held.

Two cases are deliberately *not* an END:

- **Explicit `stop()`.** `FModAudioSystem::StopChannel` (+0x73cd1) already calls
  `ChannelInfo::Reset` itself immediately after `stop()`. A late END on a looping
  voice would take `HandleChannelEnd`'s restart branch and bring back the sound
  the game just silenced.
- **`Sound::release`.** The source is gone; there is nothing to report.

A voice that finishes before the companion gets back from `playSound` to install
its callback still works — a 3 ms effect does happen — because completion is
recorded on the channel and waits for a callback to appear.

The `[snd] stats` line now carries `N END sent over M updates`. If `M` is zero on
the next hardware run then `UpdateSystem` is not being driven and the delivery
point is wrong; everything else here still holds.

## Verified

Host test, compiling `loader/audio_patch.c` itself against a vitasdk shim, nine
scenarios: single END per natural completion and never a repeat; completion
before the callback is installed; `stop()` before *and* after completion, both
silent; `Sound::release`; a paused voice never ending; timed-silence streams
ending on schedule; a callback that re-enters `playSound` neither deadlocking nor
recursing; and 40 voices in sequence yielding exactly 40 ENDs with no live-voice
stealing. Mutating the dispatch to run under the lock, or dropping the
stop-suppression, each fails the suite.

Not yet verified: hardware. What to look for over a run of 20 minutes or more —

- `N END sent` climbing in step with `createSound`;
- no `out of virtual handles for main.obb`, and `[io] files: N open now` flat
  rather than climbing 6 -> 12 -> 22 -> 47;
- no `OUT OF MEMORY`, no `decode failed, substituting ... silence`;
- voices still audible, and dialogue still waiting for its line to finish.

## Not addressed

`stream too large: id=181 would need 18408 KB ... playing 427437 ms of SILENCE`.
427 seconds does not agree with 18 MB at 32 kHz mono (that is 294 s), so the
duration probe is wrong for at least that asset. It costs a channel for as long
as it claims to run, which is bad but bounded, and it is independent of the
above.
