/* sdl_patch.c -- SDL2 resolution for the KOTOR Android build (see sdl_patch.h)
 *
 * Almost every SDL_* import maps straight to Vita-native SDL2; the exceptions
 * (mirroring TheOfficialFloW/Rinnegatamante so-loader ports):
 *   SDL_CreateWindow     -> force the 960x544 Vita window
 *   SDL_RWFromFile       -> translate Android paths to ux0:data/kotor first
 *   SDL_GL_SetSwapInterval-> ret0 (vsync handled by GXM)
 *   SDL_IsChromebook     -> ret0 (not present in Vita SDL2)
 *
 * g_SDL_BufferGeometry_w/h are game globals defined in NEITHER .so, so we own
 * them; the game reads them for its framebuffer size. (g_SDL_Window/g_SDL_mode
 * live in libandroid_port and resolve cross-module.)
 */

#include <vitasdk.h>
#include <SDL2/SDL.h>
#include <vitaGL.h>
#include <string.h>
#include <stdio.h>

#include "config.h"
#include "sdl_patch.h"
#include "dynlib.h"   // g_game_threads registry (watchdog sweeps it)
#include "fs_patch.h"
#include "gl_patch.h"
#include "ime_patch.h"
#include "input_patch.h"
#include "so_util.h"
#include "log.h"

extern int ret0(void);   // from dynlib.c

// Game-owned globals we must provide (framebuffer geometry).
int g_SDL_BufferGeometry_w = SCREEN_W;
int g_SDL_BufferGeometry_h = SCREEN_H;

// GL context is owned by vitaGL (vglInitExtended in main), NOT by SDL: the Vita
// SDL2 "VITA" video driver has no OpenGL backend, so requesting SDL_WINDOW_OPENGL
// fails and returns NULL. Strip that flag so SDL creates a plain framebuffer
// window; GL context creation/swap are redirected to vitaGL below.
static SDL_Window *SDL_CreateWindow_hook(const char *title, int x, int y,
                                         int w, int h, Uint32 flags) {
  Uint32 nogl = flags & ~(Uint32)SDL_WINDOW_OPENGL;
  log_printf("[SDL] CreateWindow(\"%s\", %dx%d, flags=0x%x) -> %dx%d (GL via vitaGL)",
             title ? title : "?", w, h, (unsigned)flags, SCREEN_W, SCREEN_H);
  SDL_Window *win = SDL_CreateWindow("KOTOR", 0, 0, SCREEN_W, SCREEN_H, nogl);
  if (!win)
    log_printf("[SDL] CreateWindow FAILED: %s", SDL_GetError());
  else
    input_touch_init();  // SDL video is up now -> enable front-touch injection
  return win;
}

// vitaGL already stood up the GL context in main(); hand the game a non-null
// sentinel so its `if (!ctx)` checks pass, and present frames via vitaGL.
static SDL_GLContext SDL_GL_CreateContext_hook(SDL_Window *w) {
  log_printf("[SDL] GL_CreateContext -> vitaGL context (sentinel)");
  return (SDL_GLContext)1;
}
static void SDL_GL_SwapWindow_hook(SDL_Window *w) {
  (void)w;
  // The on-screen keyboard is a system common dialog: it is composited into the
  // back buffer by vglSwapBuffers, and only when we ask for it. Passing GL_TRUE
  // unconditionally would cost a sceCommonDialogUpdate every frame of the game.
  vglSwapBuffers(ime_dialog_active() ? GL_TRUE : GL_FALSE);
  gl_patch_on_swap();  // periodic per-frame draw summary (see gl_patch.c)
  input_touch_pump();  // inject Vita front-touch as SDL finger events
  ime_pump();          // collect what the on-screen keyboard produced
}

// The game thread hangs in a sleep-poll loop (watchdog: WAITING, no kernel
// object, near-zero CPU). Log the CALLER's return address at the delay so
// addr2line pins the exact loop. Throttled: first few + a periodic sample.
// log74: the module load stalls in CServerExoAppInternal::MainLoop, which only
// calls CSWSModule::LoadModuleFinish once a completion counter catches up:
//     [r6+12] != 0 && [r6+12] == [r6+8] && [r6+20] != 1
// i.e. an outstanding-request barrier that never clears. The pthread_create
// registry stayed EMPTY, which looked like "no workers exist" -- but the game does
// not use pthreads for them: libandroid_port's ThreadHandle ctor
// (ThreadHandle::ThreadHandle(int(*)(void*), void*, bool) @ +0x80ad4) creates
// threads through SDL_CreateThread, which lands in our SDL2 and never touches the
// pthread binding. So every worker has been invisible to the watchdog.
// Register them in the same table the watchdog sweeps, capturing the entry point
// so a frozen worker can be named with addr2line.
typedef struct { SDL_ThreadFunction fn; void *data; int slot; } sdl_thread_shim_t;

static int sdl_thread_shim(void *p) {
  sdl_thread_shim_t *s = (sdl_thread_shim_t *)p;
  SDL_ThreadFunction fn = s->fn;
  void *data = s->data;
  int slot = s->slot;
  free(s);
  if (slot >= 0 && slot < GAME_THREADS_MAX)
    g_game_threads[slot].thid = sceKernelGetThreadId();
  log_printf("[thread] SDL thread started thid=0x%08x entry=%p slot=%d",
             (unsigned)sceKernelGetThreadId(), (void *)fn, slot);
  return fn(data);
}

static SDL_Thread *SDL_CreateThread_hook(SDL_ThreadFunction fn, const char *name, void *data) {
  int slot = -1;
  if (g_game_threads_n < GAME_THREADS_MAX) {
    slot = g_game_threads_n++;
    g_game_threads[slot].thid  = -1;
    g_game_threads[slot].entry = (uintptr_t)fn;
  }
  sdl_thread_shim_t *s = (sdl_thread_shim_t *)malloc(sizeof(*s));
  if (!s)
    return SDL_CreateThread(fn, name, data);
  s->fn = fn; s->data = data; s->slot = slot;
  SDL_Thread *t = SDL_CreateThread(&sdl_thread_shim, name ? name : "kotor", s);
  log_printf("[thread] SDL_CreateThread(\"%.24s\") entry=%p -> %p slot=%d",
             name ? name : "(null)", (void *)fn, (void *)t, slot);
  if (!t)
    free(s);
  return t;
}

static void SDL_Delay_hook(Uint32 ms) {
  static volatile int n = 0;
  int c = n++;
  if (c < 4 || (c & 1023) == 0)
    log_printf("[sleep] SDL_Delay(%u) #%d LR=%p", (unsigned)ms, c, __builtin_return_address(0));
  SDL_Delay(ms);
}

// --- input event tracing -------------------------------------------------
// The game booted to the main menu and idles in a render/SDL_Delay/poll loop
// waiting for input. Events are wired straight to vitasdk SDL2 (the Vita video
// backend, which reads the pad + front touch), but we had no visibility into
// whether the game polls or whether any events actually arrive. These wrappers
// log, rate-limited: how often the game pumps/polls, and EVERY delivered event's
// type -- so a run where the user taps/presses reveals if the backend produces
// events and the game consumes them. SDL_EventType values of interest:
//   0x300/0x301 KEYDOWN/UP, 0x400-0x406 MOUSE, 0x600 JOYAXIS, 0x603/4 JOYBUTTON,
//   0x700 FINGERDOWN, 0x701 FINGERUP, 0x702 FINGERMOTION, 0x650+ CONTROLLER.
// Per-event logging was bring-up scaffolding and has served its purpose: we know
// events arrive and the game consumes them. It is now a real cost. log60: 4481
// [input] lines, 41% of the whole log, dominated by 2385 SDL_JOYAXISMOTION (the
// Vita sticks emit continuously even untouched) -- and every line is a write to
// ux0:. Frame rate was bimodal 11.6-35.3 fps, worst while touching. This is the
// same trap that once pinned the game at 1 fps.
//
// Keep the diagnostic value, drop the cost: log the FIRST sighting of each event
// type (that is the interesting signal -- "a new kind of event started arriving")
// then only counts, summarised occasionally.
#define EVT_SLOTS 24
static struct { unsigned type; unsigned n; } g_evt[EVT_SLOTS];
static unsigned g_evt_used = 0, g_evt_total = 0;

static void log_event(const char *via, const SDL_Event *e) {
  unsigned t = (unsigned)e->type, i;
  for (i = 0; i < g_evt_used; i++)
    if (g_evt[i].type == t) break;
  if (i == g_evt_used) {
    if (g_evt_used < EVT_SLOTS) {
      g_evt[g_evt_used].type = t;
      g_evt[g_evt_used].n = 0;
      g_evt_used++;
      log_printf("[input] %s -> FIRST event type=0x%x", via, t);
    } else {
      i = EVT_SLOTS - 1;  // table full: fold strays into the last slot
    }
  }
  if (i < EVT_SLOTS) g_evt[i].n++;

  // log99/log101: the on-screen GAME PAD legend does not match what the buttons
  // actually do. SDL_main dispatches joystick events through a tbh table at
  // +0x18be88 (idx3 = SDL_JOYBUTTONDOWN -> +0x18c092, idx4 = UP -> +0x18c120,
  // idx2 = HAT -> +0x18c0d8).
  //
  // CORRECTION (was: "a std::map built for Android's SDL joystick ordering, so
  // remap the indices"). That theory is WRONG and the remap would be a no-op.
  // gamepadButtonById / gamepadButtonByHatId are std::map<int,GamepadButton>
  // (libc++ __ndk1::__tree; node = left@0 right@4 parent@8 black@12 key@16
  // value@20) living in .bss at 0x5c438c / 0x5c4398, and *nothing ever
  // populates them*:
  //   - each has exactly ONE GOT slot (0x5a38c0 / 0x5a38d4), and a full-.text
  //     scan finds exactly ONE site materialising each, both in this dispatcher
  //     (+0x18be24, +0x18c0da); libandroid_port.so references neither symbol.
  //   - +0x18c47a is std::map::operator[]'s insert path: new 24, key = the RAW
  //     button index at +16, value initialised to *0* at +20, then
  //     __tree_balance_after_insert, size++.
  //   - +0x18c4e8 then does  pressedGamepadButtons |= node[+20].
  //   - JOYDEVICEADDED only calls OpenFirstJoystick, which appends to the
  //     `joysticks` list and sets gamepadConnected. It builds no table.
  // So every press ORs ZERO into the mask, for any index: the joystick path is
  // inert by construction, not mis-indexed. (Note the map does grow at runtime,
  // one all-zero node per distinct index pressed -- a nonzero size() is NOT
  // evidence of a real table.)
  //
  // The fix is therefore to POPULATE the maps ourselves, keyed by the Vita
  // indices captured below, with the right GamepadButton bits. What is known of
  // that bitmask so far: it is a true bitmask (ReplaceGamepadInputForCombo at
  // +0x18b3ec does and/bic/orr on the whole word); the D-pad occupies
  // 0x100..0x800 (the hat handler bic's 0xf00 before OR-ing); 0x1000 is cleared
  // on the app-background/back-button path (+0x18c1d2).
  //
  // Vita indices, established empirically from log101 (press order
  // X O /\ [] L R START SELECT then D-pad up/down/left/right):
  //   0 triangle  1 circle  2 cross  3 square  4 L  5 R
  //   6 dpad-down 7 dpad-left 8 dpad-up 9 dpad-right  10 select  11 start
  // Budgeted -- this is a bring-up probe, not steady-state logging.
  static unsigned btn_n = 0, hat_n = 0;
  if ((t == SDL_JOYBUTTONDOWN || t == SDL_JOYBUTTONUP) && btn_n < 120) {
    btn_n++;
    log_printf("[input] JOYBUTTON%s which=%d button=%u  [#%u]",
               t == SDL_JOYBUTTONDOWN ? "DOWN" : "UP",
               (int)e->jbutton.which, (unsigned)e->jbutton.button, btn_n);
  } else if (t == SDL_JOYHATMOTION && hat_n < 40) {
    hat_n++;
    log_printf("[input] JOYHAT which=%d hat=%u value=0x%x  [#%u]",
               (int)e->jhat.which, (unsigned)e->jhat.hat,
               (unsigned)e->jhat.value, hat_n);
  }

  if ((++g_evt_total % 4096) == 0) {
    char buf[256];
    int p = 0;
    for (i = 0; i < g_evt_used && p < (int)sizeof(buf) - 24; i++)
      p += snprintf(buf + p, sizeof(buf) - p, "%s0x%x=%u",
                    i ? " " : "", g_evt[i].type, g_evt[i].n);
    log_printf("[input] %u events: %s", g_evt_total, buf);
  }
}
static void SDL_PumpEvents_hook(void) {
  static volatile int n = 0;
  int c = n++;
  if (c < 3 || (c & 2047) == 0)
    log_printf("[input] SDL_PumpEvents #%d (game is polling)", c);
  SDL_PumpEvents();
}
static int SDL_PollEvent_hook(SDL_Event *e) {
  int r = SDL_PollEvent(e);
  if (r && e) log_event("PollEvent", e);
  return r;
}
static int SDL_PeepEvents_hook(SDL_Event *e, int num, SDL_eventaction action,
                               Uint32 minType, Uint32 maxType) {
  int r = SDL_PeepEvents(e, num, action, minType, maxType);
  if (r > 0 && e && action != SDL_ADDEVENT)
    for (int i = 0; i < r && i < num; i++) log_event("PeepEvents", &e[i]);
  return r;
}

// Shaders (*.vert/*.frag/*.glsl) are the one asset class we EDIT (varying-packing
// to fit GXM's TEXCOORD budget). Copying them onto ux0:data/kotor/ proved
// unreliable (USB write-cache / non-overwriting transfers left stale originals on
// the card). So we bundle the edited copies inside the VPK and serve them from the
// read-only install dir (app0:shaders/) first, falling back to the card. This makes
// the shader set travel with the loader -- no manual copy, no stale-file class of bug.
static int is_shader_name(const char *base) {
  const char *dot = strrchr(base, '.');
  if (!dot) return 0;
  return !strcmp(dot, ".vert") || !strcmp(dot, ".frag") || !strcmp(dot, ".glsl");
}

// Font metrics (.txi) are bundled in the VPK at app0:fonts/ (see CMakeLists +
// AurResGet .txi-fallback hook). Aspyr's loose override font TGAs lack their .txi;
// serving ours here is what finally lets fontInfo populate and GUI text render.
static int is_font_txi_name(const char *base) {
  const char *dot = strrchr(base, '.');
  return dot && !strcmp(dot, ".txi");
}

// OBB fallback for plain file opens.
//
// The game reads game data through two different doors, and only one of them
// knew about the archives. Resource-manager reads reach the companion's ObbFile
// and work; anything opened as an ordinary file lands here, where we used to try
// only the VPK and then ux0:data/kotor/ before giving up. That is why
// modules/END_M01AA.rim came back empty while modules/END_M01AA_s.rim -- same
// directory, same archive -- loaded fine, and why LoadModuleStart failed with
// rc=1.
//
// libandroid_port already exports everything needed, we simply never called it:
//   ObbFile::RWFromFile(const char *)  +0x57eb0  -> SDL_RWops* straight out of the zip
//   g_mainObb  +0x143164, g_patchObb  +0x14316c  -> the mounted instances
// It strips a leading "./", collapses "//", and looks the name up with
// mz_zip_reader_locate_file(flags=0), i.e. CASE-INSENSITIVELY -- so the game's
// "./modules/END_M01AA.rim" matches the archive's "modules/end_m01aa.rim"
// without us normalising anything.
//
// Read modes only: writes must keep going to the card. Patch archive first so it
// overrides main, matching the mount order. Because this runs only after the card
// lookup has already failed, it cannot change the behaviour of anything that
// works today -- it only turns a MISS into a hit.
static SDL_RWops *(*ObbFile_RWFromFile)(void *self, const char *name) = NULL;
static void **g_mainObb_p = NULL, **g_patchObb_p = NULL;
static int g_obb_fallback_ready = 0;
static unsigned g_obb_hit = 0, g_obb_miss = 0;

void sdl_obb_fallback_init(uintptr_t rwfromfile, uintptr_t mainobb, uintptr_t patchobb) {
  ObbFile_RWFromFile = (SDL_RWops *(*)(void *, const char *))rwfromfile;
  g_mainObb_p  = (void **)mainobb;
  g_patchObb_p = (void **)patchobb;
  g_obb_fallback_ready = (ObbFile_RWFromFile && (g_mainObb_p || g_patchObb_p));
  log_printf("[obb] RWFromFile fallback %s (fn=%p main=%p patch=%p)",
             g_obb_fallback_ready ? "ARMED" : "UNAVAILABLE",
             (void *)rwfromfile, (void *)mainobb, (void *)patchobb);
}

static SDL_RWops *obb_try_open(const char *fname) {
  if (!g_obb_fallback_ready || !fname) return NULL;
  void *obbs[2] = { g_patchObb_p ? *g_patchObb_p : NULL,
                    g_mainObb_p  ? *g_mainObb_p  : NULL };
  for (int i = 0; i < 2; i++) {
    if (!obbs[i]) continue;
    SDL_RWops *rw = ObbFile_RWFromFile(obbs[i], fname);
    if (rw) {
      if (g_obb_hit < 64)
        log_printf("[obb] served from %s OBB: %s", i == 0 ? "patch" : "main", fname);
      g_obb_hit++;
      return rw;
    }
  }
  g_obb_miss++;
  return NULL;
}

static SDL_RWops *SDL_RWFromFile_hook(const char *fname, const char *mode) {
  // Read-only opens for VPK-bundled assets: prefer our bundled copy.
  if (fname && mode && (mode[0] == 'r')) {
    const char *slash = strrchr(fname, '/');
    const char *base = slash ? slash + 1 : fname;
    if (is_shader_name(base)) {
      char ap[256];
      snprintf(ap, sizeof(ap), "app0:shaders/%s", base);
      SDL_RWops *brw = SDL_RWFromFile(ap, mode);
      if (brw) {
        log_printf("[SDL] shader from VPK: %s", ap);
        return brw;
      }
    } else if (is_font_txi_name(base)) {
      char ap[256];
      snprintf(ap, sizeof(ap), "app0:fonts/%s", base);
      SDL_RWops *brw = SDL_RWFromFile(ap, mode);
      if (brw) {
        log_printf("[SDL] txi from VPK: %s", ap);
        return brw;
      }
    }
  }

  char t[512];
  fs_translate(fname, t, sizeof(t));
  SDL_RWops *rw = SDL_RWFromFile(t, mode);
  if (!rw && mode && mode[0] == 'r') {
    // Not on the card -- ask the archives, using the ORIGINAL name (ObbFile wants
    // the game-relative path, not our ux0: translation).
    rw = obb_try_open(fname);
  }
  // A MISS is usually benign (the game probes several extensions per resource)
  // and in-game they repeat per texture load,
  // so budget them the way the GL and FS traces are budgeted.
  if (!rw) {
    static unsigned miss_n = 0;
    if (miss_n < 600) {
      log_printf("[SDL] RWFromFile MISS: %s (%s)", t, mode ? mode : "?");
      if (++miss_n == 600)
        log_printf("[SDL] RWFromFile MISS trace silenced after 600 lines");
    }
  }

  // log88: the module .ifo is demanded by ResRef "MODULE" out of the archive the
  // engine registered, which is "CURRENTGAME:END_M01AA" -- the COPY in
  // currentgame/, not modules/. CExoResMan::Demand bails immediately because the
  // CRes's id (+8) is still 0xFFFFFFFF, i.e. "MODULE" was never found. The
  // original modules/end_m01aa.rim provably contains `module` type=2014, so the
  // prime suspect is that the copy is truncated or empty. Sizes settle it: a good
  // copy is 55565 bytes.
  if (rw && fname && (strstr(fname, "currentgame") || strstr(fname, "modules/"))) {
    Sint64 sz = SDL_RWsize(rw);
    log_printf("[SDL] size: %s (%s) -> %lld bytes", fname, mode ? mode : "?",
               (long long)sz);
  }
  return rw;
}

// Deliberately routed through the hook rather than raw SDL: the audio backend is
// handed game-relative names like ".\STREAMMUSIC\mus_theme_cult.mp3", which only
// resolve after path translation plus the OBB fallback.
void *sdl_load_file(const char *name, unsigned int *out_len) {
  if (out_len) *out_len = 0;
  if (!name) return NULL;
  SDL_RWops *rw = SDL_RWFromFile_hook(name, "rb");
  if (!rw) return NULL;
  return sdl_slurp_rwops_close(rw, out_len);
}

void *sdl_slurp_rwops_close(void *rwops, unsigned int *out_len) {
  SDL_RWops *rw = (SDL_RWops *)rwops;
  if (out_len) *out_len = 0;
  if (!rw) return NULL;

  Sint64 sz = SDL_RWsize(rw);
  if (sz <= 0) { SDL_RWclose(rw); return NULL; }

  char *buf = (char *)malloc((size_t)sz + 1);
  if (!buf) { SDL_RWclose(rw); return NULL; }

  size_t got = SDL_RWread(rw, buf, 1, (size_t)sz);
  SDL_RWclose(rw);
  if (got == 0) { free(buf); return NULL; }

  buf[got] = '\0';
  if (out_len) *out_len = (unsigned int)got;
  return buf;
}

static const so_default_dynlib sdl_dynlib[] = {
  // window / GL context (SDL owns creation; vitaGL is the backend)
  { "SDL_Init",                    (uintptr_t)&SDL_Init },
  { "SDL_Quit",                    (uintptr_t)&SDL_Quit },
  { "SDL_CreateWindow",            (uintptr_t)&SDL_CreateWindow_hook },
  { "SDL_DestroyWindow",           (uintptr_t)&SDL_DestroyWindow },
  { "SDL_GetWindowSize",           (uintptr_t)&SDL_GetWindowSize },
  { "SDL_GetDisplayMode",          (uintptr_t)&SDL_GetDisplayMode },
  { "SDL_GL_CreateContext",        (uintptr_t)&SDL_GL_CreateContext_hook },
  { "SDL_GL_DeleteContext",        (uintptr_t)&ret0 },
  { "SDL_GL_SetAttribute",         (uintptr_t)&ret0 },
  { "SDL_GL_SetSwapInterval",      (uintptr_t)&ret0 },
  { "SDL_GL_SwapWindow",           (uintptr_t)&SDL_GL_SwapWindow_hook },
  // error / hints
  { "SDL_GetError",                (uintptr_t)&SDL_GetError },
  { "SDL_GetHint",                 (uintptr_t)&SDL_GetHint },
  { "SDL_SetHint",                 (uintptr_t)&SDL_SetHint },
  { "SDL_IsChromebook",            (uintptr_t)&ret0 },
  { "SDL_IsScreenKeyboardShown",   (uintptr_t)&SDL_IsScreenKeyboardShown },
  // timing
  { "SDL_Delay",                   (uintptr_t)&SDL_Delay_hook },
  { "SDL_GetTicks",                (uintptr_t)&SDL_GetTicks },
  { "SDL_GetPerformanceCounter",   (uintptr_t)&SDL_GetPerformanceCounter },
  { "SDL_GetPerformanceFrequency", (uintptr_t)&SDL_GetPerformanceFrequency },
  // threading
  { "SDL_CreateMutex",             (uintptr_t)&SDL_CreateMutex },
  { "SDL_DestroyMutex",            (uintptr_t)&SDL_DestroyMutex },
  { "SDL_LockMutex",               (uintptr_t)&SDL_LockMutex },
  { "SDL_UnlockMutex",             (uintptr_t)&SDL_UnlockMutex },
  { "SDL_CreateCond",              (uintptr_t)&SDL_CreateCond },
  { "SDL_DestroyCond",             (uintptr_t)&SDL_DestroyCond },
  { "SDL_CondSignal",              (uintptr_t)&SDL_CondSignal },
  { "SDL_CondWait",                (uintptr_t)&SDL_CondWait },
  { "SDL_CreateThread",            (uintptr_t)&SDL_CreateThread_hook },
  { "SDL_WaitThread",              (uintptr_t)&SDL_WaitThread },
  { "SDL_GetThreadID",             (uintptr_t)&SDL_GetThreadID },
  // events
  { "SDL_PeepEvents",              (uintptr_t)&SDL_PeepEvents_hook },
  { "SDL_PollEvent",               (uintptr_t)&SDL_PollEvent_hook },
  { "SDL_PumpEvents",              (uintptr_t)&SDL_PumpEvents_hook },
  { "SDL_PushEvent",               (uintptr_t)&SDL_PushEvent },
  { "SDL_FlushEvents",             (uintptr_t)&SDL_FlushEvents },
  // joystick / text input
  { "SDL_NumJoysticks",            (uintptr_t)&SDL_NumJoysticks },
  { "SDL_JoystickOpen",            (uintptr_t)&SDL_JoystickOpen },
  { "SDL_JoystickClose",           (uintptr_t)&SDL_JoystickClose },
  { "SDL_JoystickGetAttached",     (uintptr_t)&SDL_JoystickGetAttached },
  { "SDL_JoystickInstanceID",      (uintptr_t)&SDL_JoystickInstanceID },
  { "SDL_StartTextInput",          (uintptr_t)&SDL_StartTextInput },
  { "SDL_StopTextInput",           (uintptr_t)&SDL_StopTextInput },
  // file IO (translate Android paths)
  { "SDL_RWFromFile",              (uintptr_t)&SDL_RWFromFile_hook },
  // RWops alloc/free: ObbFile::RWFromFile builds a custom RWops (its own
  // read/seek/close callbacks into the OBB) via SDL_AllocRW; map straight to
  // native SDL2. Missing -> unrelocated GOT slot -> PREFETCH_ABORT on blx.
  { "SDL_AllocRW",                 (uintptr_t)&SDL_AllocRW },
  { "SDL_FreeRW",                  (uintptr_t)&SDL_FreeRW },
  // game-owned globals we provide
  { "g_SDL_BufferGeometry_w",      (uintptr_t)&g_SDL_BufferGeometry_w },
  { "g_SDL_BufferGeometry_h",      (uintptr_t)&g_SDL_BufferGeometry_h },
};
const int sdl_dynlib_size = sizeof(sdl_dynlib);
const so_default_dynlib *sdl_get_dynlib(void) { return sdl_dynlib; }
