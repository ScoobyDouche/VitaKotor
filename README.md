# VitaKotor

**Star Wars: Knights of the Old Republic on the PlayStation Vita.**

A native Vita port of the *Android* release of KOTOR, built with the so-loader
technique: rather than reimplementing the game, the loader loads Aspyr's own ARM
libraries straight from your copy of the Android build, resolves everything they
import against Vita equivalents, and hands control to the game's real entry
point. Graphics run through [vitaGL](https://github.com/Rinnegatamante/vitaGL),
with a from-scratch FMOD audio implementation over `sceAudiodec` and
`sceAudioOut` underneath.

> **You must own the Android version of KOTOR.** Nothing here contains game
> code or data — only the loader, plus artwork used for the LiveArea. It does
> nothing on its own. You supply the game.

---

## Status

**Work in progress — not yet good enough to play through.**

It boots, gets through character creation, and plays past the Endar Spire onto
Taris. Combat, dialogue, inventory, containers and saves all work, and it looks
and sounds like the game. But **it crashes when moving between areas after
roughly 10–20 minutes of play**, every time, and that is a hard blocker on
actually finishing anything. See [Known issues](#known-issues).

Treat it as something to look at, not something to start a playthrough on.

| | |
|---|---|
| Startup | ~2 min on first launch, then under a minute |
| Frame rate | ~30–38 fps typical, dips into the low 20s in dense scenes |
| Session length | **~10–20 min before an area transition crashes it** |
| Audio | Effects and voice work; long music tracks are silent |
| Cutscenes | Not played — the video codec is stubbed out |
| Input | Physical controls, plus front touchscreen for menus |
| Saves | Work, stored on the Vita |

`main` is usually ahead of the newest [release](../../releases); the issue list
below says which fixes have not been released yet.

---

## Requirements

**On the Vita**

- A Vita or PS TV on firmware that can run [HENkaku](https://henkaku.xyz/) / taiHEN
- [`kubridge.skprx`](https://github.com/bythos14/kubridge/releases) installed as a taiHEN plugin
- `libshacccg.suprx` in `ur0:data/` — the runtime shader compiler. Use
  [this installer](https://github.com/Rinnegatamante/vita-shacccg-installer) if
  you don't have it. **Without it the game hangs on a black screen** at the
  first shader compile, with no error message.
- Roughly **2.7 GB** free on `ux0:`

**From your own copy of the game**

The Android release: the `.apk` and both `.obb` expansion files. An `.apk` is
just a zip — open it with any archive tool to get at the libraries inside.

---

## Installation

1. **Install `KOTOR.vpk`** — from [Releases](../../releases) — with VitaShell.

2. **Create `ux0:data/kotor/`** on your Vita.

3. **Copy four libraries** out of your APK, from `lib/armeabi-v7a/`:

   ```
   libKOTOR.so
   libandroid_port.so
   libminiz.so
   libLzmaLib.so
   ```

4. **Copy both expansion files**, renaming them exactly:

   ```
   main.<version>.com.aspyr.swkotor.obb   ->  main.obb    (~2.1 GB)
   patch.<version>.com.aspyr.swkotor.obb  ->  patch.obb   (~453 MB)
   ```

5. **Launch it.** The **first** boot takes around two minutes: it reads through
   the 2.1 GB archive and records what it needed into `main.obb.idx` beside it.
   Every boot after that reuses the recording and reaches the game in under a
   minute. A progress bar is on screen throughout, so you can tell it is
   working rather than hung.

   Delete the `.idx` files if you ever replace an `.obb`; they are rebuilt
   automatically, and a mismatched one is ignored rather than trusted.

You should end up with:

```
ux0:data/kotor/
├── libKOTOR.so
├── libandroid_port.so
├── libminiz.so
├── libLzmaLib.so
├── main.obb
└── patch.obb
```

The loader adds `main.obb.idx`, `patch.obb.idx`, `startup.tim` and `log.txt`
here on first run.

Shaders and font metrics ship inside the VPK, so there is nothing else to copy.

---

## Known issues

### Blocking

- **Crashes when changing area, after 10–20 minutes of play.** The heap
  fragments until a routine 1 MB allocation cannot find contiguous room —
  there is plenty of memory free, just none of it in one piece — and the game
  aborts. It looks like a freeze on the console because the crash handler stops
  the app where it stands; `log.txt` will say `UNDEFINED_INSTRUCTION` and, on
  builds newer than v0.1.9, print the heap state that caused it. **This is the
  thing being worked on.** Save often; the crash costs you everything since the
  last save.

### Rough edges

- **Stutter in dense scenes.** Busy areas issue around 700 draw calls a frame
  and the frame rate drops into the low 20s. A graphics-throughput limit.
- **Long music tracks are silent.** Anything over about 90 seconds is replaced
  with correctly-timed silence, so pacing stays right but the score does not
  play. Shorter music and combat stings do. Voice and effects are unaffected.
- **Cutscenes are skipped.** The Bink video decoder is stubbed out, so FMVs are
  passed over rather than played.
- **Rear touch panel is disabled** deliberately — it sits under your fingers
  while holding the console and fired spurious taps.
- **No trophies.**

### Fixed in `main`, not yet in a release

- **Sound thinning out and then disappearing entirely, voices included**, over a
  long session — and taking the game down with it. The engine was never told
  when a sound finished, so it never reused a voice or closed a music stream;
  the leak eventually exhausted both file handles and memory. If you are on
  v0.1.9 or earlier this is the audio behaviour you will see.

## Troubleshooting

Everything is logged to **`ux0:data/kotor/log.txt`**, including full CPU fault
reports. That file is the first thing to check and the first thing to attach to
a bug report.

| Symptom | Likely cause |
|---|---|
| Black screen, no error | `libshacccg.suprx` missing from `ur0:data/` |
| Crashes immediately at launch | `kubridge.skprx` not installed |
| Hangs at the loading spinner | An `.obb` is missing, misnamed, or still copying |
| Freezes on an area transition | The known memory bug above — not your install |
| Textures look wrong | Set `GL_FILTER_REDUNDANT_BINDS 0` in `loader/config.h` and rebuild |

A "freeze" is usually not a freeze: the crash handler parks the app in place so
the log survives. Check the end of `log.txt` for a `[CRASH]` block before
assuming it hung.

---

## Building from source

Needs [VitaSDK](https://vitasdk.org/) with vitaGL, vitashark, SceShaccCgExt,
mathneon, FreeType and SDL2 installed. `SETUP.md` covers the toolchain.

```bash
export VITASDK=$HOME/vitasdk
export PATH=$VITASDK/bin:$PATH

cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake"
cmake --build build -j"$(nproc)"
# -> build/KOTOR.vpk
```

The build globs GLSL shaders and font metrics out of `apk/assets/`, so extract
your own APK to `apk/` in the repo root first. That directory is gitignored and
must never be committed.

The LiveArea assets in `sce_sys/` are generated from the APK's own artwork by
`tools/mklivearea.sh` (needs ImageMagick). They are checked in, so you only need
to re-run it if you want to change how they look.

Tunables live in `loader/config.h` — MSAA mode, the redundant texture-bind
filter, memory layout. `RECON.md` documents the binary analysis the port is
built on, and `DEVELOPMENT.md` covers the architecture and the debugging
workflow.

---

## Credits

- **Andy Nguyen (TheFloW)** — the so-loader technique this is built on
- **Rinnegatamante** — vitaGL, vitashark, and the reference ports
- **VitaSDK contributors** — the toolchain that makes any of this possible
- **Aspyr Media, BioWare, Lucasfilm** — the game itself

Released under the MIT licence — see [LICENSE](LICENSE). It covers the loader
only, never the game.
