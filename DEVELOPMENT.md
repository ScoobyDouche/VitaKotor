# VitaKotor — development notes

PS Vita port of the Android build of **Star Wars: KOTOR** (Aspyr) via the
so-loader technique. See [`RECON.md`](./RECON.md) for the binary analysis and
[`SETUP.md`](./SETUP.md) for the toolchain install.

## Golden rule

> **NEVER commit, publish, upload, or otherwise distribute any file from
> `./apk/` or `com.aspyr.swkotor/` (the `.obb` game data).**
> These are the user's copyrighted, legally-owned game binaries and assets.
> They are inputs for local analysis only. `.gitignore` enforces this — do not
> remove those entries, do not `git add -f` them, and do not paste their raw
> bytes/large hex dumps into commits, issues, or PRs. Extracted **symbol lists**
> in `recon/` are fine to commit; **game binaries and assets are not.**

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
**3** pairs, named by the stamp baked into the ELF so a backup always matches
its `log.txt` header:

```
backups/KOTOR-Jul-31-2026-18-36-11.vpk    # reinstall this to revert on the Vita
backups/src-Jul-31-2026-18-36-11.tar.gz   # loader/ + CMakeLists.txt as built
```

The `.vpk` reverts the device; the tarball is what lets development resume from
that point (`tar xzf backups/src-<stamp>.tar.gz` at the repo root). Override the
count with `KEEP_BUILDS=n`. Note `touch loader/log.c` before building or the
BUILD stamp — and therefore the snapshot name — goes stale.

Inspecting the Android binaries (recon):

```bash
arm-vita-eabi-readelf -d       apk/lib/armeabi-v7a/libKOTOR.so   # dynamic/NEEDED
arm-vita-eabi-readelf --dyn-syms -W apk/lib/armeabi-v7a/libKOTOR.so
```

## Project rules

- **Target ABI is `armeabi-v7a`** (ARM32). Ignore `arm64-v8a`.
- **Graphics:** vitaGL (backs both GLES1 fixed-function and GLES2). No EGL layer
  — SDL2 owns context creation. Rebuild/reinstall vitaGL with
  `cd ~/vitadev/vitaGL && make -j && make install` when bumping it.
- **so-load, don't rewrite:** load `libKOTOR.so` **and** `libandroid_port.so`
  together — the companion resolves 266 of KOTOR's imports (GL wrappers, Win32
  shims, `FModAudioSystem`, Bink). Only implement `libandroid_port.so`'s own
  imports (see `recon/TOTAL_to_implement.txt`).
- **Symbol resolution:** seed `default_dynlib[]` from
  `recon/A_covered_by_template.txt`; adapt asset-manager/JNI impls from the
  reference loaders rather than writing new ones.
- **Audio is the hard part** (FMOD → OpenSLES over `sceAudio`). Keep a
  `NOSOUND` fallback so bring-up isn't blocked on it.
- **Saves / writable data:** redirect to `ux0:data/kotor/`. Game data (OBB) is
  read-only input.
- Keep `recon/` in sync if you re-run the readelf analysis (regen instructions
  at the bottom of `RECON.md`).
- Before claiming a phase works, **build the `.vpk` and verify** — don't assert
  success from a clean compile alone.

## Status (graphics bring-up — runs on hardware)

The loader builds cleanly to `build/KOTOR.vpk` (0 warnings) and **runs the game's
`SDL_main` on hardware**. It so-loads `libKOTOR.so` + `libandroid_port.so`,
relocates/resolves them, stubs audio (FMOD/OpenSLES) and Bink, wires libc/libm/
C++/pthread, inits vitaGL, builds the fake JNI tables (no `JNI_OnLoad`; entry is
`SDL_main`), redirects the filesystem, and hands off to the game. Init reaches
the SDL/JNI display-metrics probe and window/GL-context creation.

- **Runtime prerequisites:** copy `libKOTOR.so`, `libandroid_port.so`,
  `libminiz.so`, and `libLzmaLib.so` from `apk/lib/armeabi-v7a/` to
  `ux0:data/kotor/`; **copy all of `apk/assets/*` (GLSL shaders + `iosdialog.otf`
  font) to `ux0:data/kotor/`** — the game opens these by filename via
  `SDL_RWFromFile`, and without them shaders load empty and `glLinkProgram`
  crashes; copy the OBB archives to `ux0:data/kotor/main.obb`
  (`main.53….obb`, ~2.1 GB) and `ux0:data/kotor/patch.obb` (`patch.53….obb`,
  ~453 MB); `libshacccg.suprx` in `ur0:/data/`; `kubridge.skprx` installed. All
  JNI calls, GL wiring, OBB mount, and CPU faults log to
  `ux0:data/kotor/log.txt`.
- **OpenGL — WIRED** (`loader/gl_patch.c`): the companion's 141 real `gl*`
  imports resolve to vitaGL. 113 direct, 14 gap-stubs (symbols vitaGL lacks —
  see `recon/gl_gaps_not_in_vitaGL.txt`), and 14 **float-by-value shims** (next).
- **⚠ Float ABI:** this SDK is **hardfp** ("VFP registers"); Android armeabi-v7a
  is **softfp** (floats in core regs). Handled *per-function* for GL by shims in
  `gl_patch.c` (declare params `uint32_t`, bit-reinterpret to `float`, call
  hardfp vitaGL). Pointer variants (`*fv`, `Matrix*fv`) need no shim. The same
  mismatch still affects **non-GL** float args across the boundary (e.g.
  `CallStaticFloatMethod`); a full softfp rebuild of vitaGL + deps remains the
  eventual clean fix. Watch for it wherever a float is passed by value.

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
and crashes in `_sbrk_r` before `main`. Keep the heap (192 MB) ≤ the granted
partition.

## Layout

```
apk/                  extracted APK  (GIT-IGNORED — never commit)
com.aspyr.swkotor/    OBB game data  (GIT-IGNORED — never commit)
recon/                readelf-derived symbol lists (safe to commit)
loader/               so-loader source (main.c, so_util, dynlib, jni/audio/bink, log)
build/                CMake build output (GIT-IGNORED) -> build/KOTOR.vpk
sce_sys/              VPK icon + LiveArea assets
RECON.md  SETUP.md  DEVELOPMENT.md
```
