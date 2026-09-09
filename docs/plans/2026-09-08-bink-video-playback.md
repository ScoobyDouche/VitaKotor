# Bink Video Playback Implementation Plan

Work through the tasks in order. Each task ends at a build or hardware acceptance
gate so it can be reviewed or reverted independently.

**Goal:** Play KOTOR's shipped Bink cutscenes with correct video, audio, input
skip, subtitles, timing, and cleanup instead of marking every movie finished
immediately.

**Architecture:** Keep the complete Bink decoder and YUV renderer embedded in
`libandroid_port.so`. Integrate it in stages: validate its shaders, route only the
silent `legal.bik` through the real player, measure and correct rendering, then
provide the minimal OpenSL ES object graph its movie-audio adapter expects. The
OpenSL buffer queue should feed decoded movie PCM into the port's existing 48 kHz
Vita mixer rather than opening a second BGM audio port.

**Spec:** `docs/specs/2026-09-08-bink-video-playback.md`

**Progress:** Shader, silent-video, and one 44.1 kHz stereo A/V gate passed on
hardware. The isolated `OPENSL_TEST` build substitutes `01c.bik`, feeds its PCM
through the existing 48 kHz mixer, and returns to stable gameplay. Production
still defaults to `SKIP`; representative 48 kHz, long-run, skip, subtitle, and
repeated-play validation remain.

**Relevant code:**

- `loader/bink_patch.c`
- `loader/bink_patch.h`
- `loader/audio_patch.c`
- `loader/audio_patch.h`
- `loader/bink_audio_queue.c`
- `loader/bink_audio_queue.h`
- `loader/opensl_patch.c`
- `loader/opensl_patch.h`
- `loader/gl_patch.c`
- `loader/gl_patch.h`
- `loader/sdl_patch.c`
- `loader/config.h`
- `loader/so_util.c`
- `loader/so_util.h`

## Global constraints

- Do not add or commit any `.bik`, APK, OBB, VPK, extracted movie, or decoded
  frame/audio data. Tests may read the user's OBBs in place.
- Keep `MacDecompress` real. It is the LZMA game-resource decompressor, not Bink
  video playback.
- Movie skipping remains the default until the silent-video acceptance gate
  passes on real Vita hardware.
- Audio-bearing movies remain skipped until `slCreateEngine` returns a valid
  object graph and the OpenSL queue is proven safe.
- The game thread owns vitaGL. Do not move `MacPlayBinkGL` or its GL work to a
  loader-created thread.
- Keep the persistent vitaGL shader cache enabled. Delete
  `ux0:data/shader_cache/KOTR00001/` only when deliberately testing a cold shader
  path or after shader/compiler changes.
- No decoder or rendering optimization is accepted without a before/after trace.
- Never preload an entire movie or decode a complete movie soundtrack to PCM.
- All movie resources must be recoverable on failure and button skip, not only on
  natural completion.
- Build with zero new warnings. Run `git diff --check` before every commit.
- Reuse the existing buffered log. Per-frame logs must be event-driven or
  summarized, not unconditional.

## Feature modes

Use one compile-time policy in `loader/config.h` during development:

```c
#define BINK_MODE_SKIP            0
#define BINK_MODE_SHADER_TEST     1
#define BINK_MODE_LEGAL_VIDEO     2
#define BINK_MODE_AV_TRACE        3
#define BINK_MODE_FULL            4

#define BINK_MODE BINK_MODE_SKIP
```

The exact names may change during implementation, but the behavior must remain
separable:

| Mode | Real shaders | Real video | Movie audio |
|---|---|---|---|
| `SKIP` | No | No | No |
| `SHADER_TEST` | Yes | No | No |
| `LEGAL_VIDEO` | Yes | `legal.bik` only | No audio in asset |
| `AV_TRACE` | Yes | Explicit allowlist only | Null-safe tracing adapter |
| `FULL` | Yes | All movies | Existing Vita mixer |

Do not jump directly from `SKIP` to `FULL`.

## Baseline artifacts

Before changing Bink behavior, record:

- Current Git commit.
- Current VPK SHA-256.
- `libandroid_port.so` build ID and SHA-256 from the spec.
- Warm shader-cache state and file list.
- `ux0:data/kotor/log.txt` from a normal launch where intro movies are skipped.
- Heap, bigalloc, vitaGL, audio, and open-file baselines after reaching gameplay.

The current known-good commit at plan creation is `0281e2a`.

---

## Task 1: Make Bink hooks policy-driven

**Purpose:** Separate shader creation from movie playback so each can be enabled
without accidentally entering movie audio.

**Files:**

- Modify: `loader/config.h`
- Modify: `loader/bink_patch.c`
- Modify: `loader/bink_patch.h`

**Steps:**

- [x] Add the currently safe development mode constants (`SKIP` and
      `SHADER_TEST`). Later modes remain compile errors until their gates pass.
- [x] Resolve and log the real addresses of `MacCreateBinkShaders` and
      `MacPlayBinkGL` before installing hooks.
- [x] In `BINK_MODE_SKIP`, preserve today's behavior exactly: hook both functions
      and mark movies finished immediately.
- [x] In `BINK_MODE_SHADER_TEST`, leave `MacCreateBinkShaders` unhooked but keep
      `MacPlayBinkGL` stubbed.
- [x] Log one startup line with the selected mode and both resolved addresses.
- [x] Fail safely back to skip behavior if either symbol is missing.
- [x] Do not modify `MacDecompress`.

**Verification:**

- [x] Both `SKIP` and `SHADER_TEST` build cleanly and produce valid VPK archives.
- [ ] A normal `SKIP` build boots and skips the same intro queue as before.
- [x] `SHADER_TEST` reaches gameplay without entering real movie playback.
- [x] The log contains one Bink mode line and one later skipped `01A.bik` request.

**Commit:** `Make Bink playback modes explicit`

---

## Task 2: Validate the embedded Bink shader

**Purpose:** Prove the YUV shader and its GL object setup work under the current
vitaGL backend before any decoder or texture path is enabled.

**Files:**

- Modify only if needed: `loader/gl_patch.c`
- Modify only if needed: `loader/glsl_prep.c`
- Modify: `loader/bink_patch.c` for mode/result diagnostics

**Expected companion calls:**

- `glCreateShader`
- `glShaderSource`
- `glCompileShader`
- `glCreateProgram`
- `glAttachShader`
- `glLinkProgram`
- `glGetUniformLocation`
- `glGetAttribLocation`
- `glGenBuffers`
- `glBufferData`

**Steps:**

- [x] Build and run `BINK_MODE_SHADER_TEST` through normal startup.
- [x] Capture existing GL compile/link logs and verify both Bink stages report
      success.
- [x] Verify the linked program exposes the expected YUV sampler set:
      `YTex`, `cRTex`, `cBTex`, and `ATex`.
- [x] Continue running for several hundred `GameUpdate` calls and prove shader
      creation is not retried.
- [ ] Read the companion ready byte and object state directly only if a later
      shader retry or initialization failure makes that additional probe useful.
- [ ] Relaunch without deleting the cache and compare cold and warm Bink link
      time when validating the legal-video build.
- [ ] If the shader fails, fix only the demonstrated GLSL/vitaGL incompatibility.
      Do not proceed while the ready byte remains zero.

**Acceptance gate:**

- Bink shaders compile and link once.
- The cache contains new source-hash GXP entries.
- Warm launch loads the program without a multi-second link.
- No shader/program count grows while idling at the menu.

Hardware result: program 1 linked once in 770.6 ms cold, exposed all four YUV
samplers, and did not retry. `01A.bik` remained skipped.

**Commit:** `Enable the embedded Bink shaders`

---

## Task 3: Add a safe trampoline to the real player

**Purpose:** Route one selected movie to the original companion implementation
while every other movie retains the proven skip path.

The current `hook_addr` overwrites the companion entry point and cannot call the
original. `BINK_MODE_LEGAL_VIDEO` therefore needs a real trampoline.

**Files:**

- Modify: `loader/so_util.c`
- Modify: `loader/so_util.h`
- Modify: `loader/main.c`
- Modify: `loader/bink_patch.c`
- Modify: `loader/bink_patch.h`

**Steps:**

- [x] Move the already tested Thumb patch-length and trampoline construction
      helpers from `loader/main.c` into `loader/so_util.c` without changing their
      algorithm.
- [x] Update existing main-loader hook users to call the shared helpers.
- [x] Disassemble the `MacPlayBinkGL` prologue at companion offset `+0x587b4` and
      verify every copied instruction is position-independent.
- [x] Build `MacPlayBinkGL_orig` from that prologue before installing the wrapper.
- [x] Treat the original as a void function for wrapper purposes; the game ignores
      `r0`, and the real function does not establish a meaningful return value.
- [x] Preserve the softfp-compatible integer/pointer signature:

```c
static void (*MacPlayBinkGL_orig)(const char *path, int can_skip,
                                  unsigned char *finished, int arg);
```

- [x] Do not mutate the caller's path buffer. The isolated build deliberately
      substitutes a constant `legal.bik` path for the first request.
- [x] In `LEGAL_VIDEO`, call the original exactly once with silent `legal.bik`.
      Use the existing skip behavior for every later request.
- [x] If trampoline creation fails, log it and leave all movies skipped.

**Verification:**

- [x] All existing non-Bink trampolines and both Bink test modes build cleanly.
- [ ] The wrapper can skip a non-allowlisted movie without touching the original.
- [ ] The original function is entered only for `legal.bik`.
- [x] `git diff --check` and a clean Vita build pass.

**Commit:** `Allow one Bink movie through the real player`

---

## Task 4: Add bounded Bink telemetry

**Purpose:** Measure decode, upload, frame pacing, memory, and cleanup without
turning logging into the playback bottleneck.

**Files:**

- Modify: `loader/bink_patch.c`
- Modify: `loader/bink_patch.h`
- Modify: `loader/gl_patch.c`
- Modify: `loader/sdl_patch.c`
- Modify only if needed: `loader/dynlib.c`

**State to expose:**

```c
typedef struct {
  int active;
  unsigned frame_count;
  unsigned swaps;
  unsigned luminance_uploads;
  uint64_t luminance_bytes;
  uint64_t start_us;
  uint64_t frame_sum_us;
  uint64_t frame_max_us;
  char name[48];
} bink_perf_t;
```

The exact structure can differ, but snapshots must be coherent and low overhead.

**Steps:**

- [x] Log one `[BINK] begin` line with requested/substituted path, skip permission, and
      caller argument.
- [x] Record process time and loader open-file count before calling the original.
- [x] Mark Bink active only around the original call.
- [x] While active, count `GL_LUMINANCE` texture uploads and their true one-byte
      payload. Do not use the generic two-byte fallback in `fmt_bpp()` for Bink
      accounting.
- [x] Count swaps and summarize average/max presented movie-frame time.
- [x] Log one `[BINK] end` line with elapsed time, swaps, frame timing, uploads,
      bytes, and open-file delta. Natural versus skip classification still needs
      a companion-side discriminator.
- [ ] Keep per-frame details behind the existing 80 ms hitch threshold. Do not
      print one line per normal movie frame.
- [x] Flush the log at movie teardown so a post-movie fault preserves the record.

**Acceptance gate:** telemetry overhead is below one millisecond on normal movie
frames and produces at most a begin line, an end line, periodic summaries, and
event-driven hitches.

**Commit:** `Measure Bink playback without pacing it`

---

## Task 5: Play `legal.bik` without audio

**Purpose:** Prove the embedded decoder, OBB path, YUV surfaces, presentation,
input skip, and teardown on real Vita hardware.

`movies/legal.bik` is the only shipped movie with no audio stream. It is
1024x768, 29.97 fps, and approximately 4.97 seconds long.

**Test matrix:**

1. Natural completion.
2. Button skip after the one-second eligibility delay.
3. Button press before the delay, which must not skip.
4. Five repeated full plays.
5. Five repeated early skips.

**Checks:**

- [ ] Path resolves from `patch.obb` without loose extraction.
- [ ] Decoder opens at the stored entry offset and closes the extra descriptor.
- [ ] Movie remains visible and correctly oriented.
- [ ] Aspect ratio is acceptable on 960x544; record screenshots for comparison.
- [ ] No OpenSL call is made for this asset.
- [ ] Any physical joystick button can skip after the allowed delay.
- [ ] Natural completion returns to the expected game state.
- [ ] Button skip returns to the same state without a stuck black frame.
- [ ] The real player's `g_RenderSkip = 1` teardown does not conflict with
      `DISABLE_ADAPTIVE_RENDER_SKIP`.
- [ ] Heap, bigalloc, vitaGL, file handles, and thread counts return to baseline.
- [ ] Five repeats do not grow any resource count.

**Do not optimize yet.** If the movie plays correctly but slowly, preserve the
trace and continue to Task 6 before changing the renderer.

**Acceptance gate:** correct natural and skipped playback on real hardware with
no leak or crash.

**Commit:** `Play the silent Bink legal movie`

---

## Task 6: Choose embedded Bink or FFmpeg fallback

**Purpose:** Make the decoder architecture an evidence-based decision after the
silent movie has exercised the embedded path on real hardware.

**Reference:** `Rinnegatamante/AvP-Gold-Vita` commit
`cb77f1147a74a397795985e3190e18ccfc9e3722`, especially `src/bink.c`. It proves
FFmpeg Bink decode plus a four-buffer audio queue can run on Vita, but its source
license is noncommercial and not MIT-compatible by default. Treat it as a design
reference; do not copy its implementation into this repository without a
license decision.

**Embedded path stays preferred when:**

- `legal.bik` decodes and renders correctly.
- Its CPU time leaves acceptable frame cadence for ordinary 640x272 movies.
- Its planar YUV uploads are stable or can be optimized locally.
- A bounded OpenSL method trace indicates a small compatibility layer.

**Prototype FFmpeg only when the embedded path fails one of those gates.** The
prototype must be independently written against the current FFmpeg API, not
ported line-for-line from AvP's legacy API usage.

If a fallback prototype is required:

- [ ] Add it behind a separate `BINK_MODE_FFMPEG_TEST`; do not replace the
      embedded path during the experiment.
- [ ] Link only the required static libraries: `avformat`, `avcodec`, `avutil`,
      and either `swscale` or a direct planar renderer. Do not link OpenAL unless
      testing it in isolation.
- [ ] Verify the installed FFmpeg exposes Bink video and Bink DCT audio decoders
      in the final link, not merely strings in an archive.
- [ ] Use modern `avcodec_send_packet` / `avcodec_receive_frame`; AvP's
      `avcodec_decode_video2`, `avcodec_decode_audio4`, `AVPicture`, and
      `av_register_all` calls do not exist in the installed FFmpeg headers.
- [ ] Implement custom `AVIOContext` callbacks that expose only one stored OBB
      entry's byte range. A seek must never escape into adjacent archive data.
- [ ] Decode one frame at a time. Do not load the entire movie or extract movie
      files to `ux0:data`.
- [ ] Prefer direct planar YUV upload when FFmpeg returns a compatible format.
      Use RGB565 conversion only as a measured fallback.
- [ ] Feed decoded PCM into the existing Vita mixer through the same bounded
      queue planned for OpenSL; do not make OpenAL a second production audio
      owner.
- [ ] Reimplement KOTOR-specific movie queue, subtitles, skip delay,
      clear/present, and completion semantics explicitly.
- [ ] Compare embedded and FFmpeg paths on `legal.bik` and one 640x272 movie:
      executable size, heap, decode time, upload time, frame cadence, and cleanup.

**Decision record:** Add a short result section to the findings spec naming the
chosen decoder and the measurements that decided it. Do not maintain two
production decoders.

**Commit, only if a prototype is needed:** `Prototype FFmpeg Bink decoding`

---

## Task 7: Optimize the measured video path only if required

**Purpose:** Correct any demonstrated frame-pacing or allocation issue without
changing the embedded decoder unnecessarily.

Potential findings and bounded responses:

| Measured issue | Response |
|---|---|
| Shader compile dominates only cold run | Keep persistent cache; no renderer change |
| `glTexImage2D` reallocation dominates | Reuse matching plane storage and translate later uploads to `glTexSubImage2D` while Bink is active |
| Luminance telemetry overcounts | Correct Bink-specific bytes; do not change texture format |
| 1024x768 decode misses 30 fps but 640x272 does not | Accept legal-screen cadence or render every decoded frame without forcing 30 fps |
| Aspect ratio is wrong | Correct Bink scale/shift or viewport only while Bink is active |
| Swap wait dominates | Inspect presentation policy; do not add CPU-side speedhacks |
| CPU decoder dominates at all resolutions | Profile worker scheduling and NEON path before dropping frames |

If texture reuse is needed:

- [ ] Identify Bink plane texture IDs and remember width, height, format, and
      type per ID while Bink is active.
- [ ] Let the first `glTexImage2D` allocate storage.
- [ ] Replace a later identical non-null upload with `glTexSubImage2D`.
- [ ] Fall back to `glTexImage2D` whenever dimensions, format, type, or level
      changes.
- [ ] Clear Bink texture state at `Free_Bink_textures` or movie teardown.
- [ ] Compare allocation counts, movie-frame time, and memory pools before and
      after.

**Acceptance gate:** the chosen optimization improves the measured bottleneck
without corrupting color, stride, orientation, or cleanup.

**Commit:** use a finding-specific message, for example
`Reuse Bink plane textures between frames`.

---

## Task 8: Replace the unsafe OpenSL placeholders with a tracing object graph

**Purpose:** Discover and safely satisfy the exact OpenSL method sequence before
routing movie PCM to hardware.

The current binding is unsafe for movie audio:

```c
{ "slCreateEngine", (uintptr_t)&fmod_stub }
```

It returns success without writing the engine output pointer. Bink immediately
dereferences that pointer.

**Files:**

- Create: `loader/opensl_patch.c`
- Create: `loader/opensl_patch.h`
- Modify: `CMakeLists.txt`
- Modify: `loader/audio_patch.c`
- Modify: `loader/main.c` or resolver registration as required

**Architecture requirements:**

- OpenSL interfaces are pointer-to-vtable objects. Match that ABI exactly.
- Every created object has explicit type, realized state, requested interfaces,
  and deterministic destruction.
- `SL_IID_ENGINE`, `SL_IID_PLAY`, `SL_IID_VOLUME`, and
  `SL_IID_BUFFERQUEUE` must be distinct stable objects, not four zero words.
- Unknown methods or unsupported interfaces return an error, never success with
  an unwritten output pointer.
- Method logging is budgeted and includes object type, interface, method, and
  integer/pointer arguments.

**Minimum expected surface to implement or trace:**

- `slCreateEngine`
- Object `Realize`
- Object `GetInterface`
- Object `Destroy`
- Engine `CreateOutputMix`
- Engine `CreateAudioPlayer`
- Play `SetPlayState`
- Volume `SetVolumeLevel`
- Buffer queue `RegisterCallback`
- Buffer queue `Enqueue`
- Buffer queue `Clear`

**Steps:**

- [x] Move the OpenSL implementation into the dedicated layer; the resolver table
      remains in `audio_patch.c`.
- [x] Return a valid engine object and record the actual method order used by
      Bink.
- [x] Return valid output-mix and audio-player objects.
- [x] For `AV_TRACE`, accept audio buffers into a bounded queue but do not enable
      all movies by default.
- [x] Preserve each buffer pointer until consumption or clear; Bink may reuse it
      only after the queue callback.
- [x] Never call queue callbacks while holding the OpenSL or audio mixer lock.
- [x] Add object and queue high-water counters and verify all objects are
      destroyed after a movie.

The trace adapter may discard samples only for this discovery mode. It must pace
buffer completion consistently enough that Bink does not spin or run the movie
at unlimited speed.

**Acceptance gate:** an explicit short audio-bearing movie reaches natural end
without null dereference, unbounded queue growth, or leaked OpenSL objects. Audio
is not yet required to be audible.

Hardware result: passed with the custom existing-mixer adapter. The direct
VitaSDK OpenSL experiment was silent and faulted in `_free_r`; it is rejected.

**Commit:** `Implement a null-safe OpenSL trace adapter for Bink`

---

## Task 9: Feed Bink PCM into the existing Vita mixer

**Purpose:** Make movie audio audible without opening a second BGM port.

**Files:**

- Modify: `loader/opensl_patch.c`
- Modify: `loader/opensl_patch.h`
- Modify: `loader/audio_patch.c`
- Modify: `loader/audio_patch.h`

**Movie-source model:**

Add one mixer source owned by the OpenSL audio player:

```c
typedef struct {
  const int16_t *data;
  unsigned frames;
  unsigned consumed;
  unsigned channels;
  unsigned rate;
  void *owner;
} MovieAudioBuffer;
```

Use a small bounded FIFO of buffer descriptors. Do not copy large queued buffers
unless hardware evidence proves the Bink buffers cannot remain valid until the
callback.

**Steps:**

- [x] Parse the PCM format passed to `CreateAudioPlayer`: channel count, sample
      rate, sample format, channel mask, and byte order.
- [x] Accept 44.1 kHz and 48 kHz stereo signed 16-bit PCM. Reject unsupported
      formats with a clear log.
- [x] Mix the active buffer in `mix_grain()` outside the FMOD channel model.
- [x] Resample 44.1 kHz to the existing 48 kHz output with the same linear
      interpolation policy used by ordinary sounds.
- [x] Apply OpenSL millibel volume to a linear gain before the global limiter.
- [x] Honor stopped, paused, and playing states.
- [x] When a buffer is fully consumed, retire it and invoke the registered queue
      callback outside every lock.
- [x] `Clear` retires queued buffers without callbacks unless the observed Bink
      contract requires otherwise.
- [x] Movie-player destruction stops the source and removes all descriptors
      before returning.
- [x] Keep ordinary FMOD music/effects behavior stable. Record whether the game
      calls `FMOD_System_MixerSuspend` around movies; implement explicit suspend
      semantics only if the trace shows it is needed.

Hardware finding: Bink's async worker serviced its audio pump once before the
decoded-frame sequence became nonzero and never woke again. The isolated mode
captures the valid handle and calls the original locked pump once per sequence
advance from the movie swap path. On `01c.bik`, 499 forced pumps produced 2,086
`Lock`/`Unlock` pairs and 2,937,056 decoded bytes.

**Telemetry:**

- Queues/enqueues/completions.
- Maximum queued buffers and bytes.
- Mixer grains containing movie audio.
- 44.1-to-48 kHz resampling count.
- Starved movie-audio grains.
- Callback latency.
- Natural-end and skip teardown counts.

**Acceptance gate:** movie audio is audible through the existing Vita output,
does not create another BGM port, and does not regress game audio before or after
the movie.

Hardware result: passed for natural `01c.bik` playback. Audio was audible, both
players tore down, and gameplay remained stable. Button skip, 48 kHz, long-run,
and repeated-play gates remain in Task 10.

**Commit:** `Mix Bink audio through the existing Vita output`

---

## Task 10: Validate representative audio/video movies

Use an allowlist before enabling the entire library.

**Required assets:**

| Movie | Purpose |
|---|---|
| `legal.bik` | Silent 1024x768 video |
| `01c.bik` | Short 640x272, 44.1 kHz stereo |
| `aspyr.bik` | 1024x576, 48 kHz stereo |
| `01a.bik` | 120-second 640x272 long-run and subtitle test |

**For each movie:**

- [ ] Test natural completion.
- [ ] Test button skip after one second.
- [ ] Verify no skip before one second.
- [ ] Record video frames, slow frames, max frame time, and average cadence.
- [ ] Record OpenSL queue depth, audio starvation, and callback latency.
- [ ] Verify A/V sync at start, middle, and end.
- [ ] Verify the last audio buffer does not continue after video end or skip.
- [ ] Verify game audio resumes afterward.
- [ ] Verify heap, bigalloc, vitaGL pools, handles, and threads return to baseline.

**Long-run criterion:** `01a.bik` completes without accumulating timing drift,
queue growth, decoder errors, or resource loss.

**Commit:** `Validate Bink audio and video across shipped formats`

---

## Task 11: Subtitles and language behavior

**Purpose:** Validate the companion's existing subtitle implementation rather
than replacing it.

**Steps:**

- [ ] Trace `getCurrentLanguage` and confirm value zero means the intended
      English/no-sidecar behavior.
- [ ] Test one movie with each available localized sidecar suffix in a controlled
      language build: `de`, `es`, `fr`, and `it`.
- [ ] Verify subtitle path lookup in main OBB, patch OBB, and loose-file fallback.
- [ ] Verify subtitle text uses the existing font and wrap path without crashing.
- [ ] Verify subtitle timing remains correct at normal cadence and after a slow
      decoded frame.
- [ ] Verify button skip tears down subtitle resources with the movie.

Do not add translated subtitle data to the repository.

**Commit:** `Validate Bink subtitles and language selection`

---

## Task 12: Enable normal movie playback

**Purpose:** Replace development allowlists with production behavior only after
all previous gates pass.

**Steps:**

- [ ] Make full playback the default Bink mode.
- [ ] Keep `BINK_MODE_SKIP` as a compile-time recovery option.
- [ ] Consider a runtime sentinel such as `ux0:data/kotor/skip_movies` only if a
      field recovery mechanism is needed; do not add one speculatively.
- [ ] Remove temporary high-volume OpenSL method logging while retaining summary
      and failure diagnostics.
- [ ] Keep unsupported-format and allocation failures explicit and safe.
- [ ] On a movie-specific failure, skip that movie and continue the queue rather
      than wedging startup or gameplay.
- [ ] Update `README.md`, `DEVELOPMENT.md`, and release notes.
- [ ] Change the status in the findings spec from implementation proposed to
      hardware validated, with measured results.

**Final acceptance:**

- Intro sequence plays from a clean launch.
- Story movies play during a normal multi-area session.
- Natural end and physical-button skip work.
- 44.1 and 48 kHz audio work through the existing Vita mixer.
- Long movies maintain A/V sync.
- No movie-related crash, shader retry, descriptor leak, texture leak, audio
  queue leak, thread leak, or post-movie sound loss occurs.

**Commit:** `Enable Bink cutscene playback`

---

## Hardware comparison template

Record this table for each test build:

| Metric | Skip baseline | Silent video | Tracing A/V | Full A/V |
|---|---:|---:|---:|---:|
| Startup to menu | | | | |
| Bink shader cold link | | | | |
| Bink shader warm link | | | | |
| Average movie frame time | | | | |
| Maximum movie frame time | | | | |
| Frames over 50 ms | | | | |
| Frames over 80 ms | | | | |
| YUV upload MB/s | | | | |
| Maximum texture reallocations/frame | | | | |
| Movie audio queue high-water | | | | |
| Movie audio starved grains | | | | |
| A/V drift at end | | | | |
| Open descriptors before/after | | | | |
| vitaGL VRAM before/after | | | | |
| vitaGL RAM before/after | | | | |
| newlib used before/after | | | | |
| bigalloc live before/after | | | | |

## Rollback rules

- Shader failure: return to `BINK_MODE_SKIP`; do not attempt video.
- Silent movie crash: keep real shaders if stable, but restore movie skipping.
- Audio-bearing movie crash: retain video-only allowlist and restore OpenSL
  placeholders only while movie calls remain blocked.
- Texture corruption: disable any upload-reuse optimization before questioning
  the decoder.
- Audio regression outside movies: remove the movie source from the shared mixer
  and return to the last video-only commit.
- Resource growth after repeated playback: do not enable the next mode until the
  exact object or buffer lifetime is fixed.

Every rollback should preserve the log and the smallest build that reproduces
the fault.
