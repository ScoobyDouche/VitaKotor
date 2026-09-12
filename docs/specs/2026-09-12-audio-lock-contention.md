# Audio mixer lock contention

Status: ported from KOTOR II; audio smoke-tested on real Vita, performance gain
not yet established by a controlled A/B.

## Motivation

KOTOR I and KOTOR II use the same custom FMOD backend design. Before this
change, the audio worker held the shared kernel mutex while resampling and mixing
every active channel for an entire 1,024-frame output grain. `System::update`
also acquired and released that mutex once for every channel on every call.

KOTOR II hardware instrumentation measured the game thread spending about 93 ms
of a busy frame waiting across roughly 11,200 audio-mutex acquisitions. Moving
the per-sample mix outside the lock and batching the END scan reduced
`System::update` from about 2.5 ms to 0.03 ms on that game while preserving
audio. Commit `0420f9643b62801debacbc672a6cabdd61d873f7` contains the source
implementation used as the reference.

## KOTOR I adaptation

The KOTOR I backend has the same `Snd`, `Chan`, PCM cache, streaming ring,
callback, and mixer structure, so the synchronization design was portable
without copying KOTOR II-specific channel sizing, module-load barriers, null
audio mode, or performance instrumentation.

The KOTOR I implementation now:

- snapshots listener and active-channel state under one short mutex hold;
- performs stream/PCM resampling, attenuation, panning, and accumulation without
  holding the game-facing mutex;
- writes playback positions and completion state back under a short lock;
- uses a monotonically increasing channel generation to reject stale write-back
  after channel allocation, stop, seek, or sound release;
- defers PCM cache frees while an unlocked mix snapshot may still reference the
  sample buffer;
- collects all pending FMOD END callbacks under one `System::update` lock and
  invokes them after releasing it, preserving callback re-entry.

KOTOR I retains its existing 64 playable channels plus 32 callback-retirement
slots and its existing Bink/OpenSL path.

## Validation

Automated validation completed:

- Full Vita build and VPK packaging succeeded.
- Audio-ring tests passed.
- Bink audio-queue tests passed.
- Real audio fixture tests passed for all three long ADPCM beds, the MP3 music
  container, prefixed PCM, rewind behavior, mixed read sizes, and transient
  in-memory regressions.

Real-Vita smoke testing completed without a crash or deferred-PCM overflow.
Music and effects remained functional. Stream decoding, playback completion,
END callbacks, and channel reuse continued in the hardware log.

The user did not hear an audio regression, but did not observe an obvious frame
rate change. The run did not hold a fixed camera and workload, so it is not a
valid performance A/B. A controlled before/after capture in the same area is
still needed before claiming an FPS improvement for KOTOR I.

## Artifact

Test executable SHA-256:

```text
4928ee144fd8b4a4b5eedc649c209ec719b919d650414e7ca5826ce19b4f58c8
```

The previously installed hardware executable was backed up with SHA-256:

```text
215ff614edaddf22614a37c08361c6b33b0166784e82ef49c9de123f20d3c17b
```

Hardware log:

```text
kotor1-hardware-audio-lock-20260911.log
```

That log is archived outside the repository under the OpenCode temporary
directory.
