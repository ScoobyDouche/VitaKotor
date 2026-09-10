# Bink video playback

Status: enabled by default and validated on real Vita hardware.

## Current behavior

KOTOR's Android companion contains its complete Bink decoder and YUV renderer.
The loader enables that implementation rather than replacing the codec:

- `MacCreateBinkShaders` creates the embedded YUV shader normally.
- `MacPlayBinkGL` receives every original movie path from the game.
- Stored `.bik` members are read and sought directly inside the mounted OBBs.
- Decoded movie PCM enters the existing 48 kHz Vita mixer through the loader's
  minimal OpenSL-compatible object graph.
- Physical controller buttons use the companion's existing skip behavior after
  its built-in delay.
- Missing companion exports or a failed player trampoline fall back to the skip
  stub, allowing the game to continue.

The audio-pump hook is tied to the tested version 53 companion binary at
`libandroid_port.so + 0x85670`. If that trampoline cannot be installed, the
loader logs the failure and the embedded player remains available, but movie
audio may be silent because frame-driven pumping is unavailable.

`MacDecompress` is intentionally untouched. Despite its name, it is KOTOR's LZMA
resource decompressor for `.bzf` game data, not part of movie playback.

## Build policy

The default in `loader/config.h` is `BINK_MODE_PLAY`. `CMakeLists.txt` always
builds `loader/bink_audio_queue.c` and `loader/opensl_patch.c` and defines
`KOTOR_USE_BINK_OPENSL=1` so movie PCM is available to `audio_patch.c`.

Three optional regression modes remain:

- `KOTOR_BINK_SHADER_TEST=ON`: real shader initialization, movies skipped.
- `KOTOR_BINK_LEGAL_VIDEO_TEST=ON`: substitute silent `legal.bik` for the first
  request, then skip later movies.
- `KOTOR_BINK_OPENSL_TEST=ON`: substitute `01c.bik` for the first request, then
  skip later movies.

Only one regression mode may be selected. `BINK_MODE_SKIP` also remains available
as a direct compile definition for emergency fallback builds.

## Video path

`MacPlayBinkGL` runs synchronously on the game thread, which owns the vitaGL
context. The embedded player:

1. Resolves the requested movie from loose files or a stored OBB entry.
2. Creates CPU and GL textures for Y, Cr, Cb, and optional alpha planes.
3. Decodes frames with its internal workers.
4. Uploads each plane and converts YUV to RGB with the embedded shader.
5. Draws subtitles through KOTOR's normal wrapped-text path when a localized
   sidecar is available.
6. Closes the decoder, frees frame resources, clears/presents the final frame,
   and sets the companion's render-skip handoff on completion or permitted skip.

The loader records movie duration, presented swaps, frame interval average/max,
plane upload count/bytes, file-handle balance, audio queue state, and pump state.

## Audio path

The embedded Bink sound adapter expects a small OpenSL ES surface. The loader
implements only that observed contract:

- Engine, output-mix, and up to four audio-player objects.
- Object realization, interface lookup, and destruction.
- Play-state control.
- Buffer queue enqueue, clear, state, callback registration, and callbacks.
- Volume and stereo-position setters.

PCM buffers are queued without copying in a bounded queue of at most 38 entries.
The existing Vita audio thread mixes them before its global limiter. Mono and
stereo sources are accepted; source rates are linearly resampled to the mixer's
48 kHz output rate.

The companion's async Bink audio worker pumps once before the first decoded-frame
sequence is published and is not signalled again on Vita. The loader hooks the
companion's internally locked pump at `libandroid_port.so + 0x85670`, captures
the active Bink handle, and services the pump once per frame-sequence advance from
the movie swap path. Pump state and counters reset before every movie and stop
before Bink destroys its handle.

This hook offset must be revalidated before supporting a different Android
binary. The exported player and shader entry points are resolved by symbol and do
not have that version-specific constraint.

Destroy is serialized against queue callbacks. Player destruction clears queued
buffers, removes the callback, marks playback stopped, and decrements the active
player count before returning.

## Movie inventory

The tested version 53 OBBs contain 62 unique `BIKi` movies totaling about
389.68 MiB and 27 minutes 19 seconds:

| Profile | Count | Frame rate | Audio |
|---|---:|---:|---|
| 640x272 | 53 | 29.97 fps | Usually 44.1 kHz stereo |
| 640x360 | 5 | 29.97 fps | 44.1 kHz stereo, one at 48 kHz |
| 640x480 | 2 | 29.97 fps | 44.1 kHz stereo |
| 1024x576 | 1 | 30 fps | 48 kHz stereo |
| 1024x768 | 1 | 29.97 fps | No audio |

Sixty-one movies contain Bink DCT audio. `legal.bik` is the only silent movie.
Some campaign movies include localized subtitle sidecars such as
`01a.bik.frsub.txt`.

## Validation

Validated on real Vita hardware:

- Embedded Bink shader compiled and linked successfully.
- Silent `legal.bik` completed in 5,027 ms with 150 swaps and no handle leak.
- `01c.bik` completed with audible 44.1 kHz stereo audio through the existing
  mixer. It used 499 forced pump calls, 1,016 stereo queue blocks, and returned
  both audio players to the free state.
- Production mode forwarded the original `01A.bik` path unchanged.
- `01A.bik` played for 101,044 ms before a physical-button skip, presenting
  3,028 swaps at 33/63 ms average/max frame intervals.
- Its 44.1 kHz stereo player enqueued 6,199 blocks and mixed 4,846,592 output
  frames. The companion's secondary 24 kHz mono track was also active.
- Both players were destroyed, active players returned to zero, OpenSL errors
  remained zero, the 39 buffers still queued at skip were discarded, file handles
  stayed balanced at `2 -> 2`, and gameplay resumed.

Host tests cover queue capacity, clear, exact completion, zero-copy buffer
lifetime, wraparound, cross-buffer interpolation, 44.1-to-48 kHz resampling, and
underrun accounting. Production plus skip, shader-only, silent-video, and
one-movie A/V configurations compile successfully.

## Remaining coverage

These are untested compatibility areas, not known failures:

- A representative 48 kHz movie.
- Localized subtitle timing and language selection.
- Several consecutive movies in one session, including both natural completion
  and skip, to extend leak and teardown coverage.

The embedded player still calls `glTexImage2D` for each YUV plane every frame.
Hardware sustained the tested 640x272 movie at its intended cadence, so persistent
textures plus `glTexSubImage2D` should only be considered if larger profiles show
measurable upload or allocation pressure.

## Rejected alternatives

- VitaSDK's full OpenSL backend opened a competing BGM port, produced no movie
  audio, and faulted during teardown.
- A separate FFmpeg/OpenAL player would duplicate KOTOR's OBB seeking, subtitle,
  skip, and render-handoff behavior while adding dependencies and another audio
  owner.
- Extracting roughly 390 MiB of movies would duplicate data and would not solve
  decoding, rendering, or audio integration.
