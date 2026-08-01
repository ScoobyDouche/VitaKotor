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
> code, data or assets, and it does nothing on its own. You supply the game.

---

## Status

**Playable.** It boots, gets through character creation, and plays past the
Endar Spire onto the first planet. Combat, dialogue, inventory, containers and
saves all work.

It is not finished — read [Known issues](#known-issues) before starting a long
save.

| | |
|---|---|
| Frame rate | ~30–38 fps typical, dips in dense scenes |
| Audio | Sound effects and some music; parts of the music and a few voice lines are missing |
| Input | Physical controls, plus front touchscreen for menus |
| Saves | Work, stored on the Vita |

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

5. **Launch it.** First boot is slow — it is mounting a 2.1 GB archive.

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

Shaders and font metrics ship inside the VPK, so there is nothing else to copy.

---

## Known issues

- **Stutter in dense scenes.** Busy areas issue around 700 draw calls a frame
  and the frame rate drops. This is a graphics-throughput limit and is the main
  thing being worked on.
- **Music is partly missing.** Long tracks are replaced with correctly-timed
  silence so dialogue and cutscene pacing stay right. Shorter music and combat
  stings do play.
- **A few voice lines don't play.**
- **Rear touch panel is disabled** deliberately — it sits under your fingers
  while holding the console and fired spurious taps.
- **No trophies.**

## Troubleshooting

Everything is logged to **`ux0:data/kotor/log.txt`**, including full CPU fault
reports. That file is the first thing to check and the first thing to attach to
a bug report.

| Symptom | Likely cause |
|---|---|
| Black screen, no error | `libshacccg.suprx` missing from `ur0:data/` |
| Crashes immediately at launch | `kubridge.skprx` not installed |
| Hangs at the loading spinner | An `.obb` is missing, misnamed, or still copying |
| Textures look wrong | Set `GL_FILTER_REDUNDANT_BINDS 0` in `loader/config.h` and rebuild |

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
