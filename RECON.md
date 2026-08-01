# KOTOR (Android → PS Vita) — so-loader Reconnaissance

Reverse-engineering recon of the Aspyr Android build of **Star Wars: Knights of
the Old Republic** (`KOTOR v1.0.10`) to plan a PS Vita port using the
so-loader technique (à la `TheOfficialFloW/gtasa_vita`).

- **Date:** 2026-07-18
- **Target ABI:** `armeabi-v7a` (ARM32) — matches the Vita's Cortex-A9.
- **Toolchain used:** `arm-vita-eabi-readelf` (VitaSDK, gcc 15.2.0)
- **References cloned:** `~/vitadev/gtasa_vita` (TheOfficialFloW),
  `~/vitadev/sotn-vita` (Rinnegatamante, SOTN Android port — recent SDL2/GLES
  so-loader, updated 2026-07-11).
- **Raw dumps:** see [`recon/`](./recon/) (one file per symbol set).

---

## 1. The libraries (from `apk/lib/armeabi-v7a/`)

| Library | Role | Disposition on Vita |
|---------|------|---------------------|
| **libKOTOR.so** (6.0 MB) | Main game logic (the target) | **so-load** |
| **libandroid_port.so** (899 KB) | Aspyr's SDL2/Win32/GL/Bink glue; exports `android_port_gl*`, Win32 shims, `FModAudioSystem`, `ObbFile`, Bink | **so-load** (resolves 266 of KOTOR's imports) |
| **libfmod.so** (948 KB) | FMOD Studio audio engine (ARM) | **so-load** + provide its backend (⚠ see Audio) |
| **libfreetype.so** (456 KB) | Font rasterizer | so-load, or link VitaSDK freetype |
| **libminiz.so** (63 KB) | zip reader (`mz_zip_*`) | so-load, or link VitaSDK zlib/minizip |
| **libLzmaLib.so** (22 KB) | LZMA (`LzmaUncompress`) | so-load |
| **libSDL2.so** (1.0 MB) | SDL2 | **replace** with VitaSDK SDL2 (native) |
| libhidapi.so, libstub.so | HID / Play-services stub | ignore |

### `libKOTOR.so` dynamic `NEEDED` (see `recon/libKOTOR.dynamic.txt`)

```
libandroid_port.so  libfmod.so  libdl.so  liblog.so  libfreetype.so
libSDL2.so  libGLESv1_CM.so  libandroid.so  libLzmaLib.so  libminiz.so
libGLESv2.so  libOpenSLES.so  libm.so  libc.so
```

**Key architectural facts (verified, not assumed):**

- **Graphics:** needs **both `libGLESv1_CM` (fixed-function ES 1.x) and
  `libGLESv2`.** vitaGL supports both — this is why vitaGL is the right backend.
- **No EGL.** Zero `egl*` imports and no `libEGL` in `NEEDED`, in either
  `libKOTOR.so` or `libandroid_port.so`. **SDL2 owns context creation** — there
  is no EGL layer to port.
- **Audio is FMOD**, not OpenAL. `FModAudioSystem` (in `libandroid_port.so`)
  wraps the FMOD C++ API from `libfmod.so`. Output goes through **OpenSLES**
  (`libandroid_port.so` imports `slCreateEngine` + `SL_IID_*`).
- **Bink video** (`MacPlayBinkGL`, `MacCreateBinkShaders`, `MacDecompress`) is
  **implemented inside `libandroid_port.so`** — the companion decodes and draws
  it; we only owe it GL + file I/O.

---

## 2. Symbol accounting for `libKOTOR.so`

`arm-vita-eabi-readelf --dyn-syms` → **487 undefined symbols** (441 FUNC, 46
OBJECT). Full list: [`recon/libKOTOR.undefined.txt`](./recon/libKOTOR.undefined.txt).

| Bucket | Count | Meaning |
|--------|------:|---------|
| **(a) Covered by the template `default_dynlib.c`** | **192** | Present in the gtasa/sotn resolution tables — libc, libm, libc++, `pthread_*`, common SDL2, stdio/string. Copy the entries. → [`recon/A_covered_by_template.txt`](./recon/A_covered_by_template.txt) |
| **Resolved by so-loading `libandroid_port.so`** | **266** | `android_port_gl*` (105), Win32 shims (`GetTickCount`, `CreateThread`, `FindFirstFileA`…), `FModAudioSystem::*`, `ObbFile::*`, Bink, touch/gamepad globals. Loading the companion `.so` satisfies these automatically. → [`recon/resolved_by_libandroid_port.txt`](./recon/resolved_by_libandroid_port.txt) |
| **Residual — direct attention** | **29** | Neither in refs nor companion. → [`recon/libKOTOR.residual.txt`](./recon/libKOTOR.residual.txt) |

The 29 residuals are almost all trivial:

- **(a) add to table** (libc/C++ ABI): `fmaxf`, `fminf`, `isgraph`, `putchar`,
  `__cxa_*`, `__gxx_personality_v0`, `_ZSt9terminatev`, `_ZTIi`,
  `_ZTVN10__cxxabiv1*` (RTTI vtables), `_ZSt*` — provided by the toolchain's
  libc/libc++, just needs the mapping.
- **(b) stub:** `getpriority`, `setpriority`, `syscall`, `system`, `dladdr`,
  `pthread_cond_timedwait_monotonic_np` (→ real `pthread_cond_timedwait`),
  `SDL_IsChromebook`, `SDL_IsScreenKeyboardShown`, `Android_Window` (data global).
- **(c) small impl:** `glMapBufferOES`, `glUnmapBufferOES` (buffer mapping —
  wrap vitaGL or emulate), `mz_zip_reader_locate_file` (from `libminiz.so`).

> **So the main `.so` is ~99% covered by "load the companion + copy the reference
> tables."** The real engineering is in `libandroid_port.so`'s own imports.

---

## 3. The real implementation surface — `libandroid_port.so` imports

`libandroid_port.so` has **403 undefined symbols**
([`recon/libandroid_port.undefined.txt`](./recon/libandroid_port.undefined.txt)).
After subtracting the reference tables (232) and vitaGL (127 of 141 GL calls),
the surface **we** must build is **127 distinct symbols**
([`recon/TOTAL_to_implement.txt`](./recon/TOTAL_to_implement.txt)),
categorized below into the three requested buckets.

### (a) Already covered by the template / SDK — just wire up

| Group | Notes |
|-------|-------|
| **C++ ABI / RTTI / exceptions** (~30) | `__cxa_throw`, `__cxa_begin_catch`, `__dynamic_cast`, `__gxx_personality_v0`, `_ZNSt*`, `_ZTV*`, `_ZTI*`, `_ZSt17__throw_bad_allocv` — link the toolchain's libc++/libsupc++. |
| **libc / newlib** | `div`, `isblank`, `mbrlen`, `rename`, `rmdir`, `strtoimax`, `strtoumax`, `sched_yield` — newlib provides. |
| **SDL2** | `SDL_AllocRW`, `SDL_DestroyCond`, `SDL_FlushEvents`, `SDL_GetThreadID`, `SDL_GetHint`, `SDL_JoystickGetAttached` — VitaSDK SDL2 has these. |
| **FreeType** (`FT_*`, 18) | Provided by so-loading `libfreetype.so` or linking VitaSDK freetype. |
| **Asset manager** (`AAsset_open/close/read/seek/getRemainingLength`, `AAssetManager_open`) | **Both gtasa and sotn implement these** in their loader — adapt their impl to read from the OBB (see §4). |
| **JNI** (`Android_JNI_GetEnv`) | Reference loaders provide a minimal fake `JNIEnv`. |

### (b) Needs a stub (no-op / trivial return)

| Symbol(s) | Stub strategy |
|-----------|---------------|
| `__android_log_print`, `__android_log_write` | → `sceClibPrintf` / `vprintf`. |
| `getpriority`, `setpriority`, `syscall`, `system` | Vita has no process model — return 0 / `-ENOSYS`. |
| `dladdr` | Return 0 (no dynamic-symbol introspection needed). |
| `pthread_cond_timedwait_monotonic_np` | Forward to `pthread_cond_timedwait`. |
| `SDL_IsChromebook`, `SDL_IsScreenKeyboardShown` | Return `SDL_FALSE`. |
| **14 GL calls not in vitaGL** → [`recon/gl_gaps_not_in_vitaGL.txt`](./recon/gl_gaps_not_in_vitaGL.txt): `glBlendColor`, `glSampleCoverage`, `glDetachShader`, `glIsBuffer`, `glIsShader`, `glValidateProgram`, `glCompressedTexSubImage2D`, `glGetShaderPrecisionFormat`, `glGetRenderbufferParameteriv`, `glGetTexParameterfv/iv`, `glGetUniformfv/iv`, `glTexParameterfv` | Mostly getters/state — thin wrappers over vitaGL internals, or safe no-op stubs. `glCompressedTexSubImage2D` / `glDetachShader` may need real forwarding; verify against a newer vitaGL first (ours: `38d2f97`). |
| `mz_zip_*`, `LzmaUncompress` | Satisfied by so-loading `libminiz.so` / `libLzmaLib.so`; otherwise stub over VitaSDK zlib/lzma. |

### (c) Needs real implementation

| Subsystem | Symbols | Plan / risk |
|-----------|---------|-------------|
| **OpenGL backend** | 127 `gl*`/`android_port_gl*` via vitaGL + the 14 gaps above | **Low risk.** vitaGL covers 127/141 directly. Init a vitaGL context under SDL2, map `android_port_gl*` (companion already does this against raw `gl*`). |
| **Audio (FMOD)** | `FMOD_System_Create`, `FMOD::System/Channel/Sound/ChannelControl::*` (~34) **+** `slCreateEngine`, `SL_IID_ENGINE/PLAY/VOLUME/BUFFERQUEUE` | **HIGH RISK — the hardest part.** Two paths: **(1)** so-load `libfmod.so` (resolves the 34 FMOD symbols) and implement a **minimal OpenSLES engine** (the 5 `sl*`/`SL_IID_*` symbols) backed by `sceAudioOut` — gtasa does exactly this. **(2)** If FMOD's ARM output won't drive our fake OpenSL, reimplement `FModAudioSystem` (30 methods, all in the companion — would require *not* loading the companion's audio path) over `sceAudio`. Start with path (1). |
| **Asset manager over OBB** | `AAsset*` (+ `ObbFile::*` inside companion) | **Medium.** The game data lives in `main.53.*.obb` / `patch.53.*.obb` (both are ZIPs, already present in `com.aspyr.swkotor/`). Point the reference `AAssetManager` impl at a mounted OBB path. |
| **JNI shim** | `Android_JNI_GetEnv` (+ any `JNI_OnLoad` expectations) | **Low.** Fake `JNIEnv` with the handful of methods the game calls (mostly for cloud-save / analytics, which can be no-op'd). |
| **Bink video** | `MacPlayBinkGL`, `MacCreateBinkShaders`, `MacDecompress` | **Low direct cost** — implemented *inside* `libandroid_port.so`; it needs only GL + file I/O from us. Validate the intro movies render; fall back to skipping cutscenes if the codec misbehaves. |
| **Input** | touch/gamepad globals (`current_touch_x/y`, `ios_gamepad_analog*`, `gamepadConnected`) | **Low.** These are data globals the companion reads; feed them from `sceCtrl`/`sceTouch` via SDL2. |

---

## 4. Prioritized porting plan

**Phase 0 — Skeleton (bring-up).**
1. Fork a loader template (`gtasa_vita` structure: `loader/main.c`, `so_util.c`).
2. so-load `libKOTOR.so` + `libandroid_port.so`; wire `so_resolve` with a
   `default_dynlib[]` seeded from `recon/A_covered_by_template.txt`.
3. Stub *everything* in `recon/TOTAL_to_implement.txt` (return 0) just to reach
   `JNI_OnLoad` / `main` without an unresolved-symbol abort.
   **Goal: it links and starts executing.**

**Phase 1 — Video out.**
4. SDL2 window + vitaGL context; map `android_port_gl*` → vitaGL.
5. Implement the 14 GL gaps (stubs first).
   **Goal: menu / first frame renders.**

**Phase 2 — Assets.**
6. Mount the OBBs; port the reference `AAsset*` manager to read from them.
   **Goal: game loads its own data (fonts, models, dialog).**

**Phase 3 — Audio (budget the most time here).**
7. so-load `libfmod.so`; implement the minimal OpenSLES engine over `sceAudioOut`.
   **Goal: music + SFX. Fallback: NOSOUND output to keep progressing.**

**Phase 4 — Input & polish.**
8. Map `sceCtrl`/`sceTouch` → the companion's input globals; on-screen touch for
   the mobile UI.
9. Bink cutscenes; JNI cloud-save/analytics no-ops; save-path redirection to
   `ux0:data/kotor/`.

**Risk register:** Audio (FMOD/OpenSL) ≫ Bink codec > OBB asset paths > GL gaps.
Memory is also a concern — the Vita gives ~340 MB (+ extra via
`sceKernelAllocMemBlock`); KOTOR's Android heap will need careful mapping.

---

## 5. Reproduce this analysis

```bash
export VITASDK=$HOME/vitasdk && export PATH=$VITASDK/bin:$PATH
SO=apk/lib/armeabi-v7a/libKOTOR.so
arm-vita-eabi-readelf -d "$SO"                 # dynamic section + NEEDED
arm-vita-eabi-readelf --dyn-syms -W "$SO" \
  | awk '$7=="UND"&&$8!=""{print $8}' | sed 's/@.*//' | sort -u   # undefined symbols
```

All derived lists are in [`recon/`](./recon/).
