# VitaKotor — development notes

PS Vita port of the Android build of **Star Wars: KOTOR** (Aspyr) via the
so-loader technique. See [`SETUP.md`](./SETUP.md) for the toolchain install.

## Golden rule

> **NEVER commit, publish, upload, or otherwise distribute any file from
> `./apk/` or `com.aspyr.swkotor/` (the `.obb` game data).**
> These are the user's copyrighted, legally-owned game binaries and assets.
> They are inputs for local analysis only. `.gitignore` enforces this — do not
> remove those entries, do not `git add -f` them, and do not paste their raw
> bytes/large hex dumps into commits, issues, or PRs.

## Environment

```bash
export VITASDK=$HOME/vitasdk
export PATH=$VITASDK/bin:$PATH   # already in ~/.bashrc
```

Cross toolchain: `arm-vita-eabi-gcc` 15.2.0. Reference ports live in
`~/vitadev/{gtasa_vita,sotn-vita}`; vitaGL source in `~/vitadev/vitaGL`.

## Build command

The loader is a standard VitaSDK CMake project. Sources live in `loader/`, but
the `CMakeLists.txt` is at the repo root, so configure with `-S .` (not
`-S loader`). Build to a `.vpk`:

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake"
cmake --build build -j"$(nproc)"
# → build/KOTOR.vpk
```

**Rollback safety net.** Every build auto-snapshots to `backups/` via
`tools/snapshot.sh` (POST_BUILD, never fails the build). It keeps the newest
**4** pairs — the build you'd ship plus three to fall back to — named by the
stamp baked into the ELF so a backup always matches its `log.txt` header:

```
backups/KOTOR-Jul-31-2026-18-36-11.vpk    # reinstall this to revert on the Vita
backups/src-Jul-31-2026-18-36-11.tar.gz   # loader/ + CMakeLists.txt as built
```

The `.vpk` reverts the device; the tarball is what lets development resume from
that point (`tar xzf backups/src-<stamp>.tar.gz` at the repo root). Override the
count with `KEEP_BUILDS=n`. Note `touch loader/log.c` before building or the
BUILD stamp — and therefore the snapshot name — goes stale.

`build/KOTOR.vpk` is the release artifact: it is the file to attach to a GitHub
release, and the newest `backups/KOTOR-<stamp>.vpk` is a byte-identical copy of
it. There is no separate hand-maintained release copy to keep in sync.

Releases also carry a quiet build, attached as `KOTOR-nolog.vpk`. It writes no
`log.txt` (a CPU fault still dumps its crash report there) and leaves out the
`[prof]` timers. It is an extra download for players who want it, never a
replacement: the logged `KOTOR.vpk` stays the main asset, because its log is
how bug reports get fixed. Build it from the same tree, in its own directory:

```bash
cmake -S . -B build-nolog -DCMAKE_BUILD_TYPE=Release -DKOTOR_NO_LOG=ON   # once
touch loader/log.c && make -C build-nolog KOTOR.vpk-vpk -j8
cp build-nolog/KOTOR.vpk KOTOR-nolog.vpk                                   # upload this name
```

It is not snapshotted to `backups/`.

Inspecting the Android binaries:

```bash
arm-vita-eabi-readelf -d       apk/lib/armeabi-v7a/libKOTOR.so   # dynamic/NEEDED
arm-vita-eabi-readelf --dyn-syms -W apk/lib/armeabi-v7a/libKOTOR.so
```

## Project rules

- **Target ABI is `armeabi-v7a`** (ARM32). Ignore `arm64-v8a`.
- **Graphics:** vitaGL (backs both GLES1 fixed-function and GLES2). No EGL layer
  — the loader initialises vitaGL on the game thread. vitaGL is linked
  statically and carries a patch; rebuild it exactly as SETUP.md section 4 says.
- **so-load, don't rewrite:** load `libKOTOR.so` **and** `libandroid_port.so`
  together — the companion resolves most of KOTOR's imports (GL wrappers, Win32
  shims, `FModAudioSystem`, Bink). The loader only implements what is left.
- **Saves / writable data:** redirect to `ux0:data/kotor/`. Game data (OBB) is
  read-only input.
- Before claiming something works, **build the `.vpk` and verify on hardware** —
  don't assert success from a clean compile alone.

## How it runs

The loader so-loads `libLzmaLib.so`, `libminiz.so`, `libandroid_port.so` and
`libKOTOR.so`, implements FMOD and the Bink OpenSL path over one Vita mixer,
wires libc/libm/C++/pthread, builds the fake JNI tables (there is no
`JNI_OnLoad`; the entry point is `SDL_main`), mounts the OBBs, redirects the
filesystem, and runs `SDL_main` on its own thread with vitaGL initialised there.

- **Runtime prerequisites:** the four `.so` files from `apk/lib/armeabi-v7a/`
  and the OBBs (`main.obb`, `patch.obb`) in `ux0:data/kotor/`;
  `libshacccg.suprx` in `ur0:/data/`; `kubridge.skprx` installed. The GLSL
  shaders and fonts from `apk/assets/` ship inside the VPK (`app0:shaders/`,
  `app0:fonts/`), so a build made without `apk/assets/` present is broken.
  Everything logs to `ux0:data/kotor/log.txt`.
- **OpenGL** (`loader/gl_patch.c`): the companion's `gl*` imports resolve to
  vitaGL, with stubs for the few vitaGL lacks and float-by-value shims (next).
- **⚠ Float ABI:** this SDK is **hardfp** ("VFP registers"); Android armeabi-v7a
  is **softfp** (floats in core regs). Every float passed by value across the
  boundary needs a shim: declare the params `uint32_t`, bit-reinterpret to
  `float`, call the hardfp function. GL has them in `gl_patch.c`, libm in
  `dynlib.c`. Pointer variants (`*fv`, `Matrix*fv`) need none.

## Debugging crashes

**In-log CPU-fault handler (preferred):** `loader/crash.c` registers a kubridge
exception handler (armed right after both `.so`s load). Any data/prefetch-abort
or undefined-instruction fault writes PC/LR/registers to `log.txt`, with PC/LR
already resolved to `libKOTOR.so + 0xNNN` / `libandroid_port.so + 0xNNN`. Route
that offset straight to `addr2line` against the matching `.so` (game modules) or
`build/KOTOR` (`0x81000000 + off`, our loader). No coredump needed for faults
that occur once the handler is armed. Faults **before** it arms (early CRT, e.g.
`_sbrk_r`) still leave only a coredump.

**Coredump fallback (pre-handler faults):** a fault produces
`ux0:data/psp2core-*.psp2dmp` — a gzipped ELF core dump. Symbolize it against the
unstripped `build/KOTOR` ELF:

```bash
gunzip -c psp2core-*.psp2dmp > core.elf
# parse MODULE_INFO/THREAD_REG_INFO notes -> get crash PC/LR + our module base,
# then: arm-vita-eabi-addr2line -f -e build/KOTOR -C 0x81000000+<segment_offset>
```
Our module's ELF text vaddr is `0x81000000`; the coredump reports segment-relative
offsets, so `addr2line` wants `0x81000000 + offset`. `LR` is usually the call site
in our code. (Note: the upstream `xyzz/vita-parse-core` is Python 2 and expects
an old pyelftools, so it needs porting before it will run — a small Python 3
rewrite of the MODULE_INFO/THREAD_REG_INFO note parsing is enough.)

**Memory gotcha (already bitten once):** the app needs `ATTRIBUTE2=12` in the SFO
(extended-memory partition) or a large `_newlib_heap_size_user` fails at CRT init
and crashes in `_sbrk_r` before `main`. Keep `MEMORY_NEWLIB_MB` in config.h
within the granted partition.

## Input mapping

Vita SDL2 delivers the physical pad as a joystick. The indices below were
established empirically by pressing each button in a known order and reading
the log:

| Index | Button | Index | Button |
|---|---|---|---|
| 0 | Triangle | 6 | D-pad down |
| 1 | Circle | 7 | D-pad left |
| 2 | Cross | 8 | D-pad up |
| 3 | Square | 9 | D-pad right |
| 4 | L | 10 | Select |
| 5 | R | 11 | Start |

This is not Android SDL's normalized ordering. KOTOR's first static constructor
populates `gamepadButtonById` with Android indices before `SDL_main`: A/B/X/Y are
0..3, L1/R1 are 9/10, and the D-pad is 11..14. The `SDL_PollEvent` and
`SDL_PeepEvents` hooks translate Vita button events into that namespace. Start
maps to the game's own `BACKBUTTON_AS_GAMEPAD_PAUSE_ID` value, 255. Sticks
already use the expected axis order and are passed through unchanged.

Touch and the gamepad run side by side. `input_patch.c` reads the front panel
with `sceTouchPeek` each frame and pushes `SDL_FINGERDOWN`/`MOTION`/`UP` events,
because Vita SDL2 produces no finger events of its own here. The rear panel is
stopped after SDL creates the window, since SDL re-enables it and it sits under
the player's fingers. While the Vita IME is up, the panel belongs to it and no
finger events are forwarded.

**Text entry — the Vita on-screen keyboard.** Editable fields ask the platform
for a keyboard via `ASLPlat_ShowVirtualKeyboard`, which on Android is a tail
call to `SDL_StartTextInput`. `ime_patch.c` overrides that import, drives
`sceImeDialog`, and replays the result as `SDL_TEXTINPUT` events — the same path
Android uses, so every field works rather than one screen. See the file header
for why an inert `SDL_StartTextInput` left the character-name screen with no way
forward.

## Layout

```
apk/                  extracted APK  (GIT-IGNORED — never commit)
com.aspyr.swkotor/    OBB game data  (GIT-IGNORED — never commit)
loader/               so-loader source (main.c, so_util, dynlib, jni/audio/bink, log)
tools/                build helpers and host-side unit tests (test_*.c)
patches/              the vitaGL patch the shipped binary depends on
docs/releases/        release notes, one file per version
build/                CMake build output (GIT-IGNORED) -> build/KOTOR.vpk
sce_sys/              VPK icon + LiveArea assets
SETUP.md  DEVELOPMENT.md
```
