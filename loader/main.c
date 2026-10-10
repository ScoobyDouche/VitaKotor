/* main.c -- KOTOR PS Vita so-loader (skeleton)
 *
 * Bring-up scaffold adapted from TheOfficialFloW/gtasa_vita. Goal at this stage:
 * link cleanly into a VPK. It loads libKOTOR.so + libandroid_port.so, relocates
 * and resolves them against the stub/wiring tables, stubs Bink + audio, and
 * initialises vitaGL -- but deliberately stops before invoking the game
 * (JNI_OnLoad / init). See RECON.md for the porting phases.
 */

#include <vitasdk.h>
#include <kubridge.h>
#include <vitaGL.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "main.h"
#include "config.h"
#include "so_util.h"
#include "dynlib.h"
#include "loadscreen.h"
#include "jni_patch.h"
#include "ini.h"
#include "langsel.h"
#include "translation.h"
#include "modset.h"
#include "audio_patch.h"
#include "bink_patch.h"
#include "fs_patch.h"
#include "input_patch.h"
#include "sdl_patch.h"
#include "gl_patch.h"
#include "ime_patch.h"
#include "crash.h"
#include "heap.h"
#include "bigalloc.h"
#include "log.h"
#include "gxm_patcher.h"
#include "gameprof.h"
#include "lzma_cache.h"
#include "obb_cache.h"
#include "threads.h"
#include "ini_write.h"

// SDL.h would #define main to SDL_main; this is the only SDL call made here.
extern int SDL_setenv(const char *name, const char *value, int overwrite);

#include <pthread.h>

so_module kotor_mod;
so_module port_mod;
so_module miniz_mod;
so_module lzma_mod;

unsigned int _newlib_heap_size_user = MEMORY_NEWLIB_MB * 1024 * 1024;

int debugPrintf(const char *text, ...) {
  va_list args;
  char buf[1024];
  va_start(args, text);
  vsnprintf(buf, sizeof(buf), text, args);
  va_end(args);
  log_printf("%s", buf);
  return 0;
}

void fatal_error(const char *fmt, ...) {
  va_list args;
  char buf[1024];
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  log_printf("[CRASH] %s", buf);

  // Show the message on-screen too, then wait so it can be read.
  vglInit(0);
  // (LiveArea/dialog wiring comes later; for now just spin.)
  while (1)
    sceKernelDelayThread(1000 * 1000);
}

int check_kubridge(void) {
  int search_unk[2] = {0};
  return _vshKernelSearchModuleByName("kubridge", search_unk);
}

int file_exists(const char *path) {
  SceIoStat stat;
  return sceIoGetstat(path, &stat) >= 0;
}

// Load + relocate + resolve a module against all three stub/wiring tables.
static int load_module(so_module *mod, const char *path, uintptr_t addr) {
  log_printf("Loading %s @ 0x%08x", path, (unsigned)addr);
  if (!file_exists(path)) {
    log_printf("  file not found on device: %s", path);
    return -1;
  }
  int res = so_load(mod, path, addr);
  if (res < 0) {
    // so_load return codes: <0 sceIoOpen (file), memblock alloc fail,
    // -1 bad ELF magic, -2 missing .dynamic/.dynsym/.rel sections.
    log_printf("  so_load failed: 0x%08x (%d)", (unsigned)res, res);
    return res;
  }
  so_relocate(mod);
  // Pass 0 also resolves cross-module (e.g. KOTOR -> libandroid_port exports).
  so_resolve(mod, default_dynlib, default_dynlib_size, 0);
  so_resolve(mod, (so_default_dynlib *)audio_get_dynlib(), audio_dynlib_size, 1);
  so_resolve(mod, (so_default_dynlib *)jni_get_dynlib(), jni_dynlib_size, 1);
  so_resolve(mod, (so_default_dynlib *)fs_get_dynlib(), fs_dynlib_size, 1);
  so_resolve(mod, (so_default_dynlib *)sdl_get_dynlib(), sdl_dynlib_size, 1);
  so_resolve(mod, (so_default_dynlib *)gl_get_dynlib(), gl_dynlib_size, 1);
  so_resolve(mod, (so_default_dynlib *)ime_get_dynlib(), ime_dynlib_size, 1);
  so_flush_caches(mod);
  return 0;
}

// Game thread's kernel UID, published by game_main_thread so the watchdog can
// sample it. -1 until the game thread starts.
static volatile SceUID g_game_thid = -1;

// Watchdog: every 3s, snapshot the game thread. runClocks rising => it's
// spinning (CPU loop); runClocks frozen + status WAITING => blocked on a sync
// object (waitType/waitId name it). This is how we localise a hang that makes
// no traceable calls, without a userland PC read.
static uint64_t g_thread_last_run[GAME_THREADS_MAX];

/* Defined with the sound probes further down; the watchdog is its clock. */
static void sound_pipeline_census(void);

/* Hitch sampler. log239: L/R onto the equip tab freezes ~1.1 s inside
 * CSWGuiManager::HandleInputEvent, and the one watchdog sample that landed in
 * a freeze found the game thread WAITING (waitType 0x10), not running. The
 * watchdog ticks every 3 s; this looks every 50 ms and, once a frame has run
 * 250 ms, logs what the game thread is doing and the name of what it waits
 * on, so the log says whose object holds it. */
static void wait_object_name(unsigned type, SceUID id, char *out, size_t n) {
  out[0] = 0;
  if (type == 0x10) {
    SceKernelEventFlagInfo i; memset(&i, 0, sizeof i); i.size = sizeof i;
    if (sceKernelGetEventFlagInfo(id, &i) >= 0) snprintf(out, n, "eventflag \"%.31s\"", i.name);
  } else if (type == 0x20) {
    SceKernelSemaInfo i; memset(&i, 0, sizeof i); i.size = sizeof i;
    if (sceKernelGetSemaInfo(id, &i) >= 0) snprintf(out, n, "sema \"%.31s\"", i.name);
  } else if (type == 0x40) {
    SceKernelMutexInfo i; memset(&i, 0, sizeof i); i.size = sizeof i;
    if (sceKernelGetMutexInfo(id, &i) >= 0) snprintf(out, n, "mutex \"%.31s\"", i.name);
  }
  if (!out[0]) snprintf(out, n, "type 0x%x id 0x%x (no name)", type, (unsigned)id);
}

static void *hitch_sampler_thread(void *arg) {
  (void)arg;
  thread_pin_self(CPU_AUX_B, "hitch-sampler");
  int budget = 600;                       /* lines, whole session */
  uint64_t seen_swap = 0;
  int n_this = 0;
  for (;;) {
    sceKernelDelayThread(50 * 1000);
    SceUID thid = g_game_thid;
    uint64_t swap = g_last_swap_end_us;
    if (thid < 0 || !swap || budget <= 0) continue;
    if (swap != seen_swap) { seen_swap = swap; n_this = 0; }
    uint64_t late = sceKernelGetProcessTimeWide() - swap;
    if (late < 250000 || n_this >= 12) continue;
    SceKernelThreadInfo ti;
    memset(&ti, 0, sizeof ti);
    ti.size = sizeof ti;
    if (sceKernelGetThreadInfo(thid, &ti) < 0) continue;
    char what[64] = "";
    if (ti.status != SCE_THREAD_RUNNING && ti.status != SCE_THREAD_READY)
      wait_object_name((unsigned)ti.waitType, ti.waitId, what, sizeof what);
    log_printf("[hitchsample] +%u ms: %s%s%s", (unsigned)(late / 1000u),
               ti.status == SCE_THREAD_RUNNING ? "running" :
               ti.status == SCE_THREAD_READY ? "ready (not scheduled)" : "WAITING",
               what[0] ? " on " : "", what);
    n_this++;
    budget--;
  }
  return NULL;
}

static void *watchdog_thread(void *arg) {
  (void)arg;
  uint64_t last_run = 0;
  thread_pin_self(CPU_AUX_B, "watchdog");
  for (;;) {
    sceKernelDelayThread(3 * 1000 * 1000);
    /* Per-thread core and load, every fourth tick (12s): which core is busy,
     * and with what. The first call only takes the baseline. */
    {
      static unsigned cpu_tick = 0;
      if (cpu_tick++ % 4 == 0) thread_census_log();
    }
    /* Heap occupancy, sampled here because this is the one thread that already
     * ticks on a fixed schedule. One line per 3s costs ~0.3ms and is what tells
     * us whether headroom drains steadily, steps down per area, or falls off a
     * cliff -- the three explanations need different fixes. */
    {
      /* Every tick for the cheap figures; every tenth for the ones that probe
         the allocator, which is often enough to watch contiguity decay without
         poking a struggling heap two hundred times a run. */
      static unsigned tick = 0;
      if (tick++ % 10 == 0) heap_log_full(NULL); else heap_log(NULL);
    }
    /* vitaGL's own heaps, which we have never measured. They are fixed at
     * vglInitExtended and hold every texture and vertex buffer the game
     * uploads, so they are a second place to run out -- and vitaGL does not
     * check its allocations, so exhaustion there is silent: the GPU reads
     * whatever is at the pointer and the geometry smears, while the GUI (small,
     * per-frame) keeps drawing correctly. log146 ended exactly like that after
     * 44 minutes in one area with the newlib heap still healthy, which is what
     * this line is here to confirm or rule out. */
    /* Held-button mask, reported only when it moves. A bit that stays set with
     * nothing held is a missed release, and the game will act as though that
     * button is held forever -- so the interesting event is a transition that
     * never comes back to 0, and printing every tick would bury it. */
    {
      static unsigned last_mask = 0;
      static int mask_seen = 0;
      unsigned m = sdl_gamepad_mask();
      if (!mask_seen || m != last_mask) {
        log_printf("[input] held-button mask: 0x%x -> 0x%x", last_mask, m);
        last_mask = m;
        mask_seen = 1;
      }
    }
    /* Input census, every fourth tick (12s). Two lines: what the hardware is
     * doing, and what the game consumed from SDL in the same window. This tells
     * us whether a dead stick originated in the pad, SDL, or the game. */
    {
      static unsigned itick = 0;
      if (itick++ % 4 == 0) { input_probe_census(); sdl_input_census(); }
    }
    /* Sound, on the same clock and for the same reason. The pipeline line is
     * short, so every fourth tick (12s); the stats line is long and used to
     * arrive every 128th createSound, so every eighth (24s) keeps its old
     * density without inheriting its habit of thinning out exactly when the
     * game goes quiet. */
    {
      static unsigned stick = 0;
      if (stick % 4 == 0) { sound_pipeline_census(); audio_log_slots(); }
      if (stick % 8 == 0) audio_log_stats();
      stick++;
    }
    log_printf("[vgl] free: vram %u/%u KB, ram %u/%u KB, phycont %u/%u KB",
               (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024u),
               (unsigned)(vglMemTotal(VGL_MEM_VRAM) / 1024u),
               (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024u),
               (unsigned)(vglMemTotal(VGL_MEM_RAM) / 1024u),
               (unsigned)(vglMemFree(VGL_MEM_SLOW) / 1024u),
               (unsigned)(vglMemTotal(VGL_MEM_SLOW) / 1024u));
    SceUID thid = g_game_thid;
    if (thid < 0)
      continue;
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    int r = sceKernelGetThreadInfo(thid, &info);
    if (r < 0) {
      log_printf("[wd] getThreadInfo(0x%x) failed 0x%08x", thid, (unsigned)r);
      continue;
    }
    uint64_t run = (uint64_t)info.runClocks;
    const char *st = info.status == SCE_THREAD_RUNNING ? "RUNNING"
                   : info.status == SCE_THREAD_READY   ? "READY"
                   : info.status == SCE_THREAD_WAITING ? "WAITING"
                   : info.status == SCE_THREAD_DORMANT ? "DORMANT"
                   : info.status == SCE_THREAD_DELETED ? "DELETED(stackoverflow?)"
                   : "?";
    log_printf("[wd] status=%s(0x%x) waitType=0x%x waitId=0x%x cpu=%d prio=%d "
               "runClk=%llu dClk=%llu %s",
               st, (unsigned)info.status, (unsigned)info.waitType,
               (unsigned)info.waitId, (int)info.currentCpuId,
               (int)info.currentPriority, (unsigned long long)run,
               (unsigned long long)(run - last_run),
               (run == last_run) ? "<< FROZEN (blocked)" : "(running)");
    last_run = run;

    // The game thread being healthy tells us nothing when the stall is on a
    // worker (log72: main thread renders the loading screen forever while all
    // I/O stops). Sweep every thread the game created and report the ones that
    // are NOT accumulating runtime -- those are the blocked ones, and `entry`
    // feeds straight into addr2line against libKOTOR.so / libandroid_port.so.
    for (int i = 0; i < g_game_threads_n && i < GAME_THREADS_MAX; i++) {
      SceUID t = g_game_threads[i].thid;
      if (t < 0 || t == thid)
        continue;
      SceKernelThreadInfo ti;
      memset(&ti, 0, sizeof(ti));
      ti.size = sizeof(ti);
      if (sceKernelGetThreadInfo(t, &ti) < 0)
        continue;
      uint64_t r2 = (uint64_t)ti.runClocks;
      uint64_t prev = g_thread_last_run[i];
      g_thread_last_run[i] = r2;
      log_printf("[wd:t%d] thid=0x%08x entry=%p status=0x%x waitType=0x%x "
                 "waitId=0x%x prio=%d runClk=%llu dClk=%llu %s",
                 i, (unsigned)t, (void *)g_game_threads[i].entry,
                 (unsigned)ti.status, (unsigned)ti.waitType, (unsigned)ti.waitId,
                 (int)ti.currentPriority, (unsigned long long)r2,
                 (unsigned long long)(r2 - prev),
                 (r2 == prev) ? "<< FROZEN (blocked)" : "(running)");
    }
  }
  return NULL;
}

// Mount the OBB game-data archives. On Android the Java layer mounts them and
// calls these natives; we have no Java, so SDL_main would spin forever waiting
// on g_obbMounted && g_patchObbMounted. Calling the game's own mountObb/
// mountPatchObb builds the ObbFile (miniz) objects into g_mainObb/g_patchObb
// AND sets the flags, so later OBB reads work (just forcing the flags would
// leave g_mainObb null -> crash). See RECON / obb-mount memory.
// Verify an OBB is actually a readable zip before we rely on it: true 64-bit
// size (sceIo) + first 4 bytes (a zip starts "PK\3\4"). mountObb sets its flag
// unconditionally even if ObbFile/miniz init fails, so a missing/bad file only
// shows up later as a null-central-directory DATA_ABORT in GetDirectoryList.
static int check_obb(const char *path) {
  SceIoStat st;
  memset(&st, 0, sizeof(st));
  if (sceIoGetstat(path, &st) < 0) {
    log_printf("    [obb] MISSING: %s", path);
    return 0;
  }
  unsigned char m[4] = {0};
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd >= 0) { sceIoRead(fd, m, 4); sceIoClose(fd); }
  int ok = (m[0] == 'P' && m[1] == 'K');
  log_printf("    [obb] %s size=%lld magic=%02x%02x%02x%02x %s", path,
             (long long)st.st_size, m[0], m[1], m[2], m[3],
             ok ? "(zip ok)" : "(NOT A ZIP!)");
  return ok;
}

// On Android these live under the app's private storage and the OS/installer
// guarantees they exist. Here nothing creates them, so the very first thing a
// New Game does -- `access("./gameinprogress/")` then `opendir(...)` -- fails.
// That pair is the last thing logged before the chargen DATA_ABORT in BOTH
// log59 and log61, at exactly the same point.
//
// Only the WRITABLE save/scratch dirs are created. Deliberately NOT created:
// override/, modules/, portraits/, movies/, errortex/ -- those are read paths
// served out of the OBB, and an empty real directory could plausibly make the
// game stop falling back to the archive.
static void ensure_writable_dirs(void) {
  static const char *dirs[] = {
    DATA_PATH "/gameinprogress",
    DATA_PATH "/currentgame",
    DATA_PATH "/saves",
    DATA_PATH "/rebootdata",
  };
  for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
    int r = sceIoMkdir(dirs[i], 0777);
    // 0x80010011 == SCE_ERROR_ERRNO_EEXIST: already there, which is fine.
    log_printf("[FS] mkdir %s -> 0x%08x%s", dirs[i], (unsigned)r,
               (r >= 0 || (unsigned)r == 0x80010011u) ? " (ok)" : " (FAILED)");
  }
}

static void mount_obbs(void) {
  void (*mountObb)(void *, void *, void *) =
      (void *)so_symbol(&kotor_mod, "Java_com_aspyr_kotor_KOTOR_mountObb");
  void (*mountPatchObb)(void *, void *, void *) =
      (void *)so_symbol(&kotor_mod, "Java_com_aspyr_kotor_KOTOR_mountPatchObb");
  void *env = jni_get_env();

  int main_ok = check_obb(OBB_MAIN_PATH);
  int patch_ok = check_obb(OBB_PATCH_PATH);
  if (!main_ok || !patch_ok)
    fatal_error("OBB archives invalid/empty at %s. Copy the real .obb game data "
                "(main ~2.1GB, patch ~453MB) there; check ux0: free space.",
                DATA_PATH);

  g_io_trace = 1;   // trace miniz's fopen/fseek/ftell/fread through the mount
  if (mountObb) {
    log_printf(">>> mountObb(\"%s\")", OBB_MAIN_PATH);
    mountObb(env, NULL, (void *)OBB_MAIN_PATH);   // fake jstring == char* path
    log_printf("<<< mountObb returned");
  } else {
    log_printf("!!! Java_com_aspyr_kotor_KOTOR_mountObb not found");
  }
  if (mountPatchObb) {
    log_printf(">>> mountPatchObb(\"%s\")", OBB_PATCH_PATH);
    mountPatchObb(env, NULL, (void *)OBB_PATCH_PATH);
    log_printf("<<< mountPatchObb returned");
  } else {
    log_printf("!!! Java_com_aspyr_kotor_KOTOR_mountPatchObb not found");
  }
  g_io_trace = 0;

  // Confirm the flags the SDL_main wait loop polls actually flipped.
  uint8_t *obb = (uint8_t *)so_symbol(&port_mod, "g_obbMounted");
  uint8_t *patch = (uint8_t *)so_symbol(&port_mod, "g_patchObbMounted");
  log_printf("    g_obbMounted=%d g_patchObbMounted=%d",
             obb ? *obb : -1, patch ? *patch : -1);

  // Did the ObbFile/miniz actually load? GetDirectoryList faulted reading
  // *(*g_patchObb + 0x68) == null. Log the ObbFile ptr and that field for both.
  void **g_main = (void **)so_symbol(&port_mod, "g_mainObb");
  void **g_patch = (void **)so_symbol(&port_mod, "g_patchObb");
  if (g_main && *g_main)
    log_printf("    g_mainObb=%p  [+0x68]=%p", *g_main, ((void **)((char *)*g_main + 0x68))[0]);
  else
    log_printf("    g_mainObb=%p (null!)", g_main ? *g_main : (void *)0);
  if (g_patch && *g_patch)
    log_printf("    g_patchObb=%p [+0x68]=%p", *g_patch, ((void **)((char *)*g_patch + 0x68))[0]);
  else
    log_printf("    g_patchObb=%p (null!)", g_patch ? *g_patch : (void *)0);

  // Now that the archives are mounted, let ordinary file opens reach them too.
  // Without this, only resource-manager reads saw the OBB and anything opened as
 // a plain file (modules/*.rim among them) failed.
  sdl_obb_fallback_init(so_symbol(&port_mod, "_ZN7ObbFile10RWFromFileEPKc"),
                        (uintptr_t)g_main, (uintptr_t)g_patch);
}

// ---- FONT FIX: per-call null-guard on CAurGUIStringInternal text methods -----
// The loadscreen lays out/draws GUI text at ~103s, but the GUI font atlas
// (d2xfont16x16b, dialogfont16x16b) doesn't load until ~111s. Both text methods --
// WrapStrings(int) (layout) and Draw(float) (render) -- fetch fontInfo the same way
// (font = *(this+0x18); virtual GetFontInfo at vtable+0x38) and deref it WITH NO
// NULL CHECK, so before the font loads they fault: WrapStrings at libKOTOR+0x42e0be
// `ldr sl,[fp,#0x24]`, Draw at +0x42ebf0 `vldr s22,[fp,#12]` (fp=fontInfo=0). An
// all-or-nothing hook can't win (an unconditional no-op kills menu text too), so we
// install per-call entry guards that reproduce the function's own fontInfo lookup
// and SKIP only when it's null, otherwise chain to the real function via a
// trampoline. Reproducing the lookup is safe: it's exactly what the game does, and
// GetFontInfo returns null cleanly (it doesn't fault) when there's no font.
//
// NOTE (still open): even at the menu fontInfo stays null -- the TXI->CAurFontInfo
// metrics parse (CAurFontInfo::ParseField) never runs, so GUI text does not yet
// render. These guards keep the app ALIVE (no crash) through the whole boot;
// getting actual text is a separate fix (populate CAurFontInfo / +0x38).
// Shared: reproduce CAurGUIStringInternal's fontInfo lookup. Returns the CAurFontInfo
// pointer (may be null == font not loaded) without ever faulting.
static void *gui_string_fontinfo(void *self) {
  if (!self) return NULL;
  void *font = *(void **)((char *)self + 0x18);       // CAurTexture* for this string
  if (!font) return NULL;
  void **vtbl = *(void ***)font;
  void *(*getFontInfo)(void *) = (void *(*)(void *))vtbl[14]; // vtable + 0x38
  return getFontInfo(font);
}

// ---- WrapStrings(int): layout -------------------------------------------------
static int (*WrapStrings_orig)(void *self, int arg) = NULL;
static int WrapStrings_noop(void *self, int arg) { (void)self; (void)arg; return 0; }

static int WrapStrings_guard(void *self, int arg) {
  if (!gui_string_fontinfo(self)) {
    static int once = 0;
    if (!once) { once = 1; log_printf("[font] WrapStrings: fontInfo null -> skip layout (no font yet)"); }
    return 0;
  }
  static int once2 = 0;
  if (!once2) { once2 = 1; log_printf("[font] WrapStrings: fontInfo live -> layout (text enabled)"); }
  return WrapStrings_orig(self, arg);
}

// ---- Draw(float): render ------------------------------------------------------
// The float arg arrives softfp (in r1); keep it opaque (uint32_t) so we never touch
// s0 and it passes straight through in r1 to the original softfp function.
static void (*Draw_orig)(void *self, uint32_t xf) = NULL;
static void Draw_noop(void *self, uint32_t xf) { (void)self; (void)xf; }

static void Draw_guard(void *self, uint32_t xf) {
  if (!gui_string_fontinfo(self)) {
    static int once = 0;
    if (!once) { once = 1; log_printf("[font] Draw: fontInfo null -> skip render (no font yet)"); }
    return;
  }
  static int once2 = 0;
  if (!once2) { once2 = 1; log_printf("[font] Draw: fontInfo live -> render (text enabled)"); }
  Draw_orig(self, xf);
}

// ---- FONT METRICS: serve bundled .txi as a MEMORY-backed resource --------------
// CAurTextureBasic::Init loads the font's glyph metrics via
// AurResGet(resref, ".txi", &size, flag=1). AurResGet checks the OBB resource index
// first; on a miss it has a filesystem fallback (sprintf -> SDL_RWFromFile), but that
// fallback is GATED on flag==0 (+0x40e490: `cmp r9,#0; beq fallback`). Init passes
// flag=1, so it never runs and the .txi is never found -- Aspyr ships the override
// font TGAs without their .txi. No metrics -> fontInfo NULL -> no GUI text at all.
//
// Simply forcing flag=0 is NOT enough, and is actively worse: see the layout table in
// config.h. The fallback builds an RWops-backed object, which sends AurResGetNextLine
// down its unbounded streaming scanner and off the end of the heap.
//
// So: let the game take its own fallback (which builds AND REGISTERS the object --
// registry insert at +0x40e410; AurResGetNextLine reads the most recently registered
// entry at +0x40e898, and AurResFree scans the same list), then convert the object in
// place to the memory-backed shape the bounded scanner expects.
#if FONT_TXI_MEMORY_INJECT
static void *(*AurResGet_orig)(char *resref, char *type, int *size, int flag) = NULL;

static int is_txi_type(const char *t) {
  return t && (!strcmp(t, ".txi") || !strcmp(t, "txi"));
}

// Field offsets of AurResGet's 28-byte resource object, as words.
enum {
  RES_RWOPS = 0,  // 0 => memory-backed (bounded scanner); non-0 => streaming
  RES_UNK1  = 1,  // OBB path sets 0xffffffff
  RES_LINE  = 2,  // line buffer; scanner allocates it lazily, sized by RES_LINECAP
  RES_DATA  = 3,  // the bytes the bounded scanner reads
  RES_SIZE  = 4,  // the bound it checks against
  RES_LINECAP = 5,
  RES_FREE  = 6,  // AurResFree hands THIS to the game's allocator
};

// Objects we converted, so AurResFree_hook can recognise them. Only a handful of
// fonts ever load, so a flat array beats any bookkeeping.
// The scanner CONSUMES the resource: at +0x40e900 it does `strd r3,r0,[r4,#12]`,
// advancing [+12] past each line and decrementing [+16]. So [+12] is NOT the
// allocation base by the time anything frees it -- keep our own copy, or free()
// reads a chunk header out of the file's own text (log44 died exactly that way).
#define TXI_INJECT_MAX 8
static void *txi_injected[TXI_INJECT_MAX];
static void *txi_injected_base[TXI_INJECT_MAX];
static int   txi_injected_n = 0;

// A stand-in for the SDL_RWops that normally sits at [+0]. AurResFree only ever
// touches offset +16 (rwops->close), so that is the only slot we populate. Its
// purpose is to steer AurResFree away from the allocator -- see AurResFree_hook.
static uintptr_t txi_dummy_rwops[8];
static int txi_dummy_close(void *ctx) { (void)ctx; return 0; }

// Chargen crashes because animBase->GetModel(255) (vtable slot 3 -- proven via
// CSWCObject::GetModel, which is `r0=this[0x68]; r2=vptr[12]; bx r2`) returns
// NULL: the class-selection creatures have no model. No .mdl/.mdx ever reaches
// the FS layer because models live inside the OBB's data/*.bzf archives, so the
// only place to see the failure is the resource gateway itself. Log the misses.
// (An SDL_RWFromFile MISS is NOT a failure -- ios_mm_new_en.tga misses too and
// still draws; the game falls back to the OBB. A NULL from AurResGet is real.)
static unsigned g_res_miss_n = 0;
#define RES_MISS_LOG_MAX 300

// Model resources go through here too. Log the object AurResGet just built so the
// provider's answer (res[12]/res[16], set from AurGetResource + its out-size) can
// be compared against the archive's real entry sizes, which we know exactly:
// pmbbs.mdl is 192904 unpacked / 86608 packed in player.bzf, gui3D_room.mdl 1553 /
// 530 in models.bzf. A short or zero res[16] means the provider failed and the
// header check is reading a buffer nobody filled.
static int is_model_type(const char *t) {
  return t && (!strcmp(t, ".mdl") || !strcmp(t, ".mdx") ||
               !strcmp(t, "mdl")  || !strcmp(t, "mdx"));
}
static unsigned g_resget_mdl_n = 0;

static void *AurResGet_hook(char *resref, char *type, int *size, int flag) {
  void *r = AurResGet_orig(resref, type, size, flag);

  if (is_model_type(type) && g_resget_mdl_n < 48) {
    const uint32_t *o = (const uint32_t *)r;
    log_printf("[model] AurResGet(\"%.16s\",\"%.4s\",flag=%d) -> %p%s",
               resref ? resref : "?", type, flag, r,
               !r ? "  <<< NULL" : "");
    if (r)
      log_printf("[model]   res[0]=%08x [3]=%08x m8=%u [4]=%d [5]=%d [6]=%08x",
                 o[0], o[3], (unsigned)(o[3] & 7u), (int)o[4], (int)o[5], o[6]);
    g_resget_mdl_n++;
  }

  if (!r) {
    if (g_res_miss_n < RES_MISS_LOG_MAX)
      log_printf("[res] MISS #%u \"%.16s\" type=\"%.8s\" flag=%d",
                 g_res_miss_n, resref ? resref : "?", type ? type : "?", flag);
    g_res_miss_n++;
  }

  if (r || !is_txi_type(type)) return r;

  // A .txi missed the OBB index. Re-run with the fallback enabled so the game
  // builds + registers the object and calls SDL_RWFromFile -> SDL_RWFromFile_hook
  // -> our VPK-bundled app0:fonts/<resref>.txi.
  r = AurResGet_orig(resref, type, size, 0);
  if (!r) return NULL;

  uint32_t *o = (uint32_t *)r;
  unsigned int len = 0;
  void *buf = sdl_slurp_rwops_close((void *)o[RES_RWOPS], &len);

  o[RES_RWOPS]   = 0;    // memory-backed -> take the BOUNDED scanner
  o[RES_UNK1]    = 0xffffffff;
  o[RES_LINE]    = 0;
  o[RES_DATA]    = (uint32_t)buf;
  o[RES_SIZE]    = len;  // 0 on failure -> scanner returns NULL at +0x40e890, no fault
  o[RES_LINECAP] = 8192; // what the OBB path uses; 500000 was the streaming buffer
  o[RES_FREE]    = 0;    // leak `buf` rather than hand a foreign pointer to the
                         // game's allocator -- a few KB, once, per font
  if (size) *size = (int)len;  // the fallback path writes a literal 1 here

  if (buf && txi_injected_n < TXI_INJECT_MAX) {
    txi_injected[txi_injected_n]      = r;
    txi_injected_base[txi_injected_n] = buf;  // the allocation base, not [+12]
    txi_injected_n++;
  }

  // Log every success, not just the first: only the handful of fonts we bundle can
  // get this far (texture .txi requests miss at SDL_RWFromFile and leave buf NULL),
  // and knowing WHICH fonts were served is what identified the two missing ones.
  if (buf)
    log_printf("[font] .txi injected as memory resource: %s len=%u", resref ? resref : "?", len);
  return r;
}

// AurResFree(+0x40e740) branches on [+0] exactly like AurResGetNextLine does:
//
//   [+0]!=0 -> ((SDL_RWops*)[+0])->close(), clear [+0], then registry removal
//   [+0]==0 -> CAuroraInterface::ReleaseResource([+24]) , then registry removal
//
// ReleaseResource is NOT null-safe: its first act is `ldrh r1,[r0,#-6]` (+0x4b0fcc),
// reading a 2-byte type tag out of an inline block header. Our [+24]=0 therefore
// faulted at 0xfffffffa (log43, straight after a successful parse). We cannot point
// [+24] at our malloc'd buffer either -- ReleaseResource would then hand a foreign
// pointer to the game's allocator.
//
// So at free time we put a dummy RWops back at [+0]. AurResFree takes the close
// branch, calls our no-op, and skips the allocator entirely -- while still doing the
// registry removal that keeps the game's bookkeeping correct.
static void (*AurResFree_orig)(void *res, int i) = NULL;
static void AurResFree_hook(void *res, int i) {
  for (int k = 0; k < txi_injected_n; k++) {
    if (txi_injected[k] != res) continue;

    uint32_t *o = (uint32_t *)res;
    free(txi_injected_base[k]);  // the base we recorded -- NEVER o[RES_DATA]
    o[RES_DATA]  = 0;
    o[RES_SIZE]  = 0;
    o[RES_FREE]  = 0;
    o[RES_RWOPS] = (uint32_t)&txi_dummy_rwops;  // -> close branch, no ReleaseResource

    txi_injected_n--;
    txi_injected[k]      = txi_injected[txi_injected_n];
    txi_injected_base[k] = txi_injected_base[txi_injected_n];
    break;
  }
  AurResFree_orig(res, i);
}

static void install_aurresget_hook(void) {
  txi_dummy_rwops[4] = (uintptr_t)&txi_dummy_close;  // +16 == rwops->close

  uintptr_t f = so_symbol(&kotor_mod, "_Z10AurResFreePvi");
  if (f) {
    AurResFree_orig = (void (*)(void *, int))build_thumb_trampoline(f, thumb_patch_len(f));
    if (AurResFree_orig) {
      hook_thumb(f, (uintptr_t)&AurResFree_hook);
      log_printf("[font] AurResFree HOOKED: f=0x%08x tramp=%p (skip ReleaseResource for injected .txi)",
                 (unsigned)f, (void *)AurResFree_orig);
    }
  }
  // Without the free hook the injection below would fault on teardown, so don't arm it.
  if (!AurResFree_orig) {
    log_printf("[font] AurResFree hook FAILED -- .txi injection NOT installed (would crash on free)");
    return;
  }

  uintptr_t a = so_symbol(&kotor_mod, "_Z9AurResGetPcS_Pib");
  if (!a) { log_printf("[font] AurResGet symbol missing -- .txi injection NOT installed"); return; }
  AurResGet_orig = (void *(*)(char *, char *, int *, int))build_thumb_trampoline(a, thumb_patch_len(a));
  if (AurResGet_orig) {
    hook_thumb(a, (uintptr_t)&AurResGet_hook);
    log_printf("[font] AurResGet HOOKED: a=0x%08x tramp=%p (.txi -> memory resource)", (unsigned)a, (void *)AurResGet_orig);
  } else {
    log_printf("[font] AurResGet trampoline FAILED -- .txi injection NOT installed");
  }
}
#endif  // FONT_TXI_MEMORY_INJECT

// ---- GUI IMAGE PIPELINE probe ----------------------------------------------
/* KOTOR uses AurGUISetupViewport/AurGUICloseViewport as a nested GUI clipping
 * stack, but glViewport is only a coordinate transform. Mirror this semantic
 * GUI boundary to scissor while preserving any caller-owned scissor state. */
#define GUI_VIEWPORT_STACK_MAX 16
typedef struct {
  GLboolean enabled;
  GLint box[4];
} GuiScissorState;

static int (*AurGUISetupViewport_orig)(int x, int y, int w, int h,
                                       const void *color, uint32_t clear,
                                       uint32_t alpha) = NULL;
static void (*AurGUICloseViewport_orig)(void) = NULL;
static GuiScissorState g_gui_scissor_stack[GUI_VIEWPORT_STACK_MAX];
static unsigned g_gui_scissor_depth = 0;
static unsigned g_gui_scissor_bypass_depth = 0;

static uintptr_t *find_jump_slot(so_module *mod, const char *name) {
  for (int i = 0; i < mod->num_relplt; i++) {
    Elf32_Rel *rel = &mod->relplt[i];
    if (ELF32_R_TYPE(rel->r_info) != R_ARM_JUMP_SLOT) continue;
    Elf32_Sym *sym = &mod->dynsym[ELF32_R_SYM(rel->r_info)];
    if (strcmp(mod->dynstr + sym->st_name, name) != 0) continue;
    return (uintptr_t *)(mod->text_base + rel->r_offset);
  }
  return NULL;
}

static void gui_scissor_restore(const GuiScissorState *state) {
  glScissor(state->box[0], state->box[1], state->box[2], state->box[3]);
  if (state->enabled) glEnable(GL_SCISSOR_TEST);
  else                glDisable(GL_SCISSOR_TEST);
}

static int AurGUISetupViewport_scissor(int x, int y, int w, int h,
                                       const void *color, uint32_t clear,
                                       uint32_t alpha) {
  if (g_gui_scissor_depth >= GUI_VIEWPORT_STACK_MAX) {
    log_printf("[gui:viewport] stack overflow at depth=%u", g_gui_scissor_depth);
    int rc = AurGUISetupViewport_orig(x, y, w, h, color, clear, alpha);
    if (rc) g_gui_scissor_bypass_depth++;
    return rc;
  }

  GuiScissorState *state = &g_gui_scissor_stack[g_gui_scissor_depth++];
  state->enabled = glIsEnabled(GL_SCISSOR_TEST);
  glGetIntegerv(GL_SCISSOR_BOX, state->box);
  g_gl_gui_viewport_scope++;
  int rc = AurGUISetupViewport_orig(x, y, w, h, color, clear, alpha);
  if (!rc) {
    g_gl_gui_viewport_scope--;
    g_gui_scissor_depth--;
    gui_scissor_restore(state);
  }
  return rc;
}

static void AurGUICloseViewport_scissor(void) {
  if (g_gui_scissor_bypass_depth) {
    AurGUICloseViewport_orig();
    g_gui_scissor_bypass_depth--;
    return;
  }
  if (!g_gui_scissor_depth) {
    AurGUICloseViewport_orig();
    return;
  }

  GuiScissorState state = g_gui_scissor_stack[g_gui_scissor_depth - 1];
  AurGUICloseViewport_orig();
  g_gl_gui_viewport_scope--;
  g_gui_scissor_depth--;
  gui_scissor_restore(&state);
}

/* Time spent building models and reading them out of the archive, for the
 * [hitch] line: the menu-tab freezes read no card data and upload no textures,
 * and the open question is whether the equipment screen's character preview
 * being rebuilt is where that second goes. Only the game thread is timed, and
 * the depth counts keep nested calls from being timed twice. */
static unsigned g_model_calls, g_model_read_calls, g_model_depth, g_read_depth;
static uint64_t g_model_us, g_model_read_us;

static void *(*ReadSync_orig)(void *self, char *name) = NULL;

static void *ReadSync_probe(void *self, char *name) {
  int timed = sceKernelGetThreadId() == g_game_thid;
  uint64_t t0 = (timed && !g_read_depth++) ? sceKernelGetProcessTimeWide() : 0;
  void *r = ReadSync_orig(self, name);
  if (timed && --g_read_depth == 0) {
    g_model_read_us += sceKernelGetProcessTimeWide() - t0;
    g_model_read_calls++;
  }
  return r;
}

/* LzmaUncompress is the OBB resource decompressor (libandroid_port imports it
 * from libLzmaLib). Hooked so repeat decodes are answered from lzma_cache. */
static int (*LzmaUncompress_orig)(unsigned char *, size_t *, const unsigned char *,
                                  size_t *, const unsigned char *, size_t) = NULL;

static int LzmaUncompress_probe(unsigned char *dest, size_t *destLen,
                                const unsigned char *src, size_t *srcLen,
                                const unsigned char *props, size_t propsSize) {
#if LZMA_CACHE_KB
  size_t dl_in = destLen ? *destLen : 0;
  size_t sl_in = srcLen ? *srcLen : 0;
  uint64_t key = 0;
  if (lzma_cache_get(&key, dest, destLen, src, srcLen, props, propsSize))
    return 0;                                   /* SZ_OK, from the cache */
  uint64_t t0 = sceKernelGetProcessTimeWide();
#endif
  int rc = LzmaUncompress_orig(dest, destLen, src, srcLen, props, propsSize);
#if LZMA_CACHE_KB
  lzma_cache_note_miss_us(sceKernelGetProcessTimeWide() - t0);
  if (rc == 0 && key)
    lzma_cache_put(key, dl_in, sl_in, dest, *destLen, *srcLen);
#endif
  return rc;
}

static void install_lzma_probe(void) {
#if LZMA_CACHE_KB
  lzma_cache_init((size_t)LZMA_CACHE_KB * 1024u);
#endif
  uintptr_t lu = so_symbol(&lzma_mod, "LzmaUncompress");
  if (!lu) { log_printf("[lzma] LzmaUncompress symbol MISSING in libLzmaLib"); return; }
  LzmaUncompress_orig = (int (*)(unsigned char *, size_t *, const unsigned char *,
                                 size_t *, const unsigned char *, size_t))
      build_thumb_trampoline(lu, thumb_patch_len(lu));
  if (LzmaUncompress_orig) hook_thumb(lu, (uintptr_t)&LzmaUncompress_probe);
  else log_printf("[lzma] LzmaUncompress trampoline FAILED");
}

/* NewCAurObject instantiates every model by name; timed for the [hitch] line. */
static void *(*NewCAurObject_orig)(char *name, char *type, void *rw1, void *rw2) = NULL;

static void *NewCAurObject_probe(char *name, char *type, void *rw1, void *rw2) {
  int timed = sceKernelGetThreadId() == g_game_thid;
  uint64_t t0 = (timed && !g_model_depth++) ? sceKernelGetProcessTimeWide() : 0;
  void *r = NewCAurObject_orig(name, type, rw1, rw2);
  if (timed && --g_model_depth == 0) {
    g_model_us += sceKernelGetProcessTimeWide() - t0;
    g_model_calls++;
  }
  return r;
}

static void hook_named(const char *sym, uintptr_t probe, void **orig, const char *tag);

static void install_model_timers(void) {
  hook_named("_Z13NewCAurObjectPcS_P9SDL_RWopsS1_", (uintptr_t)&NewCAurObject_probe,
             (void **)&NewCAurObject_orig, "NewCAurObject");
  hook_named("_ZN12IODispatcher8ReadSyncEPc", (uintptr_t)&ReadSync_probe,
             (void **)&ReadSync_orig, "IODispatcher::ReadSync");
}

static void hook_named(const char *sym, uintptr_t probe, void **orig, const char *tag) {
  uintptr_t a = so_symbol(&kotor_mod, sym);
  if (!a) { log_printf("[load] %s symbol missing", tag); return; }
  *orig = (void *)build_thumb_trampoline(a, thumb_patch_len(a));
  if (!*orig) { log_printf("[load] %s trampoline FAILED", tag); return; }
  hook_thumb(a, probe);
}

/* GameUpdate and UpdateScreen are timed for the [hitch] line, and GameUpdate
 * is where the adaptive render skip is overridden (DISABLE_ADAPTIVE_RENDER_SKIP).
 * ABI: UpdateScreen's first parameter is a float and the .so is softfp, so it
 * arrives in r0, not s0. */
static void *(*UpdateScreen_orig)(uint32_t a, int b, int c) = NULL;
static void *(*GameUpdate_orig)(void) = NULL;
static unsigned g_us_n = 0, g_gu_n = 0;
static uint64_t g_us_time = 0, g_gu_time = 0;
static uint64_t g_us_active = 0, g_gu_active = 0;
static volatile float *g_ai_update_time = NULL, *g_display_fps = NULL;
static volatile int *g_movie_fps = NULL, *g_render_skip = NULL;
static unsigned g_policy_seq = 0, g_selected_skip = 0;
/* Most catch-up updates SDL_main may run before presenting, from swkotor.ini
 * [Vita Options] CatchUpUpdates (read_vita_options). 0 = none, the measured
 * default. Raising it gives the simulation back the steps a slow frame owes
 * it, at a frame-time cost -- the knob for players who see characters jump
 * position in combat. Only meaningful with DISABLE_ADAPTIVE_RENDER_SKIP. */
static int g_catchup_cap = 0;
static float g_selector_ai_ms = 0.0f, g_last_ai_ms = 0.0f;
static int g_new_present_group = 1;

void engine_perf_snapshot(engine_perf_t *out, uint64_t now_us) {
  if (!out) return;
  out->game_calls = g_gu_n;
  out->game_us = g_gu_time + (g_gu_active ? now_us - g_gu_active : 0);
  out->screen_calls = g_us_n;
  out->screen_us = g_us_time + (g_us_active ? now_us - g_us_active : 0);
  out->policy_seq = g_policy_seq;
  out->selected_skip = g_selected_skip;
  out->selector_ai_ms = g_selector_ai_ms;
  out->next_ai_ms = g_last_ai_ms;
  out->display_fps = g_display_fps ? *g_display_fps : -1.0f;
  out->movie_fps = g_movie_fps ? *g_movie_fps : -1;
  out->model_calls = g_model_calls;
  out->model_us = g_model_us;
  out->model_read_calls = g_model_read_calls;
  out->model_read_us = g_model_read_us;
}

void engine_perf_presented(void) {
  g_new_present_group = 1;
}

static void *UpdateScreen_probe(uint32_t a, int b, int c) {
  g_us_n++;
  uint64_t start = sceKernelGetProcessTimeWide();
  g_us_active = start;
  void *rc = UpdateScreen_orig(a, b, c);
  g_us_time += sceKernelGetProcessTimeWide() - start;
  g_us_active = 0;
  return rc;
}

static void *GameUpdate_probe(void) {
  if (g_new_present_group) {
    g_selector_ai_ms = g_last_ai_ms;
    g_selected_skip = g_render_skip ? (unsigned)*g_render_skip : 0;
    g_policy_seq++;
    g_new_present_group = 0;
  }
#if DISABLE_ADAPTIVE_RENDER_SKIP
  // SDL_main has already chosen the skip count and is about to run the primary
  // update. Clearing it here makes that update render and lets SDL_main present
  // it, instead of following it with up to ten no-present update iterations.
  if (g_render_skip && *g_render_skip > g_catchup_cap) *g_render_skip = g_catchup_cap;
#endif
  g_gu_n++;
  uint64_t start = sceKernelGetProcessTimeWide();
  g_gu_active = start;
  void *rc = GameUpdate_orig();
  g_gu_time += sceKernelGetProcessTimeWide() - start;
  g_gu_active = 0;
  if (g_ai_update_time) g_last_ai_ms = *g_ai_update_time;
  gameprof_after_update();
  return rc;
}

/* --- sound pipeline ----------------------------------------------------------
 * Every probe passes the callee's return value through: a void-declared hook on
 * a value-returning function was itself a crash once. */
static void *(*SndDemand_orig)(void *) = NULL;
static void *(*StreamInit_orig)(void *) = NULL;
static void *(*FmodCreateSound_orig)(void *, char *, int, void *, unsigned, int, int) = NULL;
static void *(*FmodCreateStream_orig)(void *, char *, void *, int, int, int, int, int) = NULL;
static void *(*FmodPlaySound_orig)(void *, int) = NULL;

// Upstream of Demand: does the game ever ASK for a sound at all? If these two
// stay silent, nothing below them can ever fire and the gate is higher than the
// sound system. CResRef is a fixed char[16], not necessarily NUL-terminated.
static void *(*SndSrcCtor_orig)(void *, const void *) = NULL;
static void *(*SndSrcPlay_orig)(void *) = NULL;

/* The sound pipeline, counted end to end.
 *
 * Every one of these probes printed its first 40 calls and then went silent --
 * which in log170-172 was around t=50 s, i.e. before anything interesting had
 * happened. So when the game stopped playing sounds at t=1030 in log172 the
 * log could say that OUR playSound had gone to zero, and nothing whatever about
 * why: the four layers above it had stopped reporting sixteen minutes earlier.
 *
 * Counting is free, so count always and keep the printing capped. The layers
 * are, top to bottom:
 *
 *   CExoSoundSource::Play   the game deciding to play something
 *   SoundSource::Demand     resolving the asset (NULL m_pRes = cannot proceed)
 *   FMod::CreateSound       building the FMOD Sound   -> our createSound
 *   FMod::PlaySound         asking FMOD for a voice   -> our playSound
 *
 * The gap between any two adjacent rows names the layer that stopped. The one
 * that matters most is the last: if the game's PlaySound count runs ahead of
 * the count that reached our mixer, the game is refusing itself for want of a
 * free voice -- it has 24 2D + 16 3D of them -- and no counter on our side of
 * the boundary can see that. If they track each other, the wedge is higher up. */
static unsigned g_src_ctor = 0, g_src_play = 0, g_src_play_noint = 0;
static unsigned g_demand = 0, g_demand_nores = 0, g_demand_fail = 0;
static unsigned g_streaminit = 0;
static unsigned g_fmod_create = 0, g_fmod_createstream = 0;
static unsigned g_fmod_play = 0, g_fmod_play_null = 0;

static void sound_pipeline_census(void) {
  log_printf("[snd?] pipeline: %u SoundSource ctor, %u Play (%u no internal), "
             "%u Demand (%u no CRes / %u failed), %u StreamInit, "
             "%u CreateSound + %u CreateStream, %u PlaySound (%u returned null) "
             "-> %u reached the mixer  [gap %d]",
             g_src_ctor, g_src_play, g_src_play_noint,
             g_demand, g_demand_nores, g_demand_fail,
             g_streaminit,
             g_fmod_create, g_fmod_createstream,
             g_fmod_play, g_fmod_play_null, audio_play_count(),
             (int)g_fmod_play - (int)audio_play_count());
}

static void *SndSrcCtor_probe(void *self, const void *resref) {
  g_src_ctor++;
  return SndSrcCtor_orig(self, resref);
}
/* Where a slow sound start spends its time. log201's ambient-sound stutters
 * (CSWCSoundObject::AIUpdate peaking at 160-290 ms, one 90-240 KB OBB read each,
 * no decode logged) happen inside CExoSoundSource::Play; this splits that into
 * the resource load (Demand) and our createSound, and names the asset. Game
 * thread only, so plain globals. */
static uint64_t g_play_demand_us, g_play_create_us;
static char g_play_create_name[48];
static unsigned g_play_slow_n;

static void *SndSrcPlay_probe(void *self) {
  void *internal = self ? *(void **)((char *)self + 4) : NULL;  // m_pInternal
  g_src_play++; if (!internal) g_src_play_noint++;
  g_play_demand_us = g_play_create_us = 0;
  g_play_create_name[0] = 0;
  uint64_t t0 = sceKernelGetProcessTimeWide();
  void *rc = SndSrcPlay_orig(self);
  uint64_t dt = sceKernelGetProcessTimeWide() - t0;
  if (dt >= 25000) {
    g_play_slow_n++;
    log_printf("[snd] slow Play #%u: %u ms (Demand %u ms, CreateSound %u ms \"%.40s\")",
               g_play_slow_n, (unsigned)(dt / 1000), (unsigned)(g_play_demand_us / 1000),
               (unsigned)(g_play_create_us / 1000),
               g_play_create_name[0] ? g_play_create_name : "-");
  }
  return rc;
}

static void *SndDemand_probe(void *self) {
  void *res = self ? *(void **)((char *)self + 8) : NULL;   // m_pRes: NULL == early bail
  uint64_t t0 = sceKernelGetProcessTimeWide();
  void *rc = SndDemand_orig(self);
  g_play_demand_us += sceKernelGetProcessTimeWide() - t0;
  g_demand++; if (!res) g_demand_nores++; if (!rc) g_demand_fail++;
  return rc;
}
/* InitializeSource is void: r0 on return is its stack-guard scratch, always 0,
 * so there is no success flag to count (log198-200 read every call as failed). */
static void *StreamInit_probe(void *self) {
  void *rc = StreamInit_orig(self);
  g_streaminit++;
  return rc;
}
static unsigned g_nclose = 0, g_nrelease = 0;   /* stream/sound teardown counts */

static void *FmodCreateSound_probe(void *self, char *name, int id, void *data,
                                   unsigned size, int e, int f) {
  g_fmod_create++;
  unsigned previous_id = audio_sfx_context_push((unsigned)id);
  audio_sfx_context_name(name);
  uint64_t t0 = sceKernelGetProcessTimeWide();
  void *rc = FmodCreateSound_orig(self, name, id, data, size, e, f);
  g_play_create_us += sceKernelGetProcessTimeWide() - t0;
  if (name) {
    strncpy(g_play_create_name, name, sizeof g_play_create_name - 1);
    g_play_create_name[sizeof g_play_create_name - 1] = 0;
  }
  audio_sfx_context_name(NULL);
  audio_sfx_context_pop(previous_id);
  return rc;
}
/* Churn detector.
 *
 * log167: from t=808 the game created the SAME music stream about 7.5 times a
 * second for the rest of the session -- 1255 KB pulled out of the OBB each time,
 * never played, released 23 ms later -- and that is what took Lower City from
 * 38 fps to 1.5. The per-call log above had spent its 40-line budget by t=100,
 * so the one thing the log could not tell us was WHICH asset was storming; the
 * name had to be recovered afterwards by matching byte counts against the OBB.
 *
 * This costs a strcmp per CreateStream and prints nothing during normal play:
 * repeats of the same name only get reported at 16, 64, 256, ... and a run is
 * summarised when it ends. Unbudgeted on purpose -- a storm that starts in hour
 * two must still be named. */
static void createstream_churn(const char *name) {
  static char last[96];
  static unsigned run = 0;
  static unsigned next = 16;

  if (run && !strncmp(last, name, sizeof last - 1)) {
    if (++run >= next) {
      log_printf("[snd] CreateStream CHURN: \"%s\" x%u back to back "
                 "-- the game is re-creating this and not keeping it", last, run);
      next *= 4;
    }
    return;
  }
  if (run >= 16)
    log_printf("[snd] CreateStream churn ended: \"%s\" was created %u times in a row",
               last, run);
  snprintf(last, sizeof last, "%s", name ? name : "?");
  run  = 1;
  next = 16;
}

static void *FmodCreateStream_probe(void *self, char *name, void *rw, int c, int d,
                                    int e, int f, int g) {
  g_fmod_createstream++;
  createstream_churn(name ? name : "?");
  return FmodCreateStream_orig(self, name, rw, c, d, e, f, g);
}
static void *(*FmodCloseStream_orig)(void *, unsigned) = NULL;
static void *(*FmodReleaseSound_orig)(void *, int) = NULL;

static void *FmodCloseStream_probe(void *self, unsigned h) {
  void *rc = FmodCloseStream_orig(self, h);
  g_nclose++;
  return rc;
}
static void *FmodReleaseSound_probe(void *self, int id) {
  void *rc = FmodReleaseSound_orig(self, id);
  g_nrelease++;
  return rc;
}

/* The FModAudioSystem is only reachable as `this`, so the slot table is handed
 * to audio_patch on the first call that carries it. */
static void slots_attach(void *self) {
  static int done = 0;
  if (done || !self) return;
  done = 1;
  void (*reset)(void *) =
      (void (*)(void *))so_symbol(&port_mod, "_ZN26FModAudioSystemChannelInfo5ResetEv");
  if (!reset) log_printf("[snd] ChannelInfo::Reset symbol missing -- no slot reclaim");
  audio_slots_attach(self, reset);
}

static void *(*FmodPlayStream_orig)(void *, unsigned, int) = NULL;
static void *(*FmodStopChannel_orig)(void *, unsigned) = NULL;

static void *FmodPlayStream_probe(void *self, unsigned h, int paused) {
  slots_attach(self);
  audio_slots_ensure_free();
  return FmodPlayStream_orig(self, h, paused);
}
static void *FmodStopChannel_probe(void *self, unsigned key) {
  slots_attach(self);
  audio_slot_stop_begin(key);
  void *rc = FmodStopChannel_orig(self, key);
  audio_slot_stop_end();
  return rc;
}

static void *FmodPlaySound_probe(void *self, int id) {
  slots_attach(self);
  audio_slots_ensure_free();
  void *rc = FmodPlaySound_orig(self, id);
  g_fmod_play++; if (!rc) g_fmod_play_null++;
  return rc;
}

// hook_named() resolves against libKOTOR; the FModAudioSystem methods live in the
// companion, so the same dance against port_mod.
static void hook_named_port(const char *sym, uintptr_t probe, void **orig,
                            const char *tag) {
  uintptr_t a = so_symbol(&port_mod, sym);
  if (!a) { log_printf("[snd?] %s symbol missing", tag); return; }
  *orig = (void *)build_thumb_trampoline(a, thumb_patch_len(a));
  if (!*orig) { log_printf("[snd?] %s trampoline FAILED", tag); return; }
  hook_thumb(a, probe);
}

// Read an ini into `buf` as NUL-terminated text. Returns the byte count, or a
// negative sceIo error. The file is a few KB of settings, so it is read whole
// rather than streamed; a longer one is truncated at the buffer, which only
// costs us keys past the cut.
static int slurp_ini(const char *path, char *buf, int size) {
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0) return fd;
  int n = sceIoRead(fd, buf, size - 1);
  sceIoClose(fd);
  if (n < 0) return n;
  buf[n] = '\0';
  return n;
}

// The two spellings seen on real cards. ux0 is case-insensitive, but which one
// the game creates has varied between installs, so both are tried and the
// first that opens wins.
static const char *const kIniPaths[] = {
  "ux0:data/kotor/swkotor.ini",
  "ux0:data/kotor/swKotor.ini",
};
#define INI_PATH_COUNT ((int)(sizeof(kIniPaths) / sizeof(kIniPaths[0])))

// Resolve [Game Options] Language and hand it to the JNI layer, which is where
// the game will come looking for it (ASLPlat_GetCurrentLanguage). Absent file,
// absent key and unrecognised code all mean English -- the same thing the
// engine falls back to for an out-of-range id, so no path here can leave the
// game hunting for resources that are not in the OBB.
// What resolve_language() worked out, kept for the picker: which file to write
// back to, what the game is currently being told, and whether that came from a
// real key or from the English default -- which is what decides whether a
// first-time user gets asked at all.
static const char *g_ini_path     = NULL;   // NULL until an ini is found
static int         g_lang_id      = INI_LANG_EN;
static int         g_lang_have_key = 0;

int loader_language(void) { return g_lang_id; }

// A fan translation (translation.h) is saved next to Language as
// [Game Options] Translation=<folder>. It wins over Language, because it only
// works by standing in for the English table: the game is told English and
// asks for tv_dialog.tlk, which the translation then serves. A folder that has
// since gone from the card falls back to Language rather than to nothing.
static void resolve_translation(const char *ini) {
  char name[80];
  if (!ini_get(ini, "Game Options", "Translation", name, sizeof(name)) || !name[0])
    return;
  int i = translation_find(name);
  if (i < 0) {
    log_printf("[lang] [Game Options] Translation=%s, but there is no such folder "
               "with a dialog table in ux0:data/kotor/translations/ -- ignoring it",
               name);
    return;
  }
  translation_select(i);
  g_lang_id = INI_LANG_EN;
  g_lang_have_key = 1;
  jni_set_language(INI_LANG_EN);
  log_printf("[lang] [Game Options] Translation=%s -> the game runs as English "
             "with that folder's text", name);
}

static void resolve_language(void) {
  char buf[4097];
  translation_scan();
  for (int i = 0; i < INI_PATH_COUNT; i++) {
    if (slurp_ini(kIniPaths[i], buf, sizeof(buf)) <= 0) continue;
    g_ini_path = kIniPaths[i];
    char code[16];
    if (!ini_get(buf, "Game Options", "Language", code, sizeof(code))) {
      resolve_translation(buf);
      if (g_lang_have_key) return;
      break;
    }
    int id = ini_language_id(code);
    log_printf("[lang] %s: [Game Options] Language=%s -> id %d%s",
               kIniPaths[i], code, id,
               (id == INI_LANG_EN && strcmp(code, "en") != 0)
                 ? "  (unrecognised, using English)" : "");
    g_lang_id = id;
    g_lang_have_key = 1;
    jni_set_language(id);
    resolve_translation(buf);
    return;
  }
  log_printf("[lang] no [Game Options] Language key -> id %d (English)",
             INI_LANG_EN);
}

// Anything bigger than this is not a settings file, and rewriting whatever it
// actually is would be worse than not saving the language.
#define INI_MAX_BYTES (256 * 1024)

// Read the whole ini, or NULL when there is nothing readable to build on --
// which ini_set treats as "create the file", so a missing ini is not an error
// here. slurp_ini's fixed 4 KB is right for reading one key and wrong for a
// rewrite: truncating at 4 KB and writing that back would delete settings.
static char *read_whole_ini(const char *path) {
  SceIoStat st;
  memset(&st, 0, sizeof(st));
  if (sceIoGetstat(path, &st) < 0) return NULL;
  if (st.st_size <= 0 || st.st_size >= INI_MAX_BYTES) {
    if (st.st_size >= INI_MAX_BYTES)
      log_printf("[lang] %s is %lld bytes -- refusing to rewrite it",
                 path, (long long)st.st_size);
    return NULL;
  }
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0) return NULL;
  char *text = malloc((size_t)st.st_size + 1);
  int n = text ? sceIoRead(fd, text, (unsigned)st.st_size) : -1;
  sceIoClose(fd);
  if (n < 0) { free(text); return NULL; }
  text[n] = '\0';
  return text;
}

// [Vita Options] CatchUpUpdates=<0..10>: see g_catchup_cap. Also ObbCacheKB. Read whole rather
// than through slurp_ini's 4 KB, since a section added at the end of a long
// ini is exactly where a player would put it.
static void read_vita_options(void) {
  int cache_kb = OBB_READ_CACHE_KB;
  for (int i = 0; i < INI_PATH_COUNT; i++) {
    char *text = read_whole_ini(kIniPaths[i]);
    if (!text) continue;
    char v[16];
    if (ini_get(text, "Vita Options", "CatchUpUpdates", v, sizeof(v)) && v[0]) {
      int n = atoi(v);
      g_catchup_cap = n < 0 ? 0 : (n > 10 ? 10 : n);
    }
    // [Vita Options] ObbCacheKB: RAM for recent archive reads (obb_cache.h).
    if (ini_get(text, "Vita Options", "ObbCacheKB", v, sizeof(v)) && v[0]) {
      int n = atoi(v);
      cache_kb = n < 0 ? 0 : (n > 32 * 1024 ? 32 * 1024 : n);
    }
    free(text);
    break;
  }
  obb_cache_init((size_t)cache_kb);
  log_printf("[perf] [Vita Options] CatchUpUpdates=%d (%s)", g_catchup_cap,
             g_catchup_cap ? "slow frames may run catch-up updates"
                           : "every update presents");
}

// Replace the ini with `need` bytes of `out`, via a temp file: an interrupted
// write then costs the new setting and never the file that was already there.
static int ini_replace_file(const char *path, const char *out, size_t need) {
  char tmp[256];
  snprintf(tmp, sizeof(tmp), "%s.new", path);
  SceUID fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  int wrote = (fd >= 0) ? sceIoWrite(fd, out, (unsigned)need) : -1;
  if (fd >= 0) sceIoClose(fd);

  if (wrote != (int)need) {
    log_printf("[ini] could not write %s (%d of %u bytes) -- %s unchanged",
               tmp, wrote, (unsigned)need, path);
    sceIoRemove(tmp);
    return 0;
  }

  sceIoRemove(path);                      // FAT will not rename onto a live name
  int r = sceIoRename(tmp, path);
  if (r < 0) {
    log_printf("!!! [ini] wrote %s but could not rename it to %s (0x%08x)",
               tmp, path, (unsigned)r);
    return 0;
  }
  return 1;
}

// Undo a 2D3D Bias the engine saved while strtod was broken.
//
// The engine scales every positional sound by this bias when it is below 1
// (and every flat one by 2 - bias when above), and clamps it to 0.1..1.9. Its
// own default is 1.0. Before the softfp strtod shim, AsFLOAT read garbage, the
// clamp turned that into 0.1, and the engine wrote "0.10" back -- after which
// every boot read 0.10 honestly. log195: footsteps at volume 0.05, doors and
// footlockers 0.09, blasters 0.10, against ambience at 0.2-0.6, all with SFX
// at 100. Exactly the floor is the signature; nobody picks it by hand, and any
// other value is left alone.
static void repair_sound_bias(void) {
  const char *path = g_ini_path ? g_ini_path : kIniPaths[0];
  char *text = read_whole_ini(path);
  if (!text) return;
  char val[32];
  if (!ini_get(text, "Sound Options", "2D3D Bias", val, sizeof(val)) ||
      strtof(val, NULL) > 0.1001f) {
    free(text);
    return;
  }
  size_t need = ini_set(text, "Sound Options", "2D3D Bias", "1.00", NULL, 0);
  char *out = malloc(need + 1);
  if (out) ini_set(text, "Sound Options", "2D3D Bias", "1.00", out, need + 1);
  free(text);
  if (!out) return;
  if (ini_replace_file(path, out, need))
    log_printf("[snd] swkotor.ini: 2D3D Bias=%s was the clamp floor left by the old "
               "strtod bug -- reset to the engine default 1.00", val);
  free(out);
}

// Save the picked language into swkotor.ini.
//
// The file belongs to the user and to the engine, which rewrites it whenever
// options are saved, so this changes the one line and hands back everything
// else untouched (see ini_set). It lands via a temp file: an interrupted write
// then costs the new setting and never the file that was already there.
//
// `translation` is the fan-translation folder to save, or NULL for none. An
// ini that never had a Translation key does not gain an empty one.
static char *ini_with(const char *base, const char *key, const char *value,
                      size_t *len) {
  size_t need = ini_set(base, "Game Options", key, value, NULL, 0);
  char *out = malloc(need + 1);
  if (out) ini_set(base, "Game Options", key, value, out, need + 1);
  *len = need;
  return out;
}

static void write_language(int id, const char *translation) {
  const char *code = ini_language_code(id);
  const char *path = g_ini_path ? g_ini_path : kIniPaths[0];

  char *text = read_whole_ini(path);
  const char *base = text ? text : "";

  size_t need = 0;
  char *out = ini_with(base, "Language", code, &need);
  char had[2];
  if (out && (translation ||
              ini_get(out, "Game Options", "Translation", had, sizeof(had)))) {
    char *both = ini_with(out, "Translation", translation ? translation : "", &need);
    free(out);
    out = both;
  }
  free(text);
  if (!out) {
    log_printf("[lang] out of memory writing %s", path);
    return;
  }
  int ok = ini_replace_file(path, out, need);
  free(out);
  if (!ok) return;
  log_printf("[lang] saved [Game Options] Language=%s%s%s to %s", code,
             translation ? " Translation=" : "", translation ? translation : "",
             path);
}

// Offer the picker and act on what comes back. Deliberately called from the
// game thread before loadscreen_begin(): vitaGL is up by then, and the time a
// user spends reading a menu must not land inside the boot-duration estimate
// the progress bar persists, or the next boot's bar is pure fiction.
//
// Rows past the game's five languages are fan translations, which run as
// English with their own table swapped in (see resolve_translation).
static void offer_language_picker(void) {
  const char *extra[TRANSLATION_MAX];
  int nextra = translation_count();
  for (int i = 0; i < nextra; i++) extra[i] = translation_label(i);

  int current = translation_active() >= 0
                  ? LANGSEL_BUILTIN + translation_active() : g_lang_id;
  int picked = current;
  if (!langsel_run(current, g_lang_have_key, extra, nextra, &picked)) return;

  const char *folder = NULL;
  if (picked >= LANGSEL_BUILTIN) {
    translation_select(picked - LANGSEL_BUILTIN);
    folder = translation_name(picked - LANGSEL_BUILTIN);
    picked = INI_LANG_EN;
  } else {
    translation_select(-1);
  }
  g_lang_id = picked;
  g_lang_have_key = 1;
  jni_set_language(picked);               // no game code has run yet
  write_language(picked, folder);
}

// The mod menu, when the Google Play button asked for it on the last run (see
// modset.h). Same window and same screen as the language picker, and for the
// same reasons; row 0 is the card root as it was before mod sets existed.
static void offer_mod_menu(void) {
  if (!modset_menu_requested()) return;

  const char *rows[MODSET_MAX + 1];
  int count = 0;
  rows[count++] = "VANILLA";
  for (int i = 0; i < modset_count(); i++) rows[count++] = modset_label(i);

  int picked = modset_active() + 1;
  if (!langsel_list("CHOOSE A MOD SET",
                    count > 1 ? "CHANGE THIS AGAIN WITH THE GOOGLE PLAY BUTTON"
                              : "ADD MOD SETS AS FOLDERS IN UX0:DATA/KOTOR/MODS/",
                    rows, count, picked, &picked))
    return;
  modset_choose(picked - 1);
}

static void install_sound_probe(void) {
  hook_named("_ZN15CExoSoundSourceC1ERK7CResRef", (uintptr_t)&SndSrcCtor_probe,
             (void **)&SndSrcCtor_orig, "CExoSoundSource::CExoSoundSource(CResRef)");
  hook_named("_ZN15CExoSoundSource4PlayEv", (uintptr_t)&SndSrcPlay_probe,
             (void **)&SndSrcPlay_orig, "CExoSoundSource::Play");
  hook_named("_ZN23CExoSoundSourceInternal6DemandEv", (uintptr_t)&SndDemand_probe,
             (void **)&SndDemand_orig, "CExoSoundSourceInternal::Demand");
  hook_named("_ZN32CExoStreamingSoundSourceInternal16InitializeSourceEv",
             (uintptr_t)&StreamInit_probe, (void **)&StreamInit_orig,
             "CExoStreamingSoundSourceInternal::InitializeSource");
  hook_named_port("_ZN15FModAudioSystem11CreateSoundEPciPvmii",
                  (uintptr_t)&FmodCreateSound_probe,
                  (void **)&FmodCreateSound_orig, "FModAudioSystem::CreateSound");
  hook_named_port("_ZN15FModAudioSystem12CreateStreamEPcP9SDL_RWopsiiiii",
                  (uintptr_t)&FmodCreateStream_probe,
                  (void **)&FmodCreateStream_orig, "FModAudioSystem::CreateStream");
  hook_named_port("_ZN15FModAudioSystem9PlaySoundEi", (uintptr_t)&FmodPlaySound_probe,
                  (void **)&FmodPlaySound_orig, "FModAudioSystem::PlaySound");
  hook_named_port("_ZN15FModAudioSystem10PlayStreamEmi", (uintptr_t)&FmodPlayStream_probe,
                  (void **)&FmodPlayStream_orig, "FModAudioSystem::PlayStream");
  hook_named_port("_ZN15FModAudioSystem11StopChannelEm", (uintptr_t)&FmodStopChannel_probe,
                  (void **)&FmodStopChannel_orig, "FModAudioSystem::StopChannel");
  hook_named_port("_ZN15FModAudioSystem11CloseStreamEm", (uintptr_t)&FmodCloseStream_probe,
                  (void **)&FmodCloseStream_orig, "FModAudioSystem::CloseStream");
  hook_named_port("_ZN15FModAudioSystem12ReleaseSoundEi", (uintptr_t)&FmodReleaseSound_probe,
                  (void **)&FmodReleaseSound_orig, "FModAudioSystem::ReleaseSound");
}

static void install_load_probe(void) {
  g_ai_update_time = (volatile float *)so_symbol(&kotor_mod, "g_AIUpdateTime");
  g_display_fps = (volatile float *)so_symbol(&kotor_mod, "displayFPS");
  g_movie_fps = (volatile int *)so_symbol(&kotor_mod, "g_nSetMovieFrameRate");
  g_render_skip = (volatile int *)so_symbol(&port_mod, "g_RenderSkip");
  if (g_ai_update_time) g_last_ai_ms = *g_ai_update_time;
  log_printf("[perf] policy globals: AI=%p renderSkip=%p displayFPS=%p movieFPS=%p",
             (void *)g_ai_update_time, (void *)g_render_skip,
             (void *)g_display_fps, (void *)g_movie_fps);
#if DISABLE_ADAPTIVE_RENDER_SKIP
  log_printf("[perf] adaptive render skip override: ON (selected value is logged, then cleared)");
#endif
  // The held-button mask the watchdog reports (a plain .bss global in libKOTOR).
  sdl_gamepad_probe_init(so_symbol(&kotor_mod, "pressedGamepadButtons"));
  hook_named("_Z10GameUpdatev",
             (uintptr_t)&GameUpdate_probe, (void **)&GameUpdate_orig,
             "GameUpdate");
  hook_named("_Z12UpdateScreenfii",
             (uintptr_t)&UpdateScreen_probe, (void **)&UpdateScreen_orig,
             "UpdateScreen");
}

static void install_gui_probe(void) {
  uintptr_t *setup_slot = find_jump_slot(&kotor_mod,
      "_Z19AurGUISetupViewportiiiiRK6Vectorbf");
  uintptr_t *close_slot = find_jump_slot(&kotor_mod,
      "_Z19AurGUICloseViewportv");
  if (setup_slot && close_slot) {
    AurGUISetupViewport_orig = (int (*)(int, int, int, int, const void *, uint32_t, uint32_t))*setup_slot;
    AurGUICloseViewport_orig = (void (*)(void))*close_slot;
    uintptr_t setup_replacement = (uintptr_t)&AurGUISetupViewport_scissor;
    uintptr_t close_replacement = (uintptr_t)&AurGUICloseViewport_scissor;
    kuKernelCpuUnrestrictedMemcpy(setup_slot, &setup_replacement, sizeof setup_replacement);
    kuKernelCpuUnrestrictedMemcpy(close_slot, &close_replacement, sizeof close_replacement);
    log_printf("[gui:viewport] nested AurGUI clipping enabled via PLT");
  } else {
    log_printf("[gui:viewport] AurGUI PLT replacement FAILED setup=%p close=%p",
               (void *)setup_slot, (void *)close_slot);
  }

}

static void install_font_probe(void) {
  uintptr_t ws = so_symbol(&kotor_mod, "_ZN21CAurGUIStringInternal11WrapStringsEi");
  if (ws) {
    WrapStrings_orig = (int (*)(void *, int))build_thumb_trampoline(ws, thumb_patch_len(ws));
    if (WrapStrings_orig) {
      hook_thumb(ws, (uintptr_t)&WrapStrings_guard);
      log_printf("[font] WrapStrings GUARDED: ws=0x%08x tramp=%p", (unsigned)ws, (void *)WrapStrings_orig);
    } else {
      hook_thumb(ws, (uintptr_t)&WrapStrings_noop);
      log_printf("[font] WrapStrings trampoline FAILED -- no-op fallback (text off, no crash)");
    }
  } else {
    log_printf("[font] WrapStrings symbol missing -- guard NOT installed");
  }

  uintptr_t dr = so_symbol(&kotor_mod, "_ZN21CAurGUIStringInternal4DrawEf");
  if (dr) {
    Draw_orig = (void (*)(void *, uint32_t))build_thumb_trampoline(dr, thumb_patch_len(dr));
    if (Draw_orig) {
      hook_thumb(dr, (uintptr_t)&Draw_guard);
      log_printf("[font] Draw GUARDED: dr=0x%08x tramp=%p", (unsigned)dr, (void *)Draw_orig);
    } else {
      hook_thumb(dr, (uintptr_t)&Draw_noop);
      log_printf("[font] Draw trampoline FAILED -- no-op fallback (text off, no crash)");
    }
  } else {
    log_printf("[font] Draw symbol missing -- guard NOT installed");
  }
}

// Runs the game's SDL_main (passed as arg) on its own large-stack thread.
// vitaGL MUST be initialised here, on this thread -- GXM binds its render/
// display context to the initialising thread, and the game does ALL its GL from
// this thread. Initialising vitaGL on the main thread instead makes the first
// GXM-touching call (framebuffer/texture setup after the GL cap-query) block
// forever on a cross-thread GPU sync. (Pure glGetIntegerv queries still work,
// which is why init got as far as it did.)
static void *game_main_thread(void *arg) {
  int (*SDL_main)(int, char **) = arg;
  char *game_argv[] = { "KOTOR", NULL };

  g_game_thid = sceKernelGetThreadId();   // publish for the watchdog
  log_printf(">>> game thread UID = 0x%08x", (unsigned)g_game_thid);
  // Core 0 is this thread's alone: everything else is pinned to 1 and 2.
  thread_pin_self(CPU_GAME, "game-main");

  log_printf(">>> init vitaGL on game thread");
  /* This symbol exists only when vitaGL is built with HAVE_SHADER_CACHE=1.
   * Referencing it makes an uncached archive fail at link time instead of
   * silently reintroducing multi-second first-use shader stalls. */
  extern char vgl_shader_cache_path[256];
  log_printf(">>> vitaGL application shader cache storage = %p",
             (void *)vgl_shader_cache_path);
  vglSetupRuntimeShaderCompiler(SHARK_OPT_UNSAFE, SHARK_ENABLE, SHARK_ENABLE, SHARK_ENABLE);
  // vitaGL's 1 MB vertex USSE pool filled in a 20-minute session (log192);
  // gxm_patcher.c also frees idle variants, the bigger pool keeps that rare.
  vglSetupShaderPatcher(GXMP_BUFFER_MEM, GXMP_VERTEX_USSE_MEM, GXMP_FRAGMENT_USSE_MEM);
  // The garbage collector frees the GPU memory of each retired frame. By default
  // it may land on the game thread's core; keep it beside the other helpers.
  vglSetupGarbageCollector(0x10000100, thread_mask(CPU_AUX_B));
  vglInitExtended(0, SCREEN_W, SCREEN_H, MEMORY_VITAGL_THRESHOLD_MB * 1024 * 1024, GL_MSAA_MODE);
  gxmp_arm();
  log_printf(">>> vitaGL application shader cache: %s", vgl_shader_cache_path);

  // vitaGL ignores the return of sceGxmShaderPatcherCreate (gxm.c:561), so a
  // failed patcher init is silent -- the global just stays NULL and the first
  // sceGxmShaderPatcherRegisterProgram (during glLinkProgram) hands SceGxm a
  // null and faults at FAR=0x24. Both are non-static globals; report them so a
  // failure here is visible at init instead of as a mystery crash later.
  {
    extern SceGxmShaderPatcher *gxm_shader_patcher;
    extern GLboolean is_shark_online;
    log_printf(">>> vitaGL up: gxm_shader_patcher=%p  shark_online=%d",
               (void *)gxm_shader_patcher, (int)is_shark_online);
    if (!gxm_shader_patcher)
      log_printf("!!! gxm_shader_patcher is NULL -- shader patcher failed to "
                 "create; every glLinkProgram will fault inside SceGxm");
  }

  // Mount the OBB archives so SDL_main's wait loop (g_obbMounted &&
  // g_patchObbMounted) proceeds. This is the slowest part of startup on a cold
  // cache, so put a progress bar up first -- vitaGL is already initialised
  // above, and the bar draws from this thread via the archive read path.
  // A prebuilt .idx means the mount replays from cache and startup is about a
  // minute shorter, so the bar needs the matching estimate.
  offer_mod_menu();
  offer_language_picker();

  int warm = 0;
  { SceUID t = sceIoOpen(DATA_PATH "/main.obb.idx", SCE_O_RDONLY, 0);
    if (t >= 0) { warm = 1; sceIoClose(t); } }
  loadscreen_begin(warm);

  mount_obbs();
  io_obb_mount_done();      // stop recording, write the replay caches
  // NOT loadscreen_end() here: the game draws nothing for another ~59s. The
  // bar stays up until the first glDraw* call hands the screen over.

  ensure_writable_dirs();

  log_printf(">>> entering SDL_main");
  int rc = SDL_main(1, game_argv);
  log_printf("<<< SDL_main returned %d", rc);
  return NULL;
}

int main(int argc, char *argv[]) {
  log_init();
  log_printf("KOTOR Vita loader starting (skeleton, link-only)");

  sceKernelChangeThreadPriority(0, 127);
  sceKernelChangeThreadCpuAffinityMask(0, CPU_AUX_B);   // parks in pthread_join
  thread_census_add(sceKernelGetThreadId(), "main");

  sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);

  // Start watching for the language picker's L trigger the moment the pad can
  // be read. The picker itself is not reached for several seconds yet, and all
  // of them are black screen, so the trigger has to be latched across the whole
  // wait rather than sampled once at the end of it.
  langsel_watch_begin();
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_STOP);
  // Stopping the rear panel is not enough on its own: reVita turns rear
  // sampling back on in every pad read it intercepts, and SDL's touch backend
  // then turns the fingers resting on the back into finger events the game
  // treats as screen drags. SDL skips the rear port entirely with this set.
  SDL_setenv("VITA_DISABLE_TOUCH_BACK", "1", 1);

  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  scePowerSetGpuXbarClockFrequency(166);

  if (check_kubridge() < 0)
    fatal_error("kubridge.skprx is not installed.");

  // vitaGL compiles the game's GLSL at runtime via SceShaccCg (libshacccg.suprx),
  // which is NOT present on retail Vitas. Without it the first shader op hangs
  // silently; fail fast with a clear message instead (matches gtasa_vita).
  if (!file_exists("ur0:/data/libshacccg.suprx") &&
      !file_exists("ur0:/data/external/libshacccg.suprx"))
    fatal_error("libshacccg.suprx is not installed (need it in ur0:/data/).");

  // Load the compression libs FIRST: the companion and libKOTOR list them as
  // NEEDED and import mz_zip_reader_*/LzmaUncompress, which then resolve
  // cross-module to these (the real miniz reads the OBB zips; ret0 stubs made
  // the game read a null zip central directory and crash).
  if (load_module(&lzma_mod, LZMA_SO, LZMA_LOAD_ADDRESS) < 0)
    fatal_error("could not load %s", LZMA_SO);
  if (load_module(&miniz_mod, MINIZ_SO, MINIZ_LOAD_ADDRESS) < 0)
    fatal_error("could not load %s", MINIZ_SO);

  // Then the companion, so libKOTOR's imports of it resolve cross-module.
  if (load_module(&port_mod, ANDROID_PORT_SO, ANDROID_PORT_LOAD_ADDRESS) < 0)
    fatal_error("could not load %s", ANDROID_PORT_SO);
  if (load_module(&kotor_mod, SO_PATH, LOAD_ADDRESS) < 0)
    fatal_error("could not load %s", SO_PATH);

  // Now that both module bases are known, arm the CPU-fault handler so any
  // hardware fault (incl. during static ctors below or inside the game) writes
  // its PC/LR to log.txt instead of silently stopping the log.
  crash_init();

  // Arm the new-handler before any of the game's static ctors run, so a heap
  // exhaustion anywhere from here on is reported and survivable rather than an
  // uncaught bad_alloc (log140).
  heap_init();

  // Same point in the sequence, for the same reason: from the first static ctor
  // onwards every allocation at or above BIGALLOC_MIN_BYTES should be landing in
  // the pool rather than carving up newlib's arena (log145).
  bigalloc_init();

  // Stub Bink (companion-provided) so cutscenes are skipped for now.
  bink_patch(&port_mod);

  so_initialize(&lzma_mod);
  so_initialize(&miniz_mod);
  so_initialize(&port_mod);
  so_initialize(&kotor_mod);

  // Font metrics: inject our bundled .txi as a memory-backed resource so
  // CAurFontInfo populates and GUI text renders. The guards below stay installed
  // regardless -- they keep the GUI-string methods null-safe during the window
  // before the font loads, and are the safety net if injection doesn't take.
#if FONT_TXI_MEMORY_INJECT
  install_aurresget_hook();
#endif
  install_font_probe();
  install_gui_probe();
  install_lzma_probe();
  install_model_timers();
  install_load_probe();
  gameprof_install();
  ini_write_install();
  install_sound_probe();
  modset_install_button();

  // NOTE: vitaGL is initialised on the game thread (see game_main_thread), not
  // here -- GXM context must live on the thread that issues GL calls.

  // Read the language out of swkotor.ini before the JNI tables go up: the
  // game polls getCurrentLanguage from its first frame onwards.
  resolve_language();
  read_vita_options();
  repair_sound_bias();
  modset_scan();          // before the game opens anything on the card

  // Build the fake JNI tables (this build has no JNI_OnLoad; see RECON-JNI.md).
  jni_setup();

  // Phase 1, step 3: hand off to the game's real entry point. SDL renames the
  // game's main() to SDL_main; run it on a dedicated large-stack thread (the
  // Vita main thread's stack is too small for the game). main() then parks in
  // the join so the process stays alive and vitaGL isn't torn down.
  int (*SDL_main)(int, char **) = (void *)so_symbol(&kotor_mod, "SDL_main");
  if (!SDL_main) {
    fatal_error("SDL_main not found in libKOTOR.so");
  }

  // Watchdog first, so it's sampling before/at the moment the game thread hangs.
  pthread_t wd_thread;
  pthread_create(&wd_thread, NULL, watchdog_thread, NULL);
  pthread_t hs_thread;
  pthread_create(&hs_thread, NULL, hitch_sampler_thread, NULL);

  log_printf(">>> starting game entry SDL_main on dedicated thread");
  pthread_t game_thread;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);   // 4 MB game stack
  if (pthread_create(&game_thread, &attr, game_main_thread, (void *)SDL_main) != 0)
    fatal_error("failed to spawn game thread");
  pthread_join(game_thread, NULL);

  log_printf("<<< game thread exited; loader shutting down");
  return 0;
}
