# Bink video playback: embedded decoder findings and integration design

Status: embedded video and one 44.1 kHz stereo A/V movie passed isolated hardware
validation on 2026-09-09. A minimal OpenSL-compatible queue now feeds the existing
48 kHz Vita mixer. Production still skips every movie pending representative
movie, skip, subtitle, and repeated-play validation.

## Executive summary

KOTOR's Android companion already contains the complete Bink playback stack. We
do not need to port or implement the proprietary video decoder. The following
pieces are compiled into `libandroid_port.so`:

- Bink video and Bink DCT audio decoding.
- File access to loose files and stored entries inside the mounted OBBs.
- Worker threads and playback timing.
- CPU frame buffers for Y, Cr, Cb, and optional alpha planes.
- OpenGL ES shaders that convert those planes to RGB.
- Subtitle loading and drawing.
- An OpenSL ES adapter for movie audio.

The loader currently hooks the two entry points that activate this stack and
replaces them with no-ops. `MacPlayBinkGL` marks each movie finished immediately,
and `MacCreateBinkShaders` returns without creating the YUV shader. That was a
safe bring-up decision, not a decoder limitation.

Video-only playback and one audio/video playback gate now pass. The production
default remains conservative because sixty-one of the sixty-two shipped movies
contain audio and the broader movie/skip/subtitle matrix has not run yet.

The recommended sequence is:

1. Enable and validate the real Bink shaders while movie playback remains
   skipped.
2. Play the one silent movie, `movies/legal.bik`, through the real decoder.
3. Measure decode cost, YUV upload cost, completion, input skip, and cleanup.
4. Implement the small OpenSL ES subset Bink uses and route movie PCM into the
   existing Vita audio mixer. Completed for the isolated mode.
5. Validate representative formats, long playback, skip, subtitles, and repeated
   teardown before enabling ordinary audio-bearing movies.

## Current behavior

`loader/bink_patch.c` currently installs these hooks:

```c
hook_addr(so_symbol(port_mod, "_Z13MacPlayBinkGLPKcbRbi"),
          (uintptr_t)&MacPlayBinkGL_stub);
hook_addr(so_symbol(port_mod, "_Z20MacCreateBinkShadersv"),
          (uintptr_t)&bink_stub);
```

The movie stub logs the path, writes `1` to the supplied `finished` byte, and
returns. The game therefore advances through its movie queue without decoding,
drawing, or playing audio.

`MacDecompress` must remain untouched. Despite the shared `Mac` prefix, it is the
LZMA resource decompressor used by `.bzf` game data. Stubbing it previously broke
models and other compressed resources.

## Binary identity

These findings apply to the ARMv7 libraries extracted from the tested Android
package:

| Binary | Size | Build ID | SHA-256 |
|---|---:|---|---|
| `libandroid_port.so` | 899,284 bytes | `f5266c0b212120bb1e959dd20b3bf86980ce0d39` | `a91f9235e4d0bd8bb43450d83b9df61d6339dd09c664278b9ac1666b22afe2da` |
| `libKOTOR.so` | 6,044,464 bytes | `991195884bf4a23fef67e980fa0ec3266ae42241` | `099c43db23a891a19f40f5805c3582b390de39d96d6e50be52649a40498a68b5` |

Both libraries are ARMv7 EABI5, Thumb-2, soft-float ABI, VFPv3, and NEON-enabled.
The loader is hardfp and already bridges the float-by-value GL calls used by the
companion.

There is no external Bink shared-library dependency. The decoder and its error
strings are in `libandroid_port.so`. Its `DT_NEEDED` list includes OpenSL ES,
SDL2, GLES, FMOD, miniz, LZMA, libc, and libm, but no `libbink.so`.

## Relevant companion symbols

Offsets below are relative to the load base of `libandroid_port.so`. Function
symbols are Thumb entry points; disassembly offsets are shown with bit zero
cleared.

| Symbol | Offset | Size |
|---|---:|---:|
| `MacCreateBinkShaders()` | `+0x58268` | `0x24` |
| `MacPlayBinkGL(char const*, bool, bool&, int)` | `+0x587b4` | `0x3c8` |
| `Create_Bink_shaders` | `+0x60fcc` | `0x290` |
| `Create_Bink_textures` | `+0x6125c` | `0x256` |
| `Free_Bink_textures` | `+0x614b2` | `0xa0` |
| `Draw_Bink_textures` | `+0x61554` | `0x2a0` |
| `BinkSubtitles::BinkSubtitles` | `+0x58318` | `0x244` |
| `BinkSubtitles::Draw(float)` | `+0x5857c` | `0x238` |

Embedded diagnostics include `Not a Bink file.`, `Error reading Bink header.`,
`Bink Snd`, `Bink IO`, `BinkAsy0`, and `BinkGPU not supported.`. The last string
matches the observed CPU-decode plus planar-texture path.

## Shader initialization

`MacCreateBinkShaders` is guarded by a one-byte ready flag at companion BSS
offset `+0xdc120`:

1. Return immediately when the ready byte is nonzero.
2. Call `Create_Bink_shaders` otherwise.
3. Set the ready byte only when shader creation succeeds.

The embedded vertex shader starts around `+0xc6891`; the fragment shader starts
around `+0xc6984`. The fragment shader samples four luminance textures:

```glsl
uniform sampler2D YTex;
uniform sampler2D cRTex;
uniform sampler2D cBTex;
uniform sampler2D ATex;
```

It converts Y, Cr, and Cb to RGB and uses `ATex` for alpha. The shader uses GL
operations that are already backed by `loader/gl_patch.c`:

- `glCreateShader`
- `glShaderSource`
- `glCompileShader`
- `glGetShaderiv`
- `glGetShaderInfoLog`
- `glCreateProgram`
- `glAttachShader`
- `glLinkProgram`
- `glGetProgramiv`
- `glGetUniformLocation`
- `glGetAttribLocation`
- `glEnableVertexAttribArray`
- `glVertexAttribPointer`
- `glUniform4fv`
- `glUniform1i`
- `glGenBuffers`
- `glBindBuffer`
- `glBufferData`
- `glActiveTexture`
- `glUseProgram`

None of the loader's unsupported GLES gap stubs is required by this path.

Persistent shader state occupies companion BSS around `+0x13df98` through
`+0x13dfc8`, including the program, VBO, IBO, attribute locations, sampler
locations, and shader IDs.

If shader creation fails, the ready byte stays zero. `MacCreateBinkShaders` is
called from the SDL startup path and can be called again from `GameUpdate`.
Repeated failure can therefore compile repeatedly and leak partial objects. The
first milestone must prove one successful initialization before playback is
enabled.

The project's required vitaGL application shader cache will hash and persist the
final Bink shader source once it has linked successfully.

## Frame texture and draw path

The embedded decoder produces up to four CPU planes:

- Y luminance.
- Cr chroma.
- Cb chroma.
- Optional alpha.

`Create_Bink_textures` allocates a CPU buffer and an OpenGL texture for each
active plane. The textures use:

```text
GL_TEXTURE_2D
GL_LINEAR
GL_CLAMP_TO_EDGE
GL_LUMINANCE
GL_UNSIGNED_BYTE
```

vitaGL supports the required L8 format.

`Draw_Bink_textures` binds the Bink shader and its VBO/IBO, uploads scale and
shift uniforms, uploads each active plane with `glTexImage2D`, and draws six
indexed triangle vertices. The position layout is four floats at offset zero;
the UV layout is two floats at offset sixteen with a 24-byte stride.

The current path calls `glTexImage2D` for every plane on every movie frame. In
vitaGL, replacing an existing valid texture can free and reallocate the mapped
GPU buffer. A normal three-plane 30 fps movie may therefore perform about ninety
texture reallocations per second. This is a performance and allocator-churn risk,
not an API blocker. If hardware traces show it matters, the first rendering
optimization should allocate each plane once and use `glTexSubImage2D` for
subsequent frames.

Approximate planar upload rates, excluding alpha, are:

| Dimensions | Bytes per frame | At 30 fps |
|---|---:|---:|
| 640x272 | 261,120 | 7.8 MB/s |
| 1024x576 | 884,736 | 26.5 MB/s |
| 1024x768 | 1,179,648 | 35.4 MB/s |

These rates are much smaller than uploading full RGBA frames, but they still need
hardware measurement. The loader's current texture telemetry reports unknown
formats as two bytes per pixel and will overcount the one-byte luminance planes;
that accounting should be corrected before using the trace to judge Bink memory
traffic.

## File and OBB access

`MacPlayBinkGL` normalizes backslashes to slashes and first calls
`SDL_RWFromFile(path, "rb")`. The loader already translates the path and retries
against the mounted OBBs.

When the returned RWops belongs to an `ObbFile::Entry`, the companion obtains the
real OBB path and the entry's data offset. The embedded decoder then uses its own
POSIX `open`, `read`, `lseek`, and `close` adapter against that real archive. The
loader supplies all those imports.

Every shipped `.bik` member uses ZIP method `store`; compressed size equals
uncompressed size. Bink data can therefore be streamed and sought directly from
the OBB without inflating a ZIP member first.

The decoder opens one additional real descriptor for the archive while a movie
is active. Repeated playback tests must verify that it is closed on natural end,
button skip, decoder failure, and texture-setup failure.

## Movie inventory

The tested version 53 OBBs contain 62 unique Bink movies:

| Archive | Movie count | Payload |
|---|---:|---:|
| `main.obb` | 12 | 171.12 MiB |
| `patch.obb` | 50 | 218.56 MiB |
| Total | 62 | 389.68 MiB |

Total duration is approximately 27 minutes 19 seconds. Individual files range
from about 3.2 seconds to 120.3 seconds and from 0.42 MiB to 25.67 MiB.

All files identify as Bink Video revision `i` (`BIKi`). The profiles are:

| Profile | Count | Frame rate | Audio |
|---|---:|---:|---|
| 640x272 | 53 | 29.97 fps | Usually 44.1 kHz stereo |
| 640x360 | 5 | 29.97 fps | 44.1 kHz stereo, one at 48 kHz |
| 640x480 | 2 | 29.97 fps | 44.1 kHz stereo |
| 1024x576 | 1 | 30 fps | 48 kHz stereo |
| 1024x768 | 1 | 29.97 fps | No audio |

Sixty-one movies contain Bink DCT audio. `movies/legal.bik` is the only silent
movie, which makes it the correct first end-to-end video test.

The first likely real intro movie is `movies/leclogo.bik`. The intro queue also
contains `biologo`, `Aspyr_BlueDust_intro`, `aspyr`, and `legal`; the BlueDust
identifier does not exactly match a file and may be an alias.

Some main-archive movies have localized subtitle sidecars named like:

```text
movies/01a.bik.desub.txt
movies/01a.bik.essub.txt
movies/01a.bik.frsub.txt
movies/01a.bik.itsub.txt
```

The subtitle constructor checks the OBBs and loose files. Subtitle rendering uses
the game's wrapped text path rather than the Bink YUV shader.

## Playback call and ABI

The exported C++ signature is:

```text
MacPlayBinkGL(char const *path, bool can_skip, bool &finished, int arg)
```

The game initializes the referenced byte to zero, passes its address in `r2`, and
stores it into the movie-player object after the call.

The real function does not write that reference and its apparent return value is
not used. The current stub writes `1` deliberately to implement skip behavior; it
is not emulating the real completion contract.

On successful natural completion or permitted user skip, the real function:

1. Closes the Bink decoder.
2. Frees frame textures and CPU plane buffers.
3. Writes `1` to companion global `g_RenderSkip`.
4. Clears color and depth.
5. Presents one final frame.

The loader currently clears positive `g_RenderSkip` values before a primary
`GameUpdate` to prevent adaptive no-present update bursts. Movie completion must
be tested with that override enabled.

Failure paths return without changing the referenced `finished` byte. Shader
failure also leaves the shader-ready byte zero, permitting later retries.

## Timing, threading, and skip input

The observed `PlayMoviesAsync` path is synchronous despite its name. It walks the
movie queue and calls `StartMovie` on the game thread. That is the same thread
that initializes and owns the vitaGL context, avoiding the port's known
cross-thread GXM deadlock.

The embedded decoder creates worker threads through pthreads. The loader provides
the required pthread mutex, condition, create, join, and clock calls. Its worker
priority and affinity requests degrade to no-ops because `getpriority`,
`setpriority`, and `syscall` are stubs. That is acceptable for initial playback
but must be measured under load.

Movie playback flushes SDL events at start. User skip accepts either
`SDL_FINGERDOWN` or any `SDL_JOYBUTTONDOWN`, but only when the caller permits
skipping and after about one second of frames. Gameplay touch is disabled, so
physical buttons are the available skip path. The controller translation hook
preserves the joystick event type, so no new input API is required.

The Bink code requests Android clock ID 1 as a monotonic source. Vita identifies
that value as realtime, while its monotonic clock is ID 4. Bink condition waits
and the provided pthread implementation both use the same realtime basis, so
deadlines remain internally consistent unless the wall clock changes during a
movie. This is a low-priority correctness risk.

## The OpenSL audio blocker

The embedded Bink sound adapter is independent of the project's FMOD replacement.
Movie PCM does not currently flow through `Sys_createSound` or the existing
streaming ring.

On the first audio track, the Bink adapter calls `slCreateEngine`, reads the
output engine pointer, and immediately calls through that object without checking
the result.

Production `SKIP` builds retain the inert bindings below because no movie enters
the audio path:

```c
{ "slCreateEngine",     (uintptr_t)&fmod_stub },
{ "SL_IID_ENGINE",      (uintptr_t)&sl_iid_engine },
{ "SL_IID_PLAY",        (uintptr_t)&sl_iid_play },
{ "SL_IID_VOLUME",      (uintptr_t)&sl_iid_volume },
{ "SL_IID_BUFFERQUEUE", (uintptr_t)&sl_iid_bufferqueue },
```

`fmod_stub` returns success but does not write the output pointer. The four IID
objects contain zero. An audio-bearing Bink movie therefore reaches a predictable
null dereference.

Simply removing the Bink hooks in production is still not safe until the remaining
acceptance matrix passes.

VitaSDK has a full OpenSL implementation, but hardware testing rejected it:

- It opens a BGM audio port of its own.
- The FMOD replacement already owns a BGM port.
- A second port may fail or compete for output policy.
- It defaults to 44.1 kHz unless configured through its weak frequency symbol.
- It introduces a second mixer and lifecycle beside the existing 48 kHz mixer.
- The isolated `01c.bik` run rendered normally but remained silent, then faulted
  in `_free_r` during movie teardown with invalid address `0xfee606b7`.

The implemented architecture is a minimal OpenSL-compatible object graph that
accepts Bink's decoded PCM queue and feeds it into the existing Vita audio mixer.
The observed surface is limited to:

- Engine object creation, realization, interface lookup, and destruction.
- Output-mix object creation.
- Audio-player creation.
- Play interface state.
- Buffer-queue enqueue, callback registration, and teardown. `Clear` and
  `GetState` are implemented defensively but were not called by this movie.
- Volume level and stereo-position control.
- Stable, distinct IID objects.

The queue holds at most 38 caller-owned PCM descriptors. It invokes completion
callbacks only after consuming each buffer and outside its state lock, allowing
Bink to re-enter `Enqueue`. 44.1 kHz input is linearly resampled into the existing
48 kHz output before the global limiter.

### Bink audio-pump wake failure

The first custom-mixer hardware run was stable but silent. Both players were
created and set playing, but all 3,125 enqueues came from the 24 kHz helper; the
44.1 kHz movie player enqueued zero buffers.

Bounded hooks resolved the BINKSND callbacks and service path:

| Role | Companion offset |
|---|---:|
| Audio pump | `+0x85670` |
| `Ready` | `+0x82168` |
| `Lock` | `+0x821bc` |
| `Unlock` | `+0x82430` |
| OpenSL state initializer | `+0x824b0` |
| Queue submitter | `+0x82c5c` |

The audio worker invoked the pump once immediately after open. At that point the
Bink handle's decoded-frame sequence at `+0x570` was zero, so the pump returned
before calling `Ready`. The worker never serviced the handle again. The sequence
is incremented in the frame-advance path at `+0x8562e`.

The isolated fix captures the handle on the worker's first pump call and invokes
the original, internally locked pump from the movie swap path once per sequence
advance. The handle is cleared before the main audio player is destroyed. This
does not modify decoder state or force the sequence value.

Hardware result on `01c.bik`:

- Audible audio and video completed, then gameplay remained stable.
- 16,755 ms, 500 swaps, 33/64 ms average/max frame interval.
- 499 forced pumps for `gateMax=499` after the one initial worker pump.
- 2,086 `Lock`/`Unlock` pairs committed 2,937,056 decoded bytes.
- The 44.1 kHz stereo player enqueued and completed 1,016 blocks, with peak
  sample 32,767 and 798,929 mixed output frames.
- Movie-audio underrun was 1,839 output frames, about 38 ms total.
- Both players were destroyed, active-player count returned to zero, queue errors
  were zero, file handles remained `2 -> 2`, and no crash occurred.
- Successful pump-test VPK SHA-256:
  `fb64e7df8d38a1d1e7331d0fc67afd876885a97581a1be9f9ae876e81a3679aa`.
- Successful hardware log SHA-256:
  `16195ed26ecb700475c26cdd58b67d1c42191fe45bd2e74caa9a8c59de038842`.

## Memory and performance constraints

Streaming from the stored OBB entries is favorable. Average compressed movie
read bandwidth is only around 0.25 MB/s; per-frame YUV upload and CPU decode are
more important.

No implementation may preload all movie bytes or decode complete audio tracks.
The longest movie is about 25.7 MiB compressed, and fully decoded stereo audio
alone can exceed 20 MiB.

Approximate full-frame allocations are:

| Resolution | RGBA frame | Three RGBA buffers |
|---|---:|---:|
| 640x272 | 0.66 MiB | 1.99 MiB |
| 1024x576 | 2.25 MiB | 6.75 MiB |
| 1024x768 | 3.00 MiB | 9.00 MiB |

The embedded path is planar luminance rather than RGBA and should use less
texture memory. All movie textures and CPU planes must still be released
immediately after playback. Repeated short-movie playback is required to detect
leaks and fragmentation.

## Rejected approaches

### Remove both stubs immediately

Rejected because 61 of 62 movies enter the broken OpenSL binding and can null
dereference before playback completes.

### Port or replace the Bink decoder

Rejected because the complete decoder is already embedded, and replacing it
would add licensing, compatibility, and performance work without evidence that
the shipped implementation is unusable.

### Extract all movies to the data directory

Rejected because OBB entries are already stored and seekable. Extraction would
duplicate about 390 MiB and would not solve decoding, rendering, or audio.

### Link VitaSDK OpenSL directly as the first audio solution

Rejected by hardware testing. It created a second BGM-port owner, produced no
audible movie audio, and faulted in `_free_r` during teardown. The custom adapter
uses the existing mixer and completed cleanly.

### Optimize texture upload before first playback

Rejected as premature. The existing planar `glTexImage2D` path should first be
measured on hardware. Convert it to persistent allocation plus
`glTexSubImage2D` only when traces show allocator or upload cost is material.

## Acceptance criteria

Video-only support is acceptable when:

- Bink shaders compile once, link successfully, and do not retry.
- `legal.bik` resolves from the OBB and plays for its expected duration.
- Frames are scaled correctly to 960x544 without corruption.
- A physical button skips only after the allowed delay.
- Natural completion and skip both return cleanly to gameplay.
- Decoder, texture, CPU-plane, and file-handle resources return to baseline.
- Five repeated plays do not grow vitaGL pools or newlib usage.
- No OpenSL call is entered for the silent movie.

Full audio/video support is acceptable when:

- Both 44.1 kHz and 48 kHz stereo movie tracks play.
- Audio uses the existing Vita output path without competing BGM ports.
- Buffer-queue callbacks and teardown match the embedded adapter's expectations.
- Audio and video remain synchronized over the 120-second `01a.bik` test.
- Natural end and button skip stop audio without stale queued buffers.
- Subtitle timing and language selection are correct where sidecars exist.
- Repeated playback does not leak threads, descriptors, textures, queue buffers,
  or OpenSL-compatible objects.

## Open questions

- Does the Bink shader compile unchanged under the current vitaGL GLSL prepass?
- Can the Vita sustain the embedded CPU decoder at 29.97 fps for 1024x768?
- How much time is spent in decode versus per-plane `glTexImage2D`?
- Does the shader's scale/shift produce the correct aspect ratio on 960x544?
- Does movie completion's `g_RenderSkip = 1` interact with the loader override?
- Does the frame-sequence pump workaround remain correct during button skip and
  120-second playback?
- Do 48 kHz movie tracks bypass resampling and retain synchronization?
- Does subtitle language zero select the intended English/no-sidecar behavior?
- Are worker priority and clock-ID differences visible on long movies?

The implementation tasks and validation gates are in
`docs/plans/2026-09-08-bink-video-playback.md`.

## External reference: AvP-Gold-Vita

[`Rinnegatamante/AvP-Gold-Vita`](https://github.com/Rinnegatamante/AvP-Gold-Vita)
is a useful independent proof that Bink playback
can be implemented on Vita without RAD's proprietary SDK. The inspected source
is repository commit `cb77f1147a74a397795985e3190e18ccfc9e3722`, especially
`src/bink.c` and `src/bink.h`.

Its architecture is different from KOTOR's embedded player:

- FFmpeg `libavformat` opens and demuxes the `.bik` file.
- FFmpeg `libavcodec` decodes Bink video and Bink audio.
- `libswscale` converts decoded video to RGB565 for normal playback or RGB24 for
  the game's small in-world FMV textures.
- The game uploads/draws that converted packed image through its own renderer.
- OpenAL owns a source and four streaming buffers (`FRAMEQUEUESIZE = 4`) for
  decoded movie audio.
- `SDL_GetTicks` gates video decode against the stream frame rate, while audio
  queue availability also influences when more packets are decoded.

The current VitaSDK container already has static `libavcodec`, `libavformat`,
`libavutil`, `libswscale`, `libswresample`, and `libopenal` archives. They total
about 4.2 MiB before the linker discards unused objects. The installed FFmpeg is
libavcodec 62.28.102 and contains modern send/receive decoding entry points. The
AvP source uses removed legacy APIs such as `avcodec_decode_video2`,
`avcodec_decode_audio4`, stream-owned `AVCodecContext`, `AVPicture`, and
`av_register_all`, so it is a design reference rather than code that can be
dropped into this project unchanged.

### What should be reused conceptually

- A bounded four-buffer audio queue is a proven, small streaming model for movie
  PCM on Vita.
- Reclaim processed buffers before decoding more audio.
- Keep decoded movie state in one explicit lifetime object and release audio,
  scaling, codec, and demux state together.
- Gate packet decode by video cadence and available audio queue capacity.
- Convert FFmpeg sample formats to a single signed 16-bit mixer format at the
  boundary.
- Keep separate entry points for one-shot FMVs, looping backgrounds, and
  audio-only Bink streams when the host game needs them.

These principles are applicable to the OpenSL-compatible adapter planned for
KOTOR even if FFmpeg is never linked.

### Why it is not the primary KOTOR path

KOTOR's companion already knows its own movie call contract, OBB offsets,
subtitle sidecars, skip timing, final clear/present behavior, and `g_RenderSkip`
handoff. Replacing it with FFmpeg would require reimplementing all of that glue.

Additional costs of the AvP architecture are:

- KOTOR movies are byte ranges inside stored OBB entries, not ordinary loose
  files. FFmpeg would need custom `AVIOContext` callbacks or an equivalent
  bounded-range input layer; extracting about 390 MiB of movies is not an
  acceptable requirement.
- AvP converts every frame through `libswscale` to RGB565 and then uploads a
  packed frame. KOTOR's embedded player already emits planar YUV, avoiding the
  CPU color conversion and cutting upload bandwidth roughly in half versus
  RGBA.
- OpenAL would create another audio-device owner beside the existing FMOD
  replacement. For KOTOR, decoded movie PCM should enter the existing mixer;
  AvP's four-buffer queue is useful, but its OpenAL output is not the preferred
  integration.
- Static FFmpeg materially increases dependency and binary surface even when the
  linker strips unused codecs.
- `AvP-Gold-Vita` carries the Rebellion source license, which prohibits
  commercial use and requires visible credit. VitaKotor's loader source is MIT.
  Do not copy `src/bink.c` or other AvP code into this repository without a
  deliberate license decision. Ideas and independently written integration code
  are the safer route.

### Fallback decision gate

FFmpeg becomes the preferred fallback only if one of these is demonstrated:

1. The embedded decoder cannot decode KOTOR's files correctly on Vita.
2. Its decoder CPU cost misses acceptable cadence while FFmpeg's Vita build is
   measurably better on the same asset.
3. The embedded OpenSL adapter requires a substantially larger or less reliable
   compatibility surface than a direct FFmpeg-to-existing-mixer path.
4. The embedded renderer has an unfixable vitaGL incompatibility.

The silent `legal.bik` milestone must run first. It provides the evidence needed
for this choice without committing to either movie-audio architecture.

## Implementation progress

The first policy gate is implemented:

- Default `BINK_MODE_SKIP` hooks both companion exports and preserves existing
  movie-skip behavior.
- `KOTOR_BINK_SHADER_TEST=ON` compiles with `BINK_MODE_SHADER_TEST`, leaves the
  real `MacCreateBinkShaders` untouched, and still hooks every
  `MacPlayBinkGL` call to the skip stub.
- Missing companion exports force all available Bink hooks back to skip mode.
- Binary disassembly confirms the shader-test normal path calls `hook_addr` only
  for `MacPlayBinkGL`; the shader hook remains only in the missing-export safety
  branch.

Hardware validation passed:

- The mode logged `SHADER_TEST` and resolved the expected companion exports at
  `0x900587b5` and `0x90058269`.
- Bink program 1 compiled and linked once in 770.6 ms on the cold path.
- The linked program exposed `YTex`, `cRTex`, `cBTex`, and `ATex` as sampler
  uniforms.
- Silent `legal.bik` completed in 5,027 ms with 150 swaps, 33/52 ms average/max
  frame interval, 149 decoded Y/Cr/Cb frames, no handle leak, and continued
  gameplay.
- `01c.bik` completed with audible 44.1 kHz stereo audio through the existing
  mixer using the frame-sequence pump fix. The detailed measurements are in
  "Bink audio-pump wake failure" above.
- No Bink compile or link failure was recorded and shader creation did not retry.
- The requested `01A.bik` still logged through the movie-skip hook, proving the
  OpenSL path remained unreachable in this test.

The next package permits one real call, substituting silent `legal.bik` for the
first requested movie, and skips every later movie. Its hardware procedure is
tracked in the implementation plan.
