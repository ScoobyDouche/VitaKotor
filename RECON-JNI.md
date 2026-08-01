# RECON-JNI.md — KOTOR native / JNI surface

Map of how the Android build crosses the Java↔native boundary, so the loader
knows what to fake and where the real entry is. Derived **statically** from
`libKOTOR.so`'s dynamic symbol table (`readelf --dyn-syms`) — this build needs
no run to enumerate its surface (see "No JNI_OnLoad" below).

Regenerate:

```bash
arm-vita-eabi-readelf --dyn-syms -W apk/lib/armeabi-v7a/libKOTOR.so \
  | grep -v ' UND ' | grep -oE '(Java_[A-Za-z0-9_]+|SDL_main)' | sort -u
```

## Key finding: no `JNI_OnLoad`, no `RegisterNatives`

- **`JNI_OnLoad` / `JNI_OnUnload`: absent** (0 matches in the dynsym).
- The game **does not register natives at runtime**. Its native methods are
  resolved by the Android linker via **static export name** (`Java_<class>_<method>`).
- Therefore the "native surface" is the exported `Java_*` list below, captured
  statically — there is no runtime `RegisterNatives` dump to collect. (Our fake
  env still instruments `RegisterNatives` as insurance; it is not expected to fire.)

Architecture: Aspyr replaced SDL2's stock Android activity glue
(`org.libsdl.app.SDLActivity`, `nativeRunMain`, `nativeSetupJNI` — none present)
with their own **`com.aspyr.kotor.KOTOR`** activity. The real program entry is
**`SDL_main`**.

## Native surface — 14 exported `Java_com_aspyr_kotor_KOTOR_*` methods

All are **lifecycle / event callbacks driven by the Java activity** — none is the
program entry. They are called *by* the (nonexistent-on-Vita) Java side, so on
Vita we drive them ourselves only where needed.

| Method | Role | Loader relevance |
|---|---|---|
| `mountObb` / `mountPatchObb` / `unmountObbs` | mount the `.obb` game data | **High** — asset access; tie to `ux0:data/kotor/` + OBB path (step 2) |
| `nativeCreateMutex` | single-instance guard | Low — safe to skip/no-op |
| `nativeOnPause` / `nativeOnResume` | activity focus | Med — call `nativeOnResume` at startup if the game gates on it |
| `nativeExitApplication` / `nativeQuitOk` | shutdown | Low — wire to clean exit later |
| `nativePopupClosed` | dialog dismissed | Low |
| `nativeProfilerClicked` | debug/telemetry | Skip |
| `nativeSignIn` / `nativeSignOut` | Google Play Games | Stub — no online services on Vita |
| `nativeOnSynchCloudSaveOver` / `nativeOnSynchCloudSaveFailed` | cloud saves | Stub — local saves only |

## Program entry

| Symbol | Meaning |
|---|---|
| **`SDL_main`** | the game's real `main()` (SDL renames `main`→`SDL_main`). **This is what the loader must call** to start the game (Phase 1, step 3). |

## JNIEnv slots the game actually uses

The lib carries its own out-of-line `_JNIEnv::` wrapper methods (rather than
inlining them). Each does `va_start` then dispatches to the matching **`*MethodV`**
slot of the JNI function table — so those are the slots our fake env must
implement. Enumerated from the exported `_ZN7_JNIEnv*` symbols:

| `_JNIEnv::` wrapper used | Dispatches to table slot | idx (offset) |
|---|---|---|
| `NewObject` | `NewObjectV` | 29 (0xB4) |
| `CallObjectMethod` | `CallObjectMethodV` | 35 (0x8C) |
| `CallVoidMethod` | `CallVoidMethodV` | 62 (0xF8) |
| `CallStaticObjectMethod` | `CallStaticObjectMethodV` | 115 (0x1CC) |
| `CallStaticBooleanMethod` | `CallStaticBooleanMethodV` | 118 (0x1D8) |
| `CallStaticIntMethod` | `CallStaticIntMethodV` | 130 (0x208) |
| `CallStaticFloatMethod` | `CallStaticFloatMethodV` | 136 (0x220) |
| `CallStaticVoidMethod` | `CallStaticVoidMethodV` | 142 (0x238) |

Plus the lookups the wrappers depend on (called inline via the table):
`FindClass` (6), `GetMethodID` (33), `GetStaticMethodID` (113), string helpers,
refs, exceptions. All of these are populated in `loader/jni_patch.c`.

⚠ **`CallStaticFloatMethod` returns a `float` across the `.so` boundary.** Under
the hardfp loader vs softfp Android ABI its result lands in the wrong register.
This is the first concrete place the float-ABI mismatch will bite; it is handled
globally (softfp toolchain / vitaGL rebuild), not per-call.

## Consequence for the bring-up plan

The original step 1 ("call `JNI_OnLoad`, dump `RegisterNatives`") does not apply:
there is no `JNI_OnLoad` and no runtime registration. The env is now fully
instrumented; it will light up when the real entry (`SDL_main`) runs. The first
hardware test that exercises the JNI trace therefore happens at the entry-point
step, not before.
