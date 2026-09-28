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

**Work in progress — good enough for a real playthrough, if you save regularly.**

It boots, gets through character creation, and plays through the Endar Spire and
across Taris. Combat, dialogue, inventory, containers and saves all work, and it
looks and sounds like the game.

The two faults that used to wear a long session down are **fixed**: sound no
longer thins out and goes silent, and the world geometry no longer tears into
spikes. Sessions of close to 90 minutes now end as healthy as they started.
What remains is frame rate and a few rough edges. Nothing crashes and no save
has been lost. See [Known issues](#known-issues).

The wait before the main menu is now dressed in the game's own loading art, with
a hint to read while it works *(new in v0.1.10)*.

So: worth starting a proper playthrough on now. Save regularly all the same.

| | |
|---|---|
| Startup | ~2 min on first launch, then under a minute |
| Area loads | Under a second to load; the loading screen then stays up 15–20 s while textures upload |
| Frame rate | ~30–38 fps typical, dips into the low 20s in dense scenes, with stutters |
| Session length | ~90 min tested with nothing fatal and nothing wearing down |
| Audio | Effects, voice and music all work |
| Cutscenes | Bink movies play with sound; tap or press a button to skip |
| Input | Touchscreen, sticks and buttons — see [Controls](#controls) |
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

KOTOR's native Android gamepad path is mapped to the Vita controls. The left
stick moves, the right stick controls the camera, the D-pad navigates, and the
face buttons follow the familiar layout: Cross accepts, Circle cancels, Square
is X, and Triangle is Y. L and R are the shoulder actions; Start opens the
game's pause path. Select is currently reserved.

The front touchscreen works too, and you can mix it freely with the buttons:
tap menu items, dialogue choices and the on-screen combat buttons directly.
Some actions, like a normal attack, are easiest to reach by touch, since the
Vita has no L2/R2 for the Android layout's triggers. The rear touch panel is
off, so holding the console never taps anything.

**Typing a name** — your character's, or a save's — opens the Vita's on-screen
keyboard. Select the name field and confirm to bring it up, then type and accept. *(New in v0.1.9.1.
On v0.1.9 and earlier there is no way to enter a name at all, which leaves
character creation with no way forward.)*

## Language

The game ships its text in five languages, and the Android release picked one
from the phone's locale. There is no locale to read on the Vita, so the port
asks you instead.

**The first time you run it**, a short list comes up before the loading screen:

```
                       CHOOSE A LANGUAGE

                         > ENGLISH
                           FRANCAIS
                           ITALIANO
                           DEUTSCH
                           ESPANOL
```

Up and down to move. The bottom of the screen names the buttons — confirm is
whichever button your console uses to confirm, and the other one leaves things
alone. Your choice is saved and you are not asked again. **To change it later,
press L while the game is starting** — any time from launching it until the
loading screen appears, while the screen is still black — and the list comes
back.

The choice is written to `ux0:data/kotor/swkotor.ini`, and you can equally set
it there by hand:

```ini
[Game Options]
Language=fr
```

`en` (the default), `fr`, `it`, `de` and `es`. Anything else, or no line at all,
gives you English. If the section is not in your ini yet, add it.

This changes the on-screen text and the main-menu artwork. **Voice-over stays
English** in every language — the game data only ever shipped one set of
recordings, so this is what the retail releases did too.

### Fan translations

Unofficial translations show up in the same list, below the five built-in
languages. Give each one its own folder:

```
ux0:data/kotor/translations/Polski/tv_dialog.tlk
```

The folder name is what the list shows, so name it after the language. The
file inside can be called `tv_dialog.tlk` or `dialog.tlk`; either works. Then
**press L while the game is starting** and pick it. The choice is saved as
`Translation=Polski` next to `Language=` in `swkotor.ini`, and choosing one of
the built-in languages again switches it off.

Don't repack the OBB files, and don't edit them. The card folder is read first.

Things to know:

- A translation runs as English underneath, so the **main-menu buttons stay in
  English**: they are pictures, not text. The boot-screen tips use the
  translation's text.
- The game's fonts only have Western European letters. Accented Latin text
  (Portuguese, Polish without ł/ś/ż, and so on) shows up, but Cyrillic and
  other scripts show up as the wrong letters. A translation can include
  replacement font files in its folder: any file there overrides the game
  file with the same name. It is not yet confirmed that the game picks up fonts
  this way.
- The game's own text tables all have 49,265 entries. The log says how many
  entries your file has (`ux0:data/kotor/log.txt`, look for `[tr]`). If the
  numbers differ, the file was made for another edition of the game, and
  some lines may be missing or in the wrong place.

## Known issues

### Intermittent

None of these is reliable enough to reproduce on demand, and none of them costs
you a save. They are listed because you may hit them. Save regularly.

- **Input stopped responding once in an older build.** Around 50 minutes in,
  the camera stick and touch input stopped doing anything while the game kept
  running. It has not been seen since, but touch and buttons together have not
  had a long session yet. Relaunching clears it.

### Rough edges

- **Frame drops.** Besides the dense-scene dips below, some areas stutter
  about once a second: one frame in thirty takes around 100 ms, while nothing on
  screen changes. That time goes on the game's own update rather than drawing,
  and it is being investigated.
- **Voices over the loading screen.** The game starts running as soon as the
  area data is in, while the loading screen is still up for the texture upload,
  so the new area's sounds and conversations can start before you see it. Once,
  after dying and reloading, a line from the fight that killed you played over
  the loading screen; that one is being traced.
- **Some shiny armour looks off.** Reflective materials can shine too strongly
  or with a colour tint — Sith armour is the clearest example. The environment
  map and its mask reach the shader correctly, so the remaining suspect is how
  the reflection is blended with the lit colour.
- **The Undercity runs at 5–8 fps.** Measured against 30–40 fps in the areas
  above it. It draws roughly three and a half times as many objects per frame as
  the streets of Taris — about 480 draw calls a frame against 140 — and frame
  time scales with that almost exactly, so the port is spending its time issuing
  draws rather than the console being out of its depth. Playable, not pleasant.
- **Stutter in dense scenes.** Busy areas issue around 700 draw calls a frame
  and the frame rate drops into the low 20s. Video memory is also full for most
  of a session, so textures loaded after the first minute are served from
  ordinary RAM, which likely contributes.
- **Not every movie has been checked.** The ones played so far run with
  sound and hand back to the game cleanly; 48 kHz movies and localized
  subtitles have not been specifically tested.
- **No trophies.**

### Recently fixed

- **Footsteps, doors and containers were very quiet** *(not released yet)*.
  Early builds of the port misread decimal numbers in `swkotor.ini`, and the
  engine saved its 2D/3D sound balance at its lowest setting, which plays every
  positioned sound at a tenth of its volume. The port now resets that one value
  to the engine default on launch, so an ini carried over from an old build is
  repaired automatically.
- **Sound thinning out, then going silent, over a long session** *(not released
  yet)*. The game has 45 sound slots and frees one only when it sees the sound
  end; some never got that, and after an hour or so every slot was taken. Dead
  slots are now handed back, and an 87-minute session stayed clean.
- **World geometry tearing into spikes** after 20–40 minutes *(not released
  yet)*. The GPU's vertex-shader pool filled up and new shaders silently failed.
  The pool is four times larger and now reclaims idle entries; an 83-minute
  session had no failures.
- **Audio mixer lock contention:** the Vita mixer now snapshots active channels,
  mixes outside the game-facing mutex, and batches FMOD completion scanning under
  one lock. A real-Vita smoke test preserved working audio; the frame-rate effect
  still needs a controlled A/B. See the
  [investigation note](docs/specs/2026-09-12-audio-lock-contention.md).
- **Movies were skipped.** Bink cutscenes now play, video and sound, confirmed
  on hardware. Tapping the screen or pressing a button skips them.
- **Touchscreen is back.** It had been switched off in favour of the gamepad
  mapping; now both work at the same time.
- **Voice lines silent during in-game cutscenes** (v0.1.9.2). The MP3 decoder
  sized its output buffer by assuming every frame was the smallest one the
  format allows, which asked for 2.25x what a voice line actually needs — a
  13-second line wanted 1.8 MB when the heap could offer 1 MB, so it failed and
  the game played silence over the scene. It now sizes from the stream's own
  bitrate.
- **The area-transition crash** (v0.1.9.1). Large allocations now come from a
  pool of their own instead of being mixed in with the game's thousands of small
  long-lived objects, which is what shredded the heap. If you are on v0.1.9 or
  earlier, this is the 10–20 minute crash you will hit.
- **No way to name your character** (v0.1.9.1), which left character creation
  with nothing to press and no way on. The field asked the platform for a
  keyboard the Vita never provided, so it could not receive a letter. It now
  opens the Vita's own on-screen keyboard; the same fix covers save names.
- **Sound taking the game down with it** (v0.1.9.1). The engine was never told
  when a sound finished, so it never reused a voice or closed a music stream;
  the leak exhausted both file handles and memory and ended the session. That
  crash is gone.

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

Builds require the vitaGL packed-VBO patch and shader cache documented in
`SETUP.md`. Without
it, the game's large vertex offsets truncate above 64 KiB and skinned layouts
can underflow when shader attribute order differs from memory order, causing
geometry to stretch or explode. Build vitaGL with `HAVE_SHADER_CACHE=1` so
compiled variants persist under `ux0:data/shader_cache/KOTR00001/`; otherwise
first-use effects can block for several seconds. Use `NO_SPLASHSCREEN=1` for
Vita3K packages.

Intermittent slow frames are reported as `[hitch]` lines in
`ux0:data/kotor/log.txt`. Each line separates game-thread work from buffer-swap
wait and includes that frame's draw, texture, buffer, OBB I/O, and audio decode
activity, plus time inside the engine's `GameUpdate` and `UpdateScreen` calls.
The trace is event-driven and rate-limited so it does not log every frame.
KOTOR's adaptive render suppression is disabled by default because it amplified
one expensive AI update into as many as eleven updates before presenting again.

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
