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

**Work in progress — playable in stretches, not yet a playthrough.**

It boots, gets through character creation, and plays past the Endar Spire onto
Taris. Combat, dialogue, inventory, containers and saves all work, and it looks
and sounds like the game.

The area-transition crash that used to end every session at 10–20 minutes is
**fixed** in this release: the longest test ran 44 minutes across several area
loads with the heap still healthy at the end. What replaces it is milder but
still real — after roughly 40 minutes the world geometry starts tearing into
diagonal streaks and gets worse from there. Relaunching and loading your save
came back clean. See [Known issues](#known-issues).

So: worth playing now, in sessions, saving often. Not yet something to start a
serious playthrough on.

| | |
|---|---|
| Startup | ~2 min on first launch, then under a minute |
| Frame rate | ~30–38 fps typical, dips into the low 20s in dense scenes |
| Session length | ~40 min before the picture degrades; no crash in 44 min of testing |
| Audio | Effects and voice work; long music tracks are silent |
| Cutscenes | Not played — the video codec is stubbed out |
| Input | Touchscreen, plus some physical buttons — see [Controls](#controls) |
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

## Controls

**The touchscreen is the main way you play.** KOTOR's mobile release was built
for touch, and that has not changed here: you tap the screen to pick menu
entries, move, attack, talk and use things. The front panel maps one-to-one onto
the screen, so tap what you can see.

The **rear touch panel is switched off on purpose** — it sits under your fingers
while you hold the console and was firing taps into the game.

The **physical buttons work for some actions**, and the game draws the button it
wants on screen when it wants one. Which button that is changes with what you
are doing — a prompt in combat and a prompt in a menu will not always ask for
the same one. This is the game's own behaviour, not a remap: what the buttons do
is inherited from the Android build and has not been reworked for the Vita's
layout yet. If a prompt does not respond, the touchscreen always will.

**Typing a name** — your character's, or a save's — opens the Vita's on-screen
keyboard. Tap the name box to bring it up, type, and confirm. *(New in v0.1.9.1.
On v0.1.9 and earlier there is no way to enter a name at all, which leaves
character creation with no way forward.)*

## Known issues

### Blocking

- **The world geometry tears apart after roughly 40 minutes.** Walls and floors
  smear into diagonal streaks, getting worse over the following minute or two,
  while the HUD and dialogue text keep drawing perfectly. It is not tied to any
  particular area — it builds up with time played and is still there when you
  walk somewhere else. Nothing crashes and your save is fine — relaunching and
  loading the same save came back clean, though that has only been tried once.
  **This is the thing being worked on.**

  What is known so far: it is not a failed allocation (vitaGL reports none), and
  the game's own heap is healthy at the time. Video memory does run dry a minute
  into play and stay that way, which is measured but not yet shown to be the
  cause.

### Rough edges

- **Stutter in dense scenes.** Busy areas issue around 700 draw calls a frame
  and the frame rate drops into the low 20s. Video memory is also full for most
  of a session, so textures loaded after the first minute are served from
  ordinary RAM, which likely contributes.
- **Long music tracks are silent.** Anything over about 90 seconds is replaced
  with correctly-timed silence, so pacing stays right but the score does not
  play. Shorter music and combat stings do. Voice and effects are unaffected.
- **Cutscenes are skipped.** The Bink video decoder is stubbed out, so FMVs are
  passed over rather than played.
- **Rear touch panel is disabled** deliberately — it sits under your fingers
  while holding the console and fired spurious taps.
- **No trophies.**

### Recently fixed

- **The area-transition crash** (v0.1.9.1). Large allocations now come from a
  pool of their own instead of being mixed in with the game's thousands of small
  long-lived objects, which is what shredded the heap. If you are on v0.1.9 or
  earlier, this is the 10–20 minute crash you will hit.
- **No way to name your character** (v0.1.9.1), which left character creation
  with nothing to press and no way on. The field asked the platform for a
  keyboard the Vita never provided, so it could not receive a letter. It now
  opens the Vita's own on-screen keyboard; the same fix covers save names.
- **Sound thinning out and then disappearing entirely, voices included**
  (v0.1.9.1), over a long session — and taking the game down with it. The engine
  was never told when a sound finished, so it never reused a voice or closed a
  music stream; the leak eventually exhausted both file handles and memory.

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
