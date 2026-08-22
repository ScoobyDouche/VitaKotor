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
#include "audio_patch.h"
#include "bink_patch.h"
#include "fs_patch.h"
#include "sdl_patch.h"
#include "gl_patch.h"
#include "ime_patch.h"
#include "crash.h"
#include "heap.h"
#include "bigalloc.h"
#include "log.h"

#include <pthread.h>

so_module kotor_mod;
so_module port_mod;
so_module miniz_mod;
so_module lzma_mod;

SceTouchPanelInfo panelInfoFront, panelInfoBack;

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

static void *watchdog_thread(void *arg) {
  (void)arg;
  uint64_t last_run = 0;
  for (;;) {
    sceKernelDelayThread(3 * 1000 * 1000);
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
#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)
#endif

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

// Copy `len` position-independent thumb prologue bytes from orig_fn into a fresh
// RX block, append LDR.W PC,[PC] -> orig_fn+len (thumb), and return a callable
// thumb pointer. Both guarded methods share the same 8-byte prologue
// (push{...}/add r7,sp,#12/stmdb) -- all PC-independent, so relocating is safe.
// Returns 0 on failure.
// How many bytes hook_thumb() will actually clobber at `addr`, rounded up to a
// Thumb instruction boundary so the trampoline never resumes mid-instruction.
//
// hook_thumb writes 8 bytes (LDR PC,[PC] + target), but when (addr & 2) it first
// drops a 2-byte NOP to 4-align the LDR -- 10 bytes total. Passing a flat 8 there
// makes the trampoline resume INSIDE the target-address word: log54/55 crashed
// exactly that way on CSWGuiImage::SetExtent (0x4abe5a, 2-mod-4), resuming on the
// high halfword of &SetExtent_probe, which decoded as `strh r3,[r0,#8]` and wrote
// to address 8 with r0=NULL -- FAR=0x8, FSR=0x8c7. Every hook before it happened
// to be 0-mod-4, so this stayed latent.
//
// Thumb length rule: a halfword whose top 5 bits are 0b11101/11110/11111 starts a
// 32-bit instruction, anything else is 16-bit.
static size_t thumb_patch_len(uintptr_t addr) {
  addr &= ~(uintptr_t)1;
  size_t need = (addr & 2) ? 10 : 8;
  size_t len = 0;
  while (len < need) {
    uint16_t hw = *(const uint16_t *)(addr + len);
    len += ((hw & 0xF800) >= 0xE800) ? 4 : 2;
  }
  return len;
}

// NOTE: the resume sequence is `LDR.W PC,[PC]` followed by the target word, and a
// Thumb literal load resolves its address as Align(PC,4) -- PC being the
// instruction's address + 4. That rounds DOWN, so the LDR must itself sit on a
// 4-byte boundary or it reads the word two bytes early: half the LDR encoding
// glued to half the target address. The trampoline base is page-aligned, so this
// bites whenever `len` is 2 mod 4 -- which thumb_patch_len returns for any hook
// site that is 2-mod-4 and whose prologue happens to total 10 bytes.
// CAppManager::CreateServer (+0x3fba22, len 10) hit it and jumped to 0xba2df000,
// exactly the value this miscomputation predicts. A 2-byte NOP before the LDR
// realigns it; len already 0 mod 4 is untouched, so existing hooks are unaffected.
static uintptr_t build_thumb_trampoline(uintptr_t orig_fn, size_t len) {
  orig_fn &= ~(uintptr_t)1;
  size_t pad = (len & 2) ? 2 : 0;      // realign the LDR.W to a 4-byte boundary
  size_t sz = len + pad + 8; // + LDR.W PC,[PC] (4) + target word (4)
  SceKernelAllocMemBlockKernelOpt opt;
  memset(&opt, 0, sizeof(opt));
  opt.size = sizeof(opt);
  SceUID blk = kuKernelAllocMemBlock("gui_tramp", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX,
                                     (sz + 0xfff) & ~(size_t)0xfff, &opt);
  if (blk < 0) {
    log_printf("[font] trampoline: AllocMemBlock(USER_RX) failed 0x%08x", (unsigned)blk);
    return 0;
  }
  void *base = NULL;
  int r = sceKernelGetMemBlockBase(blk, &base);
  if (r < 0 || base == NULL) {
    log_printf("[font] trampoline: GetMemBlockBase failed 0x%08x base=%p", (unsigned)r, base);
    return 0;
  }
  uint8_t buf[32];
  if (sz > sizeof buf) {
    log_printf("[font] trampoline: len %u too large for buffer", (unsigned)len);
    return 0;
  }
  memcpy(buf, (const void *)orig_fn, len);
  uint16_t nop   = 0xbf00;                           // NOP (alignment filler)
  uint32_t ldrpc = 0xf000f8df;                       // LDR.W PC, [PC]
  uint32_t cont  = (uint32_t)(orig_fn + len) | 1u;    // resume mid-function, thumb
  if (pad) memcpy(buf + len, &nop, sizeof nop);
  memcpy(buf + len + pad, &ldrpc, sizeof ldrpc);
  memcpy(buf + len + pad + sizeof ldrpc, &cont, sizeof cont);
  kuKernelCpuUnrestrictedMemcpy(base, buf, sz);
  kuKernelFlushCaches(base, sz);
  return (uintptr_t)base | 1u;                        // thumb-callable
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
  // Scope the GL text-draw trace to exactly this call so glyph draws are
  // distinguishable from scene draws (see g_gl_text_draw in gl_patch.h).
  g_gl_text_draw = 1;
  Draw_orig(self, xf);
  g_gl_text_draw = 0;
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
// Widget images load but never draw. log50/51: exactly 4 textured quads per frame
// (1024x512 + 512x1024 + 2x 32x32) with 756x106 (the ten ios_mm_*_en.tga menu
// buttons) and 256x256 (font atlas) at ZERO for entire runs -- on every screen, and
// a GUI screen with no background art renders pure black. DPI was not the cause.
//
// log52 settled the emit side. FlushBuffer opens with
//     r5 = &cm_nGUIBufferSizeUsed; ldr r0,[r5]; cmp r0,#1; blt <store 0, return>
// and it ran 97921 times with the draw histogram frozen at 32x32/512x1024/1024x512
// -- so every flush early-outs on an empty buffer and the loss is upstream, on the
// accumulate side. (The old counter hooked CAurGUIImageInternal::Draw(float), a
// 112-byte WEAK convenience overload nothing calls; the live entries are the 8-arg
// Draw/DrawBuffered(ffffhfRK6Vectorf) pair. imageDraw=0 measured dead code.)
//
// So probe the widget-level entry instead. CSWGuiImage::Draw(float) is three
// stacked silent early-outs before it dispatches to image->vtable[0x1c]:
//     r0 = this[0x24]        cbz    -> return   (image object null)
//     r1 = this[0x0c]        cmp 0  \ itt ne
//     r1 = this[0x10]        cmpne 0/ bne draw  -> else return  (extent w/h zero)
// A zero extent silences every widget on every screen while non-widget background
// art still renders -- which is the symptom, and it explains why the histogram did
// not move when the game switched from the main menu to chargen. Log which gate
// bites plus the raw values; the field offsets are inferred, the values are not.
//
// Floats arrive in core registers (softfp caller), hence uint32_t params. Both
// hooked prologues are 8 clean bytes (push/add r7/str.w) with no PC-relative loads.
static void log_screen_globals(const char *when);  // defined with the probes below

static void (*SWImgDraw_orig)(void *self, uint32_t f) = NULL;
static void (*FlushBuf_orig)(void *self, uint32_t f) = NULL;
static const volatile int32_t *g_gui_buf_used = NULL;
static unsigned g_flush_n = 0, g_flush_nonempty = 0;
static int32_t g_flush_max_used = 0;
static unsigned g_sw_n = 0, g_sw_gate_obj = 0, g_sw_gate_w = 0, g_sw_gate_h = 0, g_sw_pass = 0;

/* Which widgets actually went through ScaleExtentForResolution.
 *
 * Two theories about the oversized minimap and the fog panel have now died on
 * hardware -- the extent counters (log155: 1200 loaded, 2047 scaled) and the
 * NPOT pad content (log156: resampled 81 times, boxes unchanged). The counter
 * comparison was never evidence in the first place: ExtentLoad and ScaleExtent
 * are tallies over DIFFERENT objects, one widget can be scaled repeatedly, and
 * SetExtent installs extents that ExtentLoad never saw. A total tells you
 * nothing about whether THIS widget was scaled.
 *
 * So record the identity, not the count. Every widget that passes through
 * ScaleExtent goes in this set; any large image reports, once, whether its own
 * pointer is in it. An element left at authored size inside a frame scaled by
 * 0.7083 is 1.41x too big for that frame, which is precisely how both the
 * minimap and the fog panel overflow. If the offending widget comes back
 * scaled=NO, that is the bug and the fix is to scale it. If it comes back
 * scaled=YES, the extent path is exonerated for good and the cause is in the
 * draw itself. */
#define GUI_PTRSET_SLOTS 1024              /* power of two; open addressing */
typedef struct { uint32_t slot[GUI_PTRSET_SLOTS]; unsigned n, overflow; } GuiPtrSet;
static GuiPtrSet g_gui_scaled;             /* widgets ScaleExtent has touched */
static GuiPtrSet g_gui_reported;           /* big images already logged once */

static unsigned gui_ptr_hash(uint32_t p) { return ((p >> 2) * 2654435761u) & (GUI_PTRSET_SLOTS - 1); }

/* Returns 1 if p was ALREADY present. Insert-and-test in one pass so the draw
 * path can use it directly as a once-only gate. */
static int gui_ptrset_add(GuiPtrSet *s, uint32_t p) {
  if (!p) return 1;
  unsigned h = gui_ptr_hash(p);
  for (unsigned i = 0; i < GUI_PTRSET_SLOTS; i++) {
    unsigned k = (h + i) & (GUI_PTRSET_SLOTS - 1);
    if (s->slot[k] == p) return 1;
    if (!s->slot[k]) { s->slot[k] = p; s->n++; return 0; }
  }
  s->overflow++;                            /* full: report rather than lie */
  return 1;
}
static int gui_ptrset_has(const GuiPtrSet *s, uint32_t p) {
  unsigned h = gui_ptr_hash(p);
  for (unsigned i = 0; i < GUI_PTRSET_SLOTS; i++) {
    unsigned k = (h + i) & (GUI_PTRSET_SLOTS - 1);
    if (s->slot[k] == p) return 1;
    if (!s->slot[k]) return 0;
  }
  return 0;
}
static unsigned g_bigimg_logged = 0;

static void SWImgDraw_probe(void *self, uint32_t f) {
  const uint32_t *o = (const uint32_t *)self;
  uint32_t img = o[9];  // +0x24
  uint32_t w = o[3];    // +0x0c
  uint32_t h = o[4];    // +0x10
  if (!img)     g_sw_gate_obj++;
  else if (!w)  g_sw_gate_w++;
  else if (!h)  g_sw_gate_h++;
  else          g_sw_pass++;
  /* The boxes are big. Report each large image once, with the one fact that
   * separates the two remaining theories. Capped, and the cap is printed --
   * a capped counter read as a finding has cost this port two hardware runs. */
  if (img && (int)w >= 200 && (int)h >= 200 && g_bigimg_logged < 200) {
    uint32_t sp = (uint32_t)(uintptr_t)self;
    if (!gui_ptrset_add(&g_gui_reported, sp)) {
      g_bigimg_logged++;
      log_printf("[gui] big image #%u self=0x%08x img=0x%08x w=%d h=%d scaled=%s"
                 "  (scaled set %u entries, %u overflowed)",
                 g_bigimg_logged, sp, (unsigned)img, (int)w, (int)h,
                 gui_ptrset_has(&g_gui_scaled, sp) ? "YES" : "NO",
                 g_gui_scaled.n, g_gui_scaled.overflow);
    }
  }
  if ((g_sw_n++ % 2400) == 0) {
    log_printf("[gui] SWImage::Draw n=%u gates objnull=%u w0=%u h0=%u PASS=%u "
               "(last img=0x%08x w=%d h=%d)",
               g_sw_n, g_sw_gate_obj, g_sw_gate_w, g_sw_gate_h, g_sw_pass,
               (unsigned)img, (int)w, (int)h);
    // Also sample at draw time: the globals may be set long after ImgInit ran.
    if ((g_sw_n % 24000) == 1) log_screen_globals("at draw");
  }
  SWImgDraw_orig(self, f);
}

static void FlushBuf_probe(void *self, uint32_t f) {
  int32_t used = g_gui_buf_used ? *g_gui_buf_used : -1;
  if (used > 0) g_flush_nonempty++;
  if (used > g_flush_max_used) g_flush_max_used = used;
  if ((g_flush_n++ % 2400) == 0)
    log_printf("[gui] flushBuffer=%u bufUsed=%d nonEmpty=%u maxUsed=%d swDraw=%u",
               g_flush_n, (int)used, g_flush_nonempty, (int)g_flush_max_used, g_sw_n);
  FlushBuf_orig(self, f);
}

// log53: 8161/8161 widget draws bailed on extent.width==0, with extent.height==480
// and a valid image object. SetExtent proves the layout -- it does
//     vld1.32 {d16-d17},[r1] ; adds r5,r4,#4 ; vst1.32 {d16-d17},[r5]
// i.e. it blits the whole CSWGuiExtent {x,y,w,h} to this+0x04..+0x13, so
// +0x0c IS width and +0x10 IS height. 480 is KOTOR's authoring height (GUIs are
// laid out in a 640x480 virtual space), so height survived the trip and width did
// not -- width is computed somewhere else and lands at 0.
//
// Find that somewhere: log the incoming extent AND the caller. The hook is
// installed as LDR PC,[PC] over the first 8 bytes -- a branch, not a call -- so LR
// still holds the original call site and __builtin_return_address(0) recovers it.
// Resolve the printed off= against libKOTOR.so's dynsym table to name the caller.
// Prologue is 8 clean bytes (push/add r7/mov r4,r0/ldr r0,[r0,#36]); the vld1 that
// would matter starts at +8. Signature is (this, const CSWGuiExtent*) -- no floats.
static void (*SetExtent_orig)(void *self, const void *ext) = NULL;
static unsigned g_se_n = 0, g_se_w0 = 0;

// log56: SetExtent NEVER receives a good width -- because it is not how the extent
// gets in. CSWGuiImage::Initialize writes it DIRECTLY:
//     vld1.32 {d16-d17},[r1] ; adds r1,r0,#4 ; vst1.32 {d16-d17},[r1] ; b.w SetParams
// no SetExtent call at all. Every SetExtent we logged was downstream code
// re-applying &this->extent (SetImage/operator= pass this+4 to themselves) long
// after it was already zero. So hook the real writer and log what it is handed.
// Site is 0-mod-4 and the first 8 bytes are vld1(4)+adds(2)+adds(2) -- clean.
static void (*ImgInit_orig)(void *self, const void *ext, const void *params) = NULL;
static unsigned g_ii_n = 0, g_ii_w0 = 0;

// log57: the port asks JNI for GetScreenHeightPixel (we answer 544) and
// GetScreenHeightInch -- and NEVER asks for a width. So width is derived inside
// libKOTOR, and `g_nScreenWidth`/`g_nScreenHeight` (adjacent globals, 0x5b0538 /
// 0x5b053c) are where it lands. A zero width there would explain every symptom at
// once: extents with a good h and w=0, on every widget, on every screen.
static const volatile int32_t *g_scr_w = NULL, *g_scr_h = NULL;
static const volatile int32_t *g_scr_wp2 = NULL, *g_scr_hp2 = NULL;

static void log_screen_globals(const char *when) {
  log_printf("[gui] screen: g_nScreenWidth=%d g_nScreenHeight=%d "
             "cm_nScreenWidthPow2=%d cm_nScreenHeightPow2=%d  (%s)",
             g_scr_w ? (int)*g_scr_w : -1, g_scr_h ? (int)*g_scr_h : -1,
             g_scr_wp2 ? (int)*g_scr_wp2 : -1, g_scr_hp2 ? (int)*g_scr_hp2 : -1,
             when);
}

static void ImgInit_probe(void *self, const void *ext, const void *params) {
  const int32_t *e = (const int32_t *)ext;
  if (e && e[2] == 0) g_ii_w0++;
  if (g_ii_n < 96 || (g_ii_n % 240) == 0) {
    uintptr_t lr = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
    log_printf("[gui] ImgInit #%u self=%p ext={x=%d y=%d w=%d h=%d} w0=%u from off=0x%06x",
               g_ii_n, self, e ? (int)e[0] : -1, e ? (int)e[1] : -1,
               e ? (int)e[2] : -1, e ? (int)e[3] : -1, g_ii_w0,
               (unsigned)(lr - kotor_mod.text_base));
    log_screen_globals("at ImgInit");
  }
  g_ii_n++;
  ImgInit_orig(self, ext, params);
}

static void SetExtent_probe(void *self, const void *ext) {
  const int32_t *e = (const int32_t *)ext;
  if (e && e[2] == 0) g_se_w0++;
  // First 64 in full (that covers main-menu construction), then thin out.
  if (g_se_n < 64 || (g_se_n % 240) == 0) {
    uintptr_t lr = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
    log_printf("[gui] SetExtent #%u self=%p ext={x=%d y=%d w=%d h=%d} w0=%u "
               "from off=0x%06x",
               g_se_n, self, e ? (int)e[0] : -1, e ? (int)e[1] : -1,
               e ? (int)e[2] : -1, e ? (int)e[3] : -1, g_se_w0,
               (unsigned)(lr - kotor_mod.text_base));
  }
  g_se_n++;
  SetExtent_orig(self, ext);
}

// Upstream of SetExtent: CSWGuiControl::Load calls CSWGuiExtent::Load, which reads
// four INT fields -- "LEFT","TOP","WIDTH","HEIGHT" -> extent+0,+4,+8,+12 (labels
// resolved from its literal pool). extent+8 is the width that reads 0.
//
// But note 0x49f358: `cbz r0, 0x49f3b6` -- if GetStructFromStruct("EXTENT") fails,
// ALL FOUR reads are skipped and the caller's CSWGuiExtent keeps whatever stale
// stack bytes it had. w=0/h=480 is exactly what uninitialized stack looks like, so
// "EXTENT struct not found" and "WIDTH field read returned the 0 default" are both
// live and they need different fixes.
//
// Distinguish them without changing behaviour: stamp the 16-byte extent with a
// sentinel, run the real Load, then see which words the callee actually wrote. Any
// word still holding the sentinel was never written -- restore the caller's
// original bytes there so the game sees exactly what it would have seen.
#define EXT_SENTINEL 0x5A5A5A5A
static unsigned g_sx_n = 0;      /* ScaleExtent calls, read by the totals line */
static int (*ExtLoad_orig)(void *self, void *gff, void *st) = NULL;
static unsigned g_xl_n = 0, g_xl_skipped = 0, g_xl_w0 = 0;

static int ExtLoad_probe(void *self, void *gff, void *st) {
  int32_t *e = (int32_t *)self;
  int32_t saved[4] = {e[0], e[1], e[2], e[3]};
  for (int i = 0; i < 4; i++) e[i] = EXT_SENTINEL;

  int rc = ExtLoad_orig(self, gff, st);

  unsigned unwritten = 0;
  for (int i = 0; i < 4; i++) {
    if (e[i] == EXT_SENTINEL) { unwritten |= (1u << i); e[i] = saved[i]; }
  }
  if (unwritten == 0xF) g_xl_skipped++;  // EXTENT struct not found -> nothing read
  if (e[2] == 0) g_xl_w0++;

  if (g_xl_n < 64 || (g_xl_n % 240) == 0)
    log_printf("[gui] ExtentLoad #%u rc=%d {L=%d T=%d W=%d H=%d} unwritten=0x%x "
               "skipped=%u w0=%u",
               g_xl_n, rc, (int)e[0], (int)e[1], (int)e[2], (int)e[3],
               unwritten, g_xl_skipped, g_xl_w0);
  if ((g_xl_n % 240) == 0)
    log_printf("[gui] extent totals: %u loaded, %u scaled  (a gap here is real, "
               "both counters are lifetime)", g_xl_n, g_sx_n);
  g_xl_n++;
  return rc;
}

// --- chargen: is a model ever even requested? ------------------------------
// log62 ruled out the missing gameinprogress/ dir (created, still faults at
// 0x24acb8) and the resource layer (only .txi misses in the whole chargen run).
// The creature has a valid animation base but animBase->GetModel(255) returns
// NULL, and no model resource is ever REQUESTED. So watch the loader itself:
// CSWCAnimBase::LoadModel(const CResRef&, unsigned char). Never called => the
// creature is never given a model (setup bug, upstream). Called => log the
// resref and the part id, and the failure is inside model loading.
// Returns a value (callers do `blx LoadModel ; cbz r0`), so the probe must pass it
// through -- declaring it void left r0 undefined on return and could have silently
// turned a successful load into a "failed" one at the call site.
static void *(*LoadModel_orig)(void *self, const void *resref, unsigned part) = NULL;
static unsigned g_lm_n = 0;

// log63 closed the chain. CSWCAnimBase::GetModel is five instructions:
//     cmp r1,#255 ; ite eq ; ldreq r0,[r0,#0xb8] ; movne r0,#0 ; bx lr
// so GetModel(255) is literally `return this->[0xb8]`. LoadModel IS called with
// the right resrefs (pmbbs/pmbbm/pmbbl/pfbbl/pfbbm/pfbbs, part=255) and the model
// DATA does load (~190KB + ~86KB new[] right after each call) -- but this+0xb8
// stays NULL, so the chargen draw null-derefs. Sample the field either side of
// the call: still NULL afterwards => LoadModel bails internally after reading the
// data, and the next step is bisecting its 424 bytes.
// log64 traced the whole chain:
//   LoadModel(resref,255) -> this[0xb8] = NewCAurObject(name,"body",NULL,NULL)
//   NewCAurObject: RWops args are NULL, so it takes the load-by-NAME path ->
//     FindModel("pmbbs") / "pmbbs_x" / "pmbbs_z"; returns NULL if the base one is
//   FindModel -> BinaryFindModel: `count = table[4]; if (count < 1) return NULL`
// i.e. an EMPTY model registry answers every lookup with NULL. g_nModelsRead is
// the engine's own count of models read into that registry -- if it is 0, nothing
// ever populated it and that (not chargen) is the real bug.
static const volatile int32_t *g_models_read = NULL;

// log65 narrowed it one more hop. g_nModelsRead is NOT 0 (it climbs 4,6,8,10,12,14
// -- 2 per LoadModel), so the registry IS being fed and the *lookup* is what fails.
// FindModel's load-on-miss path is:
//     IODispatcher::ReadSync(name) -> MaxTree*
//     MaxTree::AsModel()  ==  `if ((this[0x4c] & 0x7f) != 2) return NULL;`
//     strcasecmp(loaded->name, requested) -> mismatch writes AR_ERROR.LOG
// No RWFromFile fires in the LoadModel window, so AR_ERROR.LOG is never opened and
// the name-mismatch branch is NOT taken. That leaves ReadSync returning NULL, or
// returning a tree whose type tag != 2 (read fine, parsed as the wrong node type).
// Log the pointer and that tag byte to separate the two.
static void *(*ReadSync_orig)(void *self, char *name) = NULL;
static unsigned g_rs_n = 0;

// ReadSync's four exits, in order:
//   (1) AurResGet(name,".mdl",NULL,1) == NULL         -> NULL
//   (2) AurResGetDataBytes(4, res)    == NULL         -> NULL
//   (3) first byte != 0  -> the non-binary-MDL branch (binary MDL starts 0x00)
//   (4) MaxTree::AsModel() tag != 2                   -> NULL
// (1) is already excluded: no [res] MISS is logged for pmbbs, so AurResGet
// succeeds. Probe the header fetch to separate (2) from (3)/(4). Same resource
// family as the .txi work -- note the flag=1 (OBB blob) vs flag=0 (RWops stream)
// split that bit us there.
static void *(*ResDataBytes_orig)(unsigned long n, void *res) = NULL;
static unsigned g_rdb_n = 0;

// log67 pinned the divergence to WHICH BUFFER res[12] points at. AurResGet's OBB
// path is `r5 = AurGetResource(resref,type,&size); res[12] = res[24] = r5`, and
// AurResGetDataBytes' blob path just returns that cursor unchecked. Aligning the
// 15 observed reads mod 8 splits them cleanly:
//   working .mdl reads  -> ptr % 8 == 0   (plain decompressed new[](size) buffer)
//   every failing read  -> ptr % 8 == 6   (pool block: new[](size+6), data at +6,
//                                          the 6-byte inline header ReleaseResource
//                                          reads back via `ldrh [r0,#-6]`)
// The archive itself is exonerated: cgbody_light/pmbbs/pfbbl all LZMA-decompress
// offline to a clean `00 00 00 00` binary-MDL signature, so the bytes exist and
// the header check is right to reject what it was handed. What we do NOT know is
// what the pool block actually CONTAINS, and that is the whole question:
//   5d 00 00 00 01 ... -> the raw LZMA stream: decompression never ran
//   uninitialised junk -> it ran, but into the other buffer / it failed
//   valid MDL, shifted -> cursor/offset arithmetic is off
// So dump the bytes, the 6-byte block header, the res fields, and the thread id
// (a loader thread racing the pool would explain why menu models load and chargen
// ones do not).
static const char *g_rs_name = NULL;   // resref of the ReadSync in flight

static void *ResDataBytes_probe(unsigned long n, void *res) {
  void *p = ResDataBytes_orig(n, res);
  if (g_rdb_n < 96) {
    const uint32_t *o = (const uint32_t *)res;
    char hex[48], hdr[24];
    hex[0] = hdr[0] = 0;
    if (p) {
      const unsigned char *b = (const unsigned char *)p;
      for (int i = 0; i < 12; i++) sprintf(hex + i * 3, "%02x ", b[i]);
      for (int i = 0; i < 6; i++)  sprintf(hdr + i * 3, "%02x ", b[i - 6]);
    }
    log_printf("[model] RDB(%lu,%p) -> %p m8=%u \"%.16s\" res[0]=%08x [3]=%08x "
               "[4]=%d [6]=%08x hdr:%s| %s tid=%08x",
               n, res, p, p ? (unsigned)((uintptr_t)p & 7u) : 9u,
               g_rs_name ? g_rs_name : "-", o[0], o[3], (int)o[4], o[6],
               hdr, hex, (unsigned)sceKernelGetThreadId());
  }
  g_rdb_n++;
  return p;
}

static void *ReadSync_probe(void *self, char *name) {
  const char *prev = g_rs_name;
  g_rs_name = name;                 // tag the RDB reads this ReadSync makes
  void *r = ReadSync_orig(self, name);
  g_rs_name = prev;
  if (g_rs_n < 48) {
    int tag = r ? (int)(*(unsigned char *)((char *)r + 0x4c) & 0x7f) : -1;
    log_printf("[model] ReadSync(\"%.24s\") -> %p tag=%d%s",
               name ? name : "?", r, tag,
               !r          ? "  <<< NULL (read failed)"
               : tag != 2  ? "  <<< NOT A MODEL (AsModel returns NULL)"
                           : "  ok");
  }
  g_rs_n++;
  return r;
}

static void *LoadModel_probe(void *self, const void *resref, unsigned part) {
  void *before = *(void **)((char *)self + 0xb8);
  void *rc = LoadModel_orig(self, resref, part);
  void *after = *(void **)((char *)self + 0xb8);
  if (g_lm_n < 64)
    log_printf("[model] LoadModel #%u resref=\"%.16s\" part=%u this+0xb8: %p -> %p%s"
               "  g_nModelsRead=%d",
               g_lm_n, resref ? (const char *)resref : "?", part & 0xff,
               before, after, after ? "" : "  <<< STILL NULL",
               g_models_read ? (int)*g_models_read : -1);
  g_lm_n++;
  return rc;
}

// --- touch calibration: where ARE the widgets after scaling? ---------------
// Touch is fluid but lands off the buttons. ExtentLoad logs the AUTHORED extent
// (1024x768 space); what the hit-test and the renderer actually use is the
// SCALED one (x screenHeight/768 = 0.625 here). Log both ends so the button's
// real on-screen rect can be compared against the normalized touch coords that
// actually activate it -- measurement, not arithmetic guesswork.
// CSWGuiObject keeps its extent at this+0x08 (SetExtent/ScaleExtent both use it).
static void (*ScaleExt_orig)(void *self, uint32_t fscale) = NULL;

static void ScaleExt_probe(void *self, uint32_t fscale) {
  const int32_t *e = (const int32_t *)((const char *)self + 8);
  int32_t b[4] = {e[0], e[1], e[2], e[3]};
  gui_ptrset_add(&g_gui_scaled, (uint32_t)(uintptr_t)self);
  ScaleExt_orig(self, fscale);
  /* Cadence matched to ExtentLoad's on purpose. At a flat cap of 48 this went
   * quiet at t=145s while ExtentLoad ran on to #2400, and comparing the two
   * logged counts then "showed" 25 extents that were never scaled -- an
   * artifact of the cap, not a finding. Whether some extents really do skip
   * ScaleExtentForResolution is still open, and it matters: an element left at
   * authored size inside a frame scaled to 0.7083 is 1.41x too big for it,
   * which is what the minimap, the fog box and the save list all look like.
   * The save rows load as {L=471 T=358..567 W=300 H=30} in a 768-tall layout;
   * unscaled, T=567 falls off a 544-tall screen and lands on the buttons. */
  if (g_sx_n < 64 || (g_sx_n % 240) == 0) {
    float sc; memcpy(&sc, &fscale, 4);
    log_printf("[gui] ScaleExtent #%u self=0x%08x {L=%d T=%d W=%d H=%d} x%.4f -> {L=%d T=%d W=%d H=%d}",
               g_sx_n, (unsigned)(uintptr_t)self,
               (int)b[0], (int)b[1], (int)b[2], (int)b[3], sc,
               (int)e[0], (int)e[1], (int)e[2], (int)e[3]);
  }
  g_sx_n++;
}

// log68 read the failing block's contents and they are UNWRITTEN: the bytes are
// `10 40 40 81 10 40 40 81 ...` -- two identical pointers into OUR loader's .bss
// (0x8140xxxx), i.e. the fd/bk of a newlib free-list chunk. The block is otherwise
// perfect: its 6-byte header carries the right type tag (`d2 07` = 2002 = MDL) and
// res[4] is the exact unpacked size from the archive (4960 for cgbody_light, 192904
// for pmbbs). So AurGetResource located the entry, sized it, allocated and tagged a
// block -- and never decompressed into it. Working reads hold the real MDL bytes
// (`00 00 00 00 05 06 ...`, byte-identical to an offline LZMA decode).
//
// The correlation is exact and wider than the chargen crash: EVERY ptr%8==6 read is
// garbage and every ptr%8==0 read is filled, so gui3D_room.mdx and mainmenu.mdx are
// broken too -- ReadSync just never checks the .mdx, which is why the menu looked
// fine. That points at the decompressor, not at chargen.
//
// libandroid_port implements the OBB/BZF provider and imports LzmaUncompress from
// libLzmaLib (DT_NEEDED is present and so_resolve_link should bind it). Hook it and
// log both sizes in/out plus the SZ_ code, which separates the three candidates:
//   never called      -> the miniz/OBB read upstream failed
//   rc != 0           -> decode failed (1 DATA, 2 MEM, 4 UNSUPPORTED, 6 INPUT_EOF)
//   rc == 0, destLen  -> it "succeeded" into a buffer that is not this block
// Signature: int LzmaUncompress(u8 *dest, size_t *destLen, const u8 *src,
//                               size_t *srcLen, const u8 *props, size_t propsSize)
static int (*LzmaUncompress_orig)(unsigned char *, size_t *, const unsigned char *,
                                  size_t *, const unsigned char *, size_t) = NULL;
static unsigned g_lz_n = 0;

static int LzmaUncompress_probe(unsigned char *dest, size_t *destLen,
                                const unsigned char *src, size_t *srcLen,
                                const unsigned char *props, size_t propsSize) {
  size_t dl_in = destLen ? *destLen : 0;
  size_t sl_in = srcLen ? *srcLen : 0;
  int rc = LzmaUncompress_orig(dest, destLen, src, srcLen, props, propsSize);
  if (g_lz_n < 64) {
    char p[24];
    p[0] = 0;
    if (props)
      for (unsigned i = 0; i < propsSize && i < 5; i++) sprintf(p + i * 3, "%02x ", props[i]);
    log_printf("[lzma] #%u dest=%p m8=%u destLen=%u->%u src=%p srcLen=%u->%u "
               "props(%u):%s rc=%d out:%02x %02x %02x %02x",
               g_lz_n, dest, (unsigned)((uintptr_t)dest & 7u),
               (unsigned)dl_in, (unsigned)(destLen ? *destLen : 0), src,
               (unsigned)sl_in, (unsigned)(srcLen ? *srcLen : 0),
               (unsigned)propsSize, p, rc,
               dest ? dest[0] : 0, dest ? dest[1] : 0,
               dest ? dest[2] : 0, dest ? dest[3] : 0);
  }
  g_lz_n++;
  return rc;
}

static void install_lzma_probe(void) {
  uintptr_t lu = so_symbol(&lzma_mod, "LzmaUncompress");
  if (!lu) { log_printf("[lzma] LzmaUncompress symbol MISSING in libLzmaLib"); return; }
  // Confirm the companion's import actually bound here -- a silently unresolved
  // (ret0-stubbed) LzmaUncompress would produce exactly the unwritten block we see.
  uintptr_t imp = so_symbol(&port_mod, "LzmaUncompress");
  log_printf("[lzma] libLzmaLib LzmaUncompress=0x%08x  companion sees 0x%08x",
             (unsigned)lu, (unsigned)imp);
  LzmaUncompress_orig = (int (*)(unsigned char *, size_t *, const unsigned char *,
                                 size_t *, const unsigned char *, size_t))
      build_thumb_trampoline(lu, thumb_patch_len(lu));
  if (LzmaUncompress_orig) {
    hook_thumb(lu, (uintptr_t)&LzmaUncompress_probe);
    log_printf("[lzma] LzmaUncompress PROBED");
  } else {
    log_printf("[lzma] LzmaUncompress trampoline FAILED");
  }
}

// log71: chargen reaches the portrait screen, then DATA_ABORTs at libKOTOR+0x2bb016
// inside CSWGuiQuickPanel::OnSelectPortraitButton. That function does, unguarded:
//     GetModel(255) -> ldr r1,[r0]      (body -- fine now)
//     GetModel(254) -> ldr r1,[r0]      (part 254, r0 == NULL -> fault)
// The two GetModel bodies decide it:
//     CSWCAnimBaseHead::GetModel(p): p==254 -> this[0x44]; p==255 -> base; else 0
//     CSWCAnimBase::GetModel(p):     p==255 -> this[0xb8]; else 0
// and this[0x44] is written in exactly one place --
//     CSWCAnimBaseHead::LoadModel(resref, 254) @ 0x1c4875:
//         CResRef::CopyToString(buf); this[0x44] = NewCAurObject(buf, <type>, 0, 0)
// which is a DIFFERENT override from the CSWCAnimBase::LoadModel we already hook,
// so every head load so far has been invisible to us. Model DATA is now known good
// (pmbbs .mdl/.mdx bytes match an offline LZMA decode exactly), so this is about
// whether the head load is attempted at all and what NewCAurObject answers.
//
// NewCAurObject is the single funnel for instantiating any model by name, so
// logging it gives the whole picture in one line per attempt: which resrefs are
// asked for, with which type tag, and which come back NULL.
static void *(*NewCAurObject_orig)(char *name, char *type, void *rw1, void *rw2) = NULL;
static unsigned g_nao_n = 0;

static void *NewCAurObject_probe(char *name, char *type, void *rw1, void *rw2) {
  void *r = NewCAurObject_orig(name, type, rw1, rw2);
  if (g_nao_n < 96)
    log_printf("[model] NewCAurObject(\"%.20s\", \"%.12s\", rw=%p/%p) -> %p%s",
               name ? name : "(null)", type ? type : "(null)", rw1, rw2, r,
               r ? "" : "  <<< NULL");
  g_nao_n++;
  return r;
}

static void *(*HeadLoadModel_orig)(void *self, const void *resref, unsigned part) = NULL;
static unsigned g_hlm_n = 0;

static void *HeadLoadModel_probe(void *self, const void *resref, unsigned part) {
  void *before = *(void **)((char *)self + 0x44);
  void *r = HeadLoadModel_orig(self, resref, part);
  void *after = *(void **)((char *)self + 0x44);
  if (g_hlm_n < 64)
    log_printf("[model] Head::LoadModel part=%u this=%p +0x44: %p -> %p rc=%p%s",
               part & 0xff, self, before, after, r,
               after ? "" : "  <<< HEAD STILL NULL");
  g_hlm_n++;
  return r;
}

static void install_head_probe(void) {
  uintptr_t nao = so_symbol(&kotor_mod, "_Z13NewCAurObjectPcS_P9SDL_RWopsS1_");
  if (nao) {
    NewCAurObject_orig = (void *(*)(char *, char *, void *, void *))
        build_thumb_trampoline(nao, thumb_patch_len(nao));
    if (NewCAurObject_orig) {
      hook_thumb(nao, (uintptr_t)&NewCAurObject_probe);
      log_printf("[model] NewCAurObject PROBED: 0x%08x", (unsigned)nao);
    }
  } else {
    log_printf("[model] NewCAurObject symbol missing");
  }

  uintptr_t hlm = so_symbol(&kotor_mod, "_ZN16CSWCAnimBaseHead9LoadModelERK7CResRefh");
  if (hlm) {
    HeadLoadModel_orig = (void *(*)(void *, const void *, unsigned))
        build_thumb_trampoline(hlm, thumb_patch_len(hlm));
    if (HeadLoadModel_orig) {
      hook_thumb(hlm, (uintptr_t)&HeadLoadModel_probe);
      log_printf("[model] CSWCAnimBaseHead::LoadModel PROBED: 0x%08x", (unsigned)hlm);
    }
  } else {
    log_printf("[model] CSWCAnimBaseHead::LoadModel symbol missing");
  }

}

// log73: the module load stalls with NOBODY blocked. The game thread keeps running
// SDL_main's frame loop (the SDL_Delay LR resolves to SDL_main+0x1ccd, the frame
// limiter) at ~40fps forever, and the game never calls pthread_create at all -- the
// thread registry stayed empty -- so there is no worker to be stuck. The module
// load is therefore a state machine driven from the main loop, and it has simply
// stopped advancing: all resource I/O ceases at a fixed point (t=333s here, 665s in
// log72, same place both runs) and never resumes. No crash, no fault.
//
// So stop guessing at the state and read it. KOTOR's pipeline is
//   CServerExoAppInternal::StartNewModule / ExecuteLoadModule
//     -> CSWSModule::LoadModuleStart(name, flag)   @ 0x387025
//     -> ... staged work, progress bar driven by LoadScreenUpdate(a,b,c,d)
//     -> CSWSModule::LoadModuleFinish()            @ 0x388571
// If Start returns but Finish never runs, the bar freezes exactly as observed
// (~20% in the photo). LoadScreenUpdate's arguments are the stage counters, so
// logging them ON CHANGE gives a compact trace of how far the load got and which
// step it died on, without spamming a per-frame call.
//
// NOTE (learned the hard way): every probe
// here declares a void* return and passes it through. If the real function returns
// void the caller ignores r0 and nothing is harmed; if it returns a value we
// preserve it. Declaring `void` is the unsafe choice, not the neutral one.
// Shared helpers for the load/resource probes (defined here so the load probes
// below can use them). CExoString keeps its char* at offset 0.
static const char *exostr(const void *s) {
  const char *p = s ? *(const char *const *)s : NULL;
  return p ? p : "(empty)";
}

static volatile int g_in_lms = 0;

static void dump_res(const char *tag, void *res) {
  const uint32_t *w = (const uint32_t *)res;
  const unsigned char *b = (const unsigned char *)res;
  char txt[0x41];
  for (int i = 0; i < 0x40; i++)
    txt[i] = (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.';
  txt[0x40] = 0;
  log_printf("[res] %s CRes=%p "
             "%08x %08x %08x %08x %08x %08x %08x %08x "
             "%08x %08x %08x %08x %08x %08x %08x %08x  \"%s\"",
             tag, res, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
             w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15], txt);
}

static void *(*LoadModuleStart_orig)(void *self, const void *name, int flag) = NULL;
static void *(*LoadModuleFinish_orig)(void *self) = NULL;
static void *(*LoadScreenUpdate_orig)(int a, int b, int c, int d) = NULL;

static void *LoadModuleStart_probe(void *self, const void *name, int flag) {
  // m_sModuleName lives at CSWSModule+0x5c (LoadModuleStart compares it against
  // the argument at +0x3870b6 and skips AddModuleResources when they match --
  // which is what happens here, because CSWSModule's constructor already
  // registered the resources). The CRes it then demands is at CSWSModule+8.
  const void *cur = (const char *)self + 0x5c;
  void *res = self ? ((void **)self)[2] : NULL;
  log_printf("[load] LoadModuleStart ENTER flag=%d  m_sModuleName=\"%.48s\" "
             "arg=\"%.48s\"  CRes(this+8)=%p",
             flag, exostr(cur), exostr(name), res);
  if (res) dump_res("before Demand", res);
  g_in_lms = 1;
  void *rc = LoadModuleStart_orig(self, name, flag);
  g_in_lms = 0;
  log_printf("[load] LoadModuleStart EXIT rc=%p", rc);
  if (res) dump_res("after Demand", res);
  return rc;
}

static void *LoadModuleFinish_probe(void *self) {
  log_printf("[load] LoadModuleFinish ENTER");
  void *rc = LoadModuleFinish_orig(self);
  log_printf("[load] LoadModuleFinish EXIT rc=%p", rc);
  return rc;
}

static void *LoadScreenUpdate_probe(int a, int b, int c, int d) {
  static int la = -1, lb = -1, lc = -1, ld = -1;
  static unsigned n = 0, since = 0;
  if (a != la || b != lb || c != lc || d != ld) {
    if (n < 256)
      log_printf("[load] LoadScreenUpdate(%d, %d, %d, %d)  [%u calls since last change]",
                 a, b, c, d, since);
    n++; since = 0;
    la = a; lb = b; lc = c; ld = d;
  } else {
    since++;
  }
  return LoadScreenUpdate_orig(a, b, c, d);
}

static void hook_named(const char *sym, uintptr_t probe, void **orig, const char *tag) {
  uintptr_t a = so_symbol(&kotor_mod, sym);
  if (!a) { log_printf("[load] %s symbol missing", tag); return; }
  *orig = (void *)build_thumb_trampoline(a, thumb_patch_len(a));
  if (!*orig) { log_printf("[load] %s trampoline FAILED", tag); return; }
  hook_thumb(a, probe);
  log_printf("[load] %s PROBED: 0x%08x", tag, (unsigned)a);
}

// The barrier in MainLoop counts outstanding async resource requests, so watch the
// two ends of that queue directly: PreSpawnAsync issues a request, RetreiveAsync
// collects a finished one. If issues >> retrieves, the worker never drains it; if
// they balance, the stall is elsewhere and the counter belongs to something else.
static void *(*PreSpawnAsync_orig)(void *self, char *name) = NULL;
static void *(*RetreiveAsync_orig)(void *self, void *req) = NULL;
static unsigned g_pre_n = 0, g_ret_n = 0;

static void *PreSpawnAsync_probe(void *self, char *name) {
  void *rc = PreSpawnAsync_orig(self, name);
  if (g_pre_n < 64)
    log_printf("[async] PreSpawnAsync(\"%.24s\") -> %p  [issued=%u retrieved=%u]",
               name ? name : "?", rc, g_pre_n + 1, g_ret_n);
  g_pre_n++;
  return rc;
}

static void *RetreiveAsync_probe(void *self, void *req) {
  void *rc = RetreiveAsync_orig(self, req);
  if (g_ret_n < 64)
    log_printf("[async] RetreiveAsync(%p) -> %p  [issued=%u retrieved=%u]",
               req, rc, g_pre_n, g_ret_n + 1);
  g_ret_n++;
  return rc;
}

// ---- load-stall probe -------------------------------------------------------
// Decoded from CServerExoAppInternal::MainLoop (libKOTOR +0x3eb94c). Once per
// frame it does, in effect:
//
//   ls = ((void **)g_pAppManager)[5];        // appManager + 0x14, the load state
//   if (ls[14] != 0)          -> bail; an error code is already latched
//   mode = ls[1];             -> only 1, 2 or 3 do anything at all
//   if      (ls[3] == 0)      -> nothing queued
//   else if (ls[3] != ls[2])  -> CSWSModule::LoadModuleInProgress(ls[2], ls[3])
//   else if (ls[5] != 1)      -> CSWSModule::LoadModuleFinish()
//
// LoadModuleInProgress (+0x3884b8) loads exactly ONE area per call via
// CSWSArea::LoadArea and then stores ls[2]+1 back to ls[2]. So ls[2] is progress,
// ls[3] is the target, and Finish only fires when they meet. A frozen bar means
// ls[2] stopped climbing, which is either "MainLoop never reaches the call" or
// "LoadArea stopped succeeding" -- opposite causes. Log the gate itself plus both
// calls so the next run distinguishes them instead of us guessing again.
//
// LoadArea returning 0 is FAILURE (LoadModuleInProgress then tears the area down
// and returns 4, which makes MainLoop call UnloadModule) -- so an all-zero return
// here would show up as an abort, not a hang.
static void *g_appmgr_ptr = NULL;

static void *(*MainLoop_orig)(void *self) = NULL;
static void *(*LoadInProgress_orig)(void *self, int prog, int target) = NULL;
static void *(*LoadArea_orig)(void *self, int a) = NULL;

static void load_state_dump(const char *tag) {
  if (!g_appmgr_ptr) return;
  void *app = *(void **)g_appmgr_ptr;
  if (!app) { log_printf("[load] %s: g_pAppManager is NULL", tag); return; }
  void **ls = (void **)((void **)app)[5];
  if (!ls) { log_printf("[load] %s: appManager[+0x14] is NULL", tag); return; }
  log_printf("[load] %s: mode=%d progress=%d target=%d f20=%d err=%d",
             tag, (int)(intptr_t)ls[1], (int)(intptr_t)ls[2], (int)(intptr_t)ls[3],
             (int)(intptr_t)ls[5], (int)(intptr_t)ls[14]);
}

static unsigned g_ml_n = 0, g_lip_n = 0, g_la_n = 0;

// log81 verdict: 3360 frames were drawn after LoadModuleStart while MainLoop ran
// fewer than 301 times and LoadModuleInProgress/LoadArea ran ZERO times. So the
// server is not being pumped at all. Tracing the two things that can pump it:
//
//   GameUpdate()  (SDL_main +0x18d116/+0x18d1a0) -> CServerExoApp::MainLoop,
//                 gated only on appManager[+8] != NULL
//   UpdateScreen(float,int,int) (+0x3fe098)      -> same, but gated on b == 1
//
// and EVERY one of the 56 UpdateScreen call sites in the binary passes b=0, so
// that branch is dead code: GameUpdate is the only pump. Yet UpdateScreen is the
// only thing calling SDL_GL_SwapWindow during a load, so something is spinning it
// in a nested loop that never returns to SDL_main. Its return address names that
// loop, which is the one fact still missing.
//
// ABI: UpdateScreen's first parameter is a float and the .so is softfp, so it
// arrives in r0, not s0. Declaring it `float` would make our hardfp build read s0
// and shift b/c by one register.
static void *(*UpdateScreen_orig)(uint32_t a, int b, int c) = NULL;
static void *(*GameUpdate_orig)(void) = NULL;
static unsigned g_us_n = 0, g_gu_n = 0;

static void *UpdateScreen_probe(uint32_t a, int b, int c) {
  if (g_us_n < 16 || (g_us_n % 200) == 0) {
    uintptr_t lr = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
    log_printf("[load] UpdateScreen #%u (a=0x%08x b=%d c=%d) from off=0x%06x "
               "thid=0x%08x  [GameUpdate=%u MainLoop=%u]",
               g_us_n, (unsigned)a, b, c,
               (unsigned)(lr - kotor_mod.text_base),
               (unsigned)sceKernelGetThreadId(), g_gu_n, g_ml_n);
  }
  g_us_n++;
  return UpdateScreen_orig(a, b, c);
}

static void *GameUpdate_probe(void) {
  if ((g_gu_n % 200) == 0) {
    void *app = g_appmgr_ptr ? *(void **)g_appmgr_ptr : NULL;
    log_printf("[load] GameUpdate #%u appMgr=%p client=%p server=%p "
               "[UpdateScreen=%u MainLoop=%u]",
               g_gu_n, app,
               app ? ((void **)app)[1] : NULL,
               app ? ((void **)app)[2] : NULL,
               g_us_n, g_ml_n);
  }
  g_gu_n++;
  return GameUpdate_orig();
}

static void *MainLoop_probe(void *self) {
  // Dump every one of the first 16 -- log82 showed MainLoop runs exactly TWICE
  // and then never again, so the every-300 throttle hid the interesting call.
  if (g_ml_n < 16 || (g_ml_n % 300) == 0) {
    char t[48];
    snprintf(t, sizeof t, "MainLoop#%u thid=0x%08x", g_ml_n,
             (unsigned)sceKernelGetThreadId());
    load_state_dump(t);
  }
  g_ml_n++;
  return MainLoop_orig(self);
}

// log82: appManager[+8] (the CServerExoApp) is NULL on every GameUpdate, forever,
// so GameUpdate's `cbz r0` skips the server pump and the load can never advance.
// MainLoop ran exactly twice -- around LoadModuleStart -- so the server DID exist
// briefly and was then torn down. CAppManager::DestroyServer has five call sites:
//   +0x1972b2 CClientExoAppInternal::MainLoop        (-> DisplayMainMenu)
//   +0x19db80 CClientExoAppInternal::ShutDownToMainMenu
//   +0x2beaa4 CSWGuiSaveLoad::LoadGame
//   +0x3ec156 CServerExoAppInternal::MainLoop        (server shutdown path)
//   +0x3fda50 GameDeinit
// Logging the return address says which one fired instead of us reverse
// engineering all five. CreateServer is logged too, to bracket the lifetime.
static void *(*CreateServer_orig)(void *self, int a) = NULL;
static void *(*DestroyServer_orig)(void *self) = NULL;

// log86: modules/END_M01AA.rim, currentgame/ and _s.rim all open cleanly now (the
// OBB fallback works -- CHITIN.key and every .bzf are served from it), yet
// LoadModuleStart still exits rc=1. So CRes::Demand() on the module .ifo still
// returns NULL even though the RIM is readable. The .ifo does NOT come through
// plain file I/O -- it is registered with the resource manager by
// CSWSModule::AddModuleResources -> CExoResMan::AddEncapsulatedResourceFile, and
// only then demanded. Tellingly there was NO file I/O at all inside
// LoadModuleStart's 17 ms window, so the RIM may never be opened.
//
// Watch that whole chain:
//   CSWSModule::AddModuleResources(name)  - does it run, for which module
//   CExoEncapsulatedFile::OpenFile()      - is the RIM actually opened
//   CExoResMan::Demand(CRes*)             - log only NULL returns (the failure)
// Hook CExoResMan::Demand, NOT CRes::Demand: the latter is a 6-instruction thunk
// whose 2nd and 3rd instructions are `ldr r0,[pc,#12]` / `add r0,pc`, and the
// trampoline copies bytes without relocating them, so those would read from the
// wrong address. CExoResMan::Demand is the real implementation it tail-calls and
// its first 8 bytes are position-independent.
// CExoString stores its char* at offset 0 (CExoString::CStr is `ldr r0,[r0]`
// with an empty-string fallback), so *(char**)s is the text.
//
// NB: CExoResMan::AddEncapsulatedResourceFile would be the more direct probe but
// is UNHOOKABLE here -- it is an 8-byte thunk whose body is a PC-relative `b.w`
// into a long-branch veneer. build_thumb_trampoline copies bytes verbatim without
// relocating them, so the copied branch would go somewhere else entirely, and the
// 10-byte patch its 2-mod-4 address demands would resume inside AddKeyTable.
// Always check the first N bytes of a hook target for PC-relative instructions.
static void *(*AddModRes_orig)(void *self, const void *name) = NULL;
static void *(*OpenFile_orig)(void *self) = NULL;
static void *(*Demand_orig)(void *self, void *res) = NULL;
static unsigned g_openfile_n = 0, g_demand_null_n = 0;


static void *AddModRes_probe(void *self, const void *name) {
  log_printf("[res] AddModuleResources(\"%.64s\") ENTER", exostr(name));
  void *rc = AddModRes_orig(self, name);
  log_printf("[res] AddModuleResources(\"%.64s\") EXIT rc=%p  [OpenFile calls=%u]",
             exostr(name), rc, g_openfile_n);
  return rc;
}

static void *OpenFile_probe(void *self) {
  void *rc = OpenFile_orig(self);
  if (g_openfile_n < 48 || !rc)
    log_printf("[res] CExoEncapsulatedFile::OpenFile(self=%p) -> %p  [#%u]",
               self, rc, g_openfile_n + 1);
  g_openfile_n++;
  return rc;
}

// log87: Demand()->NULL is ROUTINE (every optional .txi probe misses), so a flat
// cap of 24 was exhausted at t=105s and the one that matters -- inside
// LoadModuleStart at t=337 -- was never logged. Scope it to the load window
// instead: g_in_lms is armed only while LoadModuleStart runs, where NULL is the
// actual failure. Dump 0x40 bytes so the CRes's ResRef is visible in the ASCII
// column (it is past +0x18, which the earlier 0x20 dump could not reach).


static void *Demand_probe(void *self, void *res) {
  void *rc = Demand_orig(self, res);
  if (!rc && res && (g_in_lms || g_demand_null_n < 4)) {
    dump_res(g_in_lms ? "IN-LoadModuleStart Demand -> NULL" : "Demand -> NULL", res);
    g_demand_null_n++;
  }
  return rc;
}

// log89: the copy theory is dead -- modules/END_M01AA.rim opens at 55565 bytes,
// byte-exact with the OBB's own entry, so the RIM is intact. What the log DOES
// show is that currentgame/END_M01AA.rim is opened `wb` and never once read: no
// `rb` open of the copy appears anywhere in the log. The main module archive is
// therefore never indexed.
//
// Why that is fatal is now settled statically. CResHelper<CResIFO,2014>::SetResRef
// (+0x3856b0) first compares the incoming ResRef against the one it already stores
// at this+0x0c and RETURNS IMMEDIATELY if they match -- so "module" is bound
// exactly once, ever. On that one call it asks CExoResMan::GetResObject(ref, 2014);
// when that misses it `new`s a 168-byte CResGFF, stamps it with the CResIFO vtable
// (+0x59f388 -- exactly the vtable in our dump, so the object we log IS this
// placeholder) and registers it via SetResObject. The placeholder's id (+8) stays
// 0xFFFFFFFF, and CExoResMan::Demand (+0x4c86cc: `ldr r1,[r1,#8]; adds r0,r1,#1;
// beq ret0`) rejects it on sight. A later archive add cannot rescue it unless it
// walks the live CRes objects (UpdateKeyTable).
//
// So the question collapses to one: is the main module RIM ever handed to the
// resource manager at all? AddEncapsulatedResourceFile is the unhookable 8-byte
// thunk (`mov r3,r2; movs r2,#3; b.w <veneer>`) -- but it is only a shim for
// CExoResMan::AddKeyTable(name, type=3, m), which IS a real 396-byte body at
// +0x4c9490: 0-mod-4, prologue push/add r7/stmdb = 8 bytes, no PC-relative ops.
// Hook that and every archive registration in the game names itself.
static unsigned long (*AddKeyTable_orig)(void *, const void *, unsigned long,
                                         unsigned long) = NULL;
static unsigned g_akt_n = 0;

static unsigned long AddKeyTable_probe(void *self, const void *name,
                                       unsigned long type, unsigned long m) {
  uintptr_t lr = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
  unsigned long rc = AddKeyTable_orig(self, name, type, m);
  g_akt_n++;
  log_printf("[res] AddKeyTable(\"%.64s\" type=%lu m=0x%lx) -> rc=0x%lx  "
             "from off=0x%06x  [#%u]", exostr(name), type, m, rc,
             (unsigned)(lr - kotor_mod.text_base), g_akt_n);
  return rc;
}

// log91: the stat() fix was correct in itself but changed nothing -- and the log
// shows **zero** `[FS] stat` lines, so `CExoBaseInternal::GetDirectoryList` never
// reaches its two stat call sites (+0x4b675e / +0x4b676c) at all. It bails
// earlier, or is never called for CURRENTGAME:. Probe the enumeration chain
// end-to-end rather than guessing which of the 3248 bytes bails:
//
//   CExoKeyTable::BuildNewTable        +0x4c5eac  type 1..4 -> tail-calls one of
//     `-> AddDirectoryContents(int)    +0x4c4adc  (type 2, our case)
//          `-> CExoBase::GetDirectoryList        the actual enumerator
//   CExoResMan::GetKeyEntry            +0x4ca62c  the lookup that then misses
//
// All four are 0-mod-4 with an 8-byte position-independent prologue
// (push/add r7/stmdb), so all pass the 3-way trampoline check.
//
// CExoArrayList<CExoString> keeps its count at +4 (GetDirectoryList itself reads
// `[r9,#4]` and compares against 1 at +0x4b678c). CResRef stores its chars at
// offset 0 -- AsyncLoad memcpys the lowercased name straight into the struct
// before passing it -- so a CResRef can be printed as a bounded char array.
// log92: GetKeyEntry answered the big question -- `global`, `mainmenu` and
// `chargen` (all in rims/) resolve as type 3002 (.rim), while `end_m01aa` (in
// modules/) does not, at either 3002 or 3009. So one directory key table
// populates and the other does not, even though BOTH directories are in the OBB
// (rims/ 13 entries, modules/ 235). AddDirectoryContents("MODULES:") itself runs
// and returns 1, so the failure is inside the enumeration.
//
// NB the earlier GetDirectoryList probe printed garbage: this is a NON-STATIC
// member, so `this` occupies r0 and every argument is shifted one register right
// (r1=out, r2=dir, r3=type). The old signature read the array-list pointer as the
// dir string and `this` as the array list -- hence "(empty)" names and a nonsense
// count. Corrected below.
//
// CExoArrayList keeps its count at +4 (GetDirectoryList reads `[r9,#4]` and
// compares against 1 at +0x4b678c).
static void *(*GetDirList_orig)(void *, void *, const void *, unsigned,
                                int, int, int) = NULL;
static void *(*AddDirContents_orig)(void *, int) = NULL;
static void *(*GetKeyEntry_orig)(void *, const void *, unsigned, void *, void *) = NULL;
static void *(*AddKey_orig)(void *, const void *, unsigned, unsigned, int) = NULL;
static unsigned g_gdl_n = 0, g_adc_n = 0, g_gke_n = 0, g_addkey_n = 0;

static void *GetDirList_probe(void *self, void *out, const void *dir,
                              unsigned type, int a, int b, int c) {
  void *rc = GetDirList_orig(self, out, dir, type, a, b, c);
  int n = out ? ((int *)out)[1] : -1;
  if (g_gdl_n < 64)
    log_printf("[res] GetDirectoryList(\"%.64s\" type=%u %d,%d,%d) -> rc=%p  "
               "count=%d  [#%u]", exostr(dir), type, a, b, c, rc, n, g_gdl_n + 1);
  // log93 pinned the failure to CURRENTGAME: -- count=1 yet not one
  // AddKey(tbl="CURRENTGAME:") in the entire log. Print the names for any short
  // list so we can see WHICH name the loop rejects. Elements are CExoString,
  // stride 8 (AddDirectoryContents walks with `adds r6,#8`), char* at offset 0;
  // the array base is the list's first word.
  // Cap 16, not 8, so RIMS: (12 entries, and known-good -- its keys DO land)
  // dumps too and gives a positive control for what a name is supposed to look
  // like next to the one currentgame entry that gets rejected.
  if (n > 0 && n <= 16) {
    const char *base = (const char *)((void **)out)[0];
    if (base)
      for (int i = 0; i < n; i++)
        log_printf("[res]    entry[%d] = \"%.64s\"", i, exostr(base + i * 8));
  }
  g_gdl_n++;
  return rc;
}

// How many keys each table actually ends up with -- the direct measure of "did
// this directory enumerate". CResRef stores its chars at offset 0.
static void *AddKey_probe(void *self, const void *resref, unsigned type,
                          unsigned id, int a) {
  void *rc = AddKey_orig(self, resref, type, id, a);
  // CURRENTGAME: is the table under test, so never throttle it; everything else
  // is background and gets a cap (log93 emitted 1200 AddKey lines and the run
  // crawled).
  const char *tbl = exostr((const char *)self + 0x20);
  int watched = tbl && strstr(tbl, "CURRENTGAME");
  if (watched || g_addkey_n < 24 ||
      ((type == 3002 || type == 3009) && g_addkey_n < 300)) {
    char n[17];
    memcpy(n, resref, 16);
    n[16] = 0;
    log_printf("[res] AddKey(tbl=\"%.32s\", \"%s\" type=%u id=%u) -> %p  [#%u]",
               exostr((const char *)self + 0x20), n, type, id, rc,
               g_addkey_n + 1);
  }
  g_addkey_n++;
  return rc;
}

static void *AddDirContents_probe(void *self, int a) {
  void *rc = AddDirContents_orig(self, a);
  if (g_adc_n < 64)
    log_printf("[res] AddDirectoryContents(\"%.64s\", %d) -> rc=%p  [#%u]",
               exostr((const char *)self + 0x20), a, rc, g_adc_n + 1);
  g_adc_n++;
  return rc;
}

static void *GetKeyEntry_probe(void *self, const void *resref, unsigned type,
                               void *tbl, void *ent) {
  uintptr_t lr = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
  void *rc = GetKeyEntry_orig(self, resref, type, tbl, ent);
  // 3002 = .rim, 3009 = .rsv -- the two AsyncLoad gates. Rare, so log them all.
  if ((type == 3002 || type == 3009) && g_gke_n < 128) {
    char n[17];
    memcpy(n, resref, 16);
    n[16] = 0;
    log_printf("[res] GetKeyEntry(\"%s\" type=%u) -> %p  from off=0x%06x  [#%u]",
               n, type, rc, (unsigned)(lr - kotor_mod.text_base), g_gke_n + 1);
    g_gke_n++;
  }
  return rc;
}

static void *CreateServer_probe(void *self, int a) {
  uintptr_t lr = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
  void *rc = CreateServer_orig(self, a);
  void *app = g_appmgr_ptr ? *(void **)g_appmgr_ptr : NULL;
  log_printf("[load] CAppManager::CreateServer(%d) -> %p  from off=0x%06x  "
             "server now=%p", a, rc, (unsigned)(lr - kotor_mod.text_base),
             app ? ((void **)app)[2] : NULL);
  return rc;
}

static void *DestroyServer_probe(void *self) {
  uintptr_t lr = (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1;
  void *app = g_appmgr_ptr ? *(void **)g_appmgr_ptr : NULL;
  log_printf("[load] CAppManager::DestroyServer() from off=0x%06x  server was=%p "
             "[GameUpdate=%u MainLoop=%u UpdateScreen=%u]",
             (unsigned)(lr - kotor_mod.text_base),
             app ? ((void **)app)[2] : NULL, g_gu_n, g_ml_n, g_us_n);
  load_state_dump("at DestroyServer");
  return DestroyServer_orig(self);
}

static void *LoadInProgress_probe(void *self, int prog, int target) {
  void *rc = LoadInProgress_orig(self, prog, target);
  if (g_lip_n < 64 || rc)
    log_printf("[load] LoadModuleInProgress(progress=%d, target=%d) -> rc=%p  [#%u]",
               prog, target, rc, g_lip_n + 1);
  g_lip_n++;
  return rc;
}

static void *LoadArea_probe(void *self, int a) {
  void *rc = LoadArea_orig(self, a);
  if (g_la_n < 64 || !rc)
    log_printf("[load] CSWSArea::LoadArea(%d) -> rc=%p  [#%u]", a, rc, g_la_n + 1);
  g_la_n++;
  return rc;
}

/* --- sound: where does the chain stop? --------------------------------------
 *
 * Audio has never made a sound on hardware. Across log101/log102 the only [snd]
 * lines were "decoder ready" and "output up" -- FMOD::System::createSound was
 * never called even once, and no individual sound file was ever opened. So the
 * backend was never the problem; something upstream never asks for a sound.
 *
 * What the disassembly already settles:
 *   - FModAudioSystem::InitSystem DID run (it is what called audio_start).
 *   - FModAudioSystem::CreateSound (+0x73220, port) has NO guard on the system
 *     handle -- it walks its cache map then goes straight to createSound. So it
 *     was never called; the gate is in libKOTOR, above the companion.
 *   - Sound reaches the companion by exactly two routes:
 *       CExoSoundSourceInternal::Demand()          -> CreateSound   (SFX; bails
 *           early if this->m_pRes (+8) is NULL or CRes::Demand() returns 0)
 *       CExoStreamingSoundSourceInternal::InitializeSource() -> CreateStream
 *           (music/VO, via an SDL_RWops -- our RWFromFile chain)
 *   - CExoSound(unsigned char, unsigned char, int, int) is built at the end of
 *     CClientExoAppInternal::InitializeSoundOptions (libKOTOR +0x19bc38). Its
 *     args are NOT what an earlier pass here guessed. Reading +0x19beac:
 *         uxtb r1,r8   <- [Sound Options] "Number 2D Voices"  (default 24)
 *         uxtb r2,r6   <- [Sound Options] "Number 3D Voices"  (default 16)
 *         clz r0,r9 ; lsrs r0,r0,#5 ; str r0,[sp]   <- 4th arg = (r9 == 0)
 *     So arg1/arg2 are VOICE COUNTS with sane nonzero defaults, and the 4th arg
 *     is the real sound-enabled boolean.
 *   - r9 comes straight from [Sound Options] "Sound Init":
 *         read "Sound Init" -> r4   (ReadIniEntry fails => r4 = 0)
 *         immediately WRITE "Sound Init" = 1
 *         r4 != 0  -> r9 = 1 -> 4th arg 0 -> SOUND DISABLED
 *         r4 == 0  -> r9 = 0 -> 4th arg 1 -> sound enabled
 *         ...and at the very end of the function, WRITE "Sound Init" = 0.
 *     That is a crash-guard: the game marks "I am about to init sound", and if
 *     it finds that mark still set on the next boot it assumes sound init killed
 *     the process last time and silently runs mute forever after.
 *   - We resolve swkotor.ini to ux0:data/kotor/swkotor.ini and log103 shows it
 *     touched 15x, so the writes have a real destination. RULED OUT on hardware
 *     2026-07-31: the card's ini has "Sound Init=0", so the 4th ctor arg is 1
 *     and sound is enabled at this level. The gate is elsewhere.
 *
 * The far better candidate, found by scanning every reference in .text: the
 * global g_bDisableSound (libKOTOR .bss +0xb3e030, GOT slot 0x5a38e8). It has
 * exactly 12 referents and exactly ONE writer -- _Z8GameInitv at +0x3fd056:
 *       ReadIniEntry(swkotor.ini, [Sound Options], "Disable Sound") -> r4
 *       r4 == 0 (key absent) -> leave g_bDisableSound at its .bss 0, and write
 *                               "Disable Sound=0" back to the ini
 *       r4 != 0              -> g_bDisableSound = (value.AsINT() != 0)
 * Every other referent only reads it, and each read is a hard bail:
 *       CExoSoundInternal::Initialize  +0x4db766  ==1 -> return 0, does nothing
 *       CExoSound::CExoSound           +0x4daa66  !=0 -> never builds m_pInternal
 *       CExoSoundSource::CExoSoundSource        !=0 -> m_pInternal = NULL
 *       SDL_main main loop             +0x18d10a  !=0 -> skips UpdateSystem()
 * One flag therefore suppresses the entire subsystem with no error anywhere,
 * which is exactly the observed symptom. So log the flag itself rather than
 * inferring it: g_bDisableSound is an exported OBJECT, we can just read it.
 */
static int *g_pDisableSound = NULL;
static void *(*GameInit_orig)(void) = NULL;

static void *GameInit_probe(void) {
  void *rc = GameInit_orig();
  if (g_pDisableSound)
    log_printf("[snd?] after GameInit: g_bDisableSound = %d  %s", *g_pDisableSound,
               *g_pDisableSound
                   ? "<<< SOUND IS OFF: swkotor.ini [Sound Options] Disable Sound is nonzero"
                   : "(sound not disabled by the global)");
  return rc;
}
/*
 *
 * Hence probes rather than another guess: one run shows which link breaks. These
 * are pure log lines -- no threads, no mixer, nothing that could touch frame rate.
 * Every probe passes the callee's return value through: a void-declared hook on a
 * value-returning function was itself a crash once.
 */
static void *(*ExoSound_ctor_orig)(void *, unsigned, unsigned, int, int) = NULL;
static void *(*ExoSoundInit_orig)(void *, unsigned, unsigned, int, int) = NULL;
static void *(*SndDemand_orig)(void *) = NULL;
static void *(*StreamInit_orig)(void *) = NULL;
static void *(*FmodCreateSound_orig)(void *, char *, int, void *, unsigned, int, int) = NULL;
static void *(*FmodCreateStream_orig)(void *, char *, void *, int, int, int, int, int) = NULL;
static void *(*FmodPlaySound_orig)(void *, int) = NULL;

static void *ExoSound_ctor_probe(void *self, unsigned a, unsigned b, int c, int d) {
  log_printf("[snd?] CExoSound(n2DVoices=%u n3DVoices=%u, %d, soundEnabled=%d)  <<< "
             "soundEnabled==0 means \"Sound Init\" was left set in swkotor.ini",
             a & 0xff, b & 0xff, c, d);
  return ExoSound_ctor_orig(self, a, b, c, d);
}
static void *ExoSoundInit_probe(void *self, unsigned a, unsigned b, int c, int d) {
  void *rc = ExoSoundInit_orig(self, a, b, c, d);
  log_printf("[snd?] CExoSoundInternal::Initialize(%u, %u, %d, %d) -> %p",
             a & 0xff, b & 0xff, c, d, rc);
  return rc;
}
// Upstream of Demand: does the game ever ASK for a sound at all? If these two
// stay silent, nothing below them can ever fire and the gate is higher than the
// sound system. CResRef is a fixed char[16], not necessarily NUL-terminated.
static void *(*SndSrcCtor_orig)(void *, const void *) = NULL;
static void *(*SndSrcPlay_orig)(void *) = NULL;

static void *SndSrcCtor_probe(void *self, const void *resref) {
  static unsigned n = 0;
  if (n < 40) {
    char nm[17] = {0};
    if (resref) memcpy(nm, resref, 16);
    for (int i = 0; i < 16; i++)
      if (nm[i] && (nm[i] < 0x20 || nm[i] > 0x7e)) nm[i] = '.';
    log_printf("[snd?] CExoSoundSource(\"%s\") #%u", nm, n);
  }
  n++;
  return SndSrcCtor_orig(self, resref);
}
static void *SndSrcPlay_probe(void *self) {
  static unsigned n = 0;
  void *internal = self ? *(void **)((char *)self + 4) : NULL;  // m_pInternal
  if (n < 40)
    log_printf("[snd?] CExoSoundSource::Play() #%u m_pInternal=%p%s", n, internal,
               internal ? "" : "  <<< NULL internal, nothing can play");
  n++;
  return SndSrcPlay_orig(self);
}

static void *SndDemand_probe(void *self) {
  void *res = self ? *(void **)((char *)self + 8) : NULL;   // m_pRes: NULL == early bail
  void *rc = SndDemand_orig(self);
  static unsigned n = 0;
  if (n < 40)
    log_printf("[snd?] SoundSource::Demand #%u m_pRes=%p -> %p%s", n, res, rc,
               res ? "" : "  <<< no CRes, cannot reach CreateSound");
  n++;
  return rc;
}
static void *StreamInit_probe(void *self) {
  void *rc = StreamInit_orig(self);
  static unsigned n = 0;
  if (n < 40) log_printf("[snd?] StreamingSource::InitializeSource #%u -> %p", n, rc);
  n++;
  return rc;
}
static unsigned g_nclose = 0, g_nrelease = 0;   /* stream/sound teardown counts */

static void *FmodCreateSound_probe(void *self, char *name, int id, void *data,
                                   unsigned size, int e, int f) {
  static unsigned n = 0;
  if (n < 40)
    log_printf("[snd?] FMod::CreateSound #%u \"%s\" id=%d data=%p size=%u (%d,%d)",
               n, name ? name : "?", id, data, size, e, f);
  n++;
  return FmodCreateSound_orig(self, name, id, data, size, e, f);
}
static void *FmodCreateStream_probe(void *self, char *name, void *rw, int c, int d,
                                    int e, int f, int g) {
  static unsigned n = 0;
  if (n < 40)
    log_printf("[snd?] FMod::CreateStream #%u \"%s\" rw=%p (%d,%d,%d,%d,%d)  "
               "[files open=%d, closes so far=%u]",
               n, name ? name : "?", rw, c, d, e, f, g, io_open_count(), g_nclose);
  n++;
  return FmodCreateStream_orig(self, name, rw, c, d, e, f, g);
}
/* Does the companion ever give a stream's OBB handle back?
 *
 * log113: open files climb 34 -> 56 as cumulative streams go 3 -> 40 and never
 * fall, then fopen fails and the app wedges. Each CreateStream holds an OBB
 * SDL_RWops. These two probes settle which fix is needed:
 *   CloseStream never fires        -> the game holds streams forever; the lever
 *                                     is stream lifetime (our placeholder length)
 *   CloseStream fires but handles stay -> the companion's OBB path leaks, and
 *                                     streams must not go through it at all
 * Log-only, and each line carries the live handle count so open/close and the
 * handle total can be read off one line. */
static void *(*FmodCloseStream_orig)(void *, unsigned) = NULL;
static void *(*FmodReleaseSound_orig)(void *, int) = NULL;

static void *FmodCloseStream_probe(void *self, unsigned h) {
  void *rc = FmodCloseStream_orig(self, h);
  g_nclose++;
  if (g_nclose < 60 || (g_nclose & 15) == 0)
    log_printf("[snd?] FMod::CloseStream #%u handle=%u  [files open=%d]",
               g_nclose, h, io_open_count());
  return rc;
}
static void *FmodReleaseSound_probe(void *self, int id) {
  void *rc = FmodReleaseSound_orig(self, id);
  g_nrelease++;
  if (g_nrelease < 40 || (g_nrelease & 63) == 0)
    log_printf("[snd?] FMod::ReleaseSound #%u id=%d  [files open=%d]",
               g_nrelease, id, io_open_count());
  return rc;
}

static void *FmodPlaySound_probe(void *self, int id) {
  static unsigned n = 0;
  if (n < 40) log_printf("[snd?] FMod::PlaySound #%u id=%d", n, id);
  n++;
  return FmodPlaySound_orig(self, id);
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
  log_printf("[snd?] %s PROBED: 0x%08x", tag, (unsigned)a);
}

// Dump the persisted ini so the log records the "Sound Init" value the game is
// about to read, independent of whatever the ctor probe reports. Read-only --
// we are still diagnosing, not fixing.
static void dump_ini(const char *path) {
  SceIoStat st;
  memset(&st, 0, sizeof(st));
  if (sceIoGetstat(path, &st) < 0) {
    log_printf("[snd?] ini ABSENT: %s  (=> defaults, sound should be ENABLED)", path);
    return;
  }
  log_printf("[snd?] ini PRESENT: %s size=%lld", path, (long long)st.st_size);
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0) { log_printf("[snd?]   open failed: 0x%08x", (unsigned)fd); return; }
  char buf[2049];
  int n = sceIoRead(fd, buf, sizeof(buf) - 1);
  sceIoClose(fd);
  if (n <= 0) { log_printf("[snd?]   read failed/empty: %d", n); return; }
  buf[n] = '\0';
  // Line-by-line so the log stays readable and CRLF does not wreck it.
  char *p = buf;
  while (*p) {
    char *e = p;
    while (*e && *e != '\n' && *e != '\r') e++;
    char save = *e;
    *e = '\0';
    if (*p) log_printf("[snd?]   | %s", p);
    *e = save;
    while (*e == '\n' || *e == '\r') e++;
    p = e;
  }
}

static void install_sound_probe(void) {
  dump_ini("ux0:data/kotor/swkotor.ini");
  dump_ini("ux0:data/kotor/swKotor.ini");
  // The one global that can suppress all of sound. GameInit is its only writer,
  // so read it before (should be .bss 0) and again right after GameInit returns.
  g_pDisableSound = (int *)so_symbol(&kotor_mod, "g_bDisableSound");
  log_printf("[snd?] g_bDisableSound @ %p = %d (pre-GameInit)", g_pDisableSound,
             g_pDisableSound ? *g_pDisableSound : -1);
  hook_named("_Z8GameInitv", (uintptr_t)&GameInit_probe,
             (void **)&GameInit_orig, "GameInit");
  hook_named("_ZN9CExoSoundC1Ehhii", (uintptr_t)&ExoSound_ctor_probe,
             (void **)&ExoSound_ctor_orig, "CExoSound::CExoSound");
  hook_named("_ZN17CExoSoundInternal10InitializeEhhii", (uintptr_t)&ExoSoundInit_probe,
             (void **)&ExoSoundInit_orig, "CExoSoundInternal::Initialize");
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
  hook_named_port("_ZN15FModAudioSystem11CloseStreamEm", (uintptr_t)&FmodCloseStream_probe,
                  (void **)&FmodCloseStream_orig, "FModAudioSystem::CloseStream");
  hook_named_port("_ZN15FModAudioSystem12ReleaseSoundEi", (uintptr_t)&FmodReleaseSound_probe,
                  (void **)&FmodReleaseSound_orig, "FModAudioSystem::ReleaseSound");
}

static void install_load_probe(void) {
  g_appmgr_ptr = (void *)so_symbol(&kotor_mod, "g_pAppManager");
  log_printf("[load] g_pAppManager @ %p", g_appmgr_ptr);
  // Let the JOYBUTTON log line report what libKOTOR did with the press. All
  // three are plain .bss globals in libKOTOR; a missing one just drops that
  // figure from the line.
  sdl_gamepad_probe_init(so_symbol(&kotor_mod, "pressedGamepadButtons"),
                         so_symbol(&kotor_mod, "pressedGamepadButtonsThisFrame"),
                         so_symbol(&kotor_mod, "gamepadButtonById"));
  hook_named("_ZN21CServerExoAppInternal8MainLoopEv",
             (uintptr_t)&MainLoop_probe, (void **)&MainLoop_orig,
             "CServerExoAppInternal::MainLoop");
  hook_named("_Z10GameUpdatev",
             (uintptr_t)&GameUpdate_probe, (void **)&GameUpdate_orig,
             "GameUpdate");
  hook_named("_Z12UpdateScreenfii",
             (uintptr_t)&UpdateScreen_probe, (void **)&UpdateScreen_orig,
             "UpdateScreen");
  hook_named("_ZN11CAppManager12CreateServerEi",
             (uintptr_t)&CreateServer_probe, (void **)&CreateServer_orig,
             "CAppManager::CreateServer");
  hook_named("_ZN11CAppManager13DestroyServerEv",
             (uintptr_t)&DestroyServer_probe, (void **)&DestroyServer_orig,
             "CAppManager::DestroyServer");
  hook_named("_ZN10CSWSModule18AddModuleResourcesERK10CExoString",
             (uintptr_t)&AddModRes_probe, (void **)&AddModRes_orig,
             "CSWSModule::AddModuleResources");
  hook_named("_ZN20CExoEncapsulatedFile8OpenFileEv",
             (uintptr_t)&OpenFile_probe, (void **)&OpenFile_orig,
             "CExoEncapsulatedFile::OpenFile");
  hook_named("_ZN16CExoBaseInternal16GetDirectoryListEP13CExoArrayListI10CExoStringERKS1_tiii",
             (uintptr_t)&GetDirList_probe, (void **)&GetDirList_orig,
             "CExoBaseInternal::GetDirectoryList");
  hook_named("_ZN12CExoKeyTable6AddKeyERK7CResReftmi",
             (uintptr_t)&AddKey_probe, (void **)&AddKey_orig,
             "CExoKeyTable::AddKey");
  hook_named("_ZN12CExoKeyTable20AddDirectoryContentsEi",
             (uintptr_t)&AddDirContents_probe, (void **)&AddDirContents_orig,
             "CExoKeyTable::AddDirectoryContents");
  hook_named("_ZN10CExoResMan11GetKeyEntryERK7CResReftPP12CExoKeyTablePP14CKeyTableEntry",
             (uintptr_t)&GetKeyEntry_probe, (void **)&GetKeyEntry_orig,
             "CExoResMan::GetKeyEntry");
  hook_named("_ZN10CExoResMan11AddKeyTableERK10CExoStringmm",
             (uintptr_t)&AddKeyTable_probe, (void **)&AddKeyTable_orig,
             "CExoResMan::AddKeyTable");
  hook_named("_ZN10CExoResMan6DemandEP4CRes",
             (uintptr_t)&Demand_probe, (void **)&Demand_orig,
             "CExoResMan::Demand");
  hook_named("_ZN10CSWSModule20LoadModuleInProgressEii",
             (uintptr_t)&LoadInProgress_probe, (void **)&LoadInProgress_orig,
             "CSWSModule::LoadModuleInProgress");
  hook_named("_ZN8CSWSArea8LoadAreaEi",
             (uintptr_t)&LoadArea_probe, (void **)&LoadArea_orig,
             "CSWSArea::LoadArea");
  hook_named("_ZN12IODispatcher13PreSpawnAsyncEPc",
             (uintptr_t)&PreSpawnAsync_probe, (void **)&PreSpawnAsync_orig,
             "IODispatcher::PreSpawnAsync");
  hook_named("_ZN12IODispatcher13RetreiveAsyncEPv",
             (uintptr_t)&RetreiveAsync_probe, (void **)&RetreiveAsync_orig,
             "IODispatcher::RetreiveAsync");
  hook_named("_ZN10CSWSModule15LoadModuleStartERK10CExoStringi",
             (uintptr_t)&LoadModuleStart_probe, (void **)&LoadModuleStart_orig,
             "CSWSModule::LoadModuleStart");
  hook_named("_ZN10CSWSModule16LoadModuleFinishEv",
             (uintptr_t)&LoadModuleFinish_probe, (void **)&LoadModuleFinish_orig,
             "CSWSModule::LoadModuleFinish");
  hook_named("_Z16LoadScreenUpdateiiii",
             (uintptr_t)&LoadScreenUpdate_probe, (void **)&LoadScreenUpdate_orig,
             "LoadScreenUpdate");
}

static void install_gui_probe(void) {
  uintptr_t db = so_symbol(&kotor_mod, "_Z18AurResGetDataBytesmPv");
  if (db) {
    ResDataBytes_orig = (void *(*)(unsigned long, void *))build_thumb_trampoline(db, thumb_patch_len(db));
    if (ResDataBytes_orig) {
      hook_thumb(db, (uintptr_t)&ResDataBytes_probe);
      log_printf("[model] AurResGetDataBytes PROBED: 0x%08x", (unsigned)db);
    }
  } else {
    log_printf("[model] AurResGetDataBytes symbol missing");
  }

  uintptr_t rs = so_symbol(&kotor_mod, "_ZN12IODispatcher8ReadSyncEPc");
  if (rs) {
    ReadSync_orig = (void *(*)(void *, char *))build_thumb_trampoline(rs, thumb_patch_len(rs));
    if (ReadSync_orig) {
      hook_thumb(rs, (uintptr_t)&ReadSync_probe);
      log_printf("[model] IODispatcher::ReadSync(char*) PROBED: 0x%08x", (unsigned)rs);
    }
  } else {
    log_printf("[model] IODispatcher::ReadSync(char*) symbol missing");
  }

  g_models_read = (const volatile int32_t *)so_symbol(&kotor_mod, "g_nModelsRead");
  log_printf("[model] g_nModelsRead @ %p", (void *)g_models_read);

  uintptr_t lm = so_symbol(&kotor_mod, "_ZN12CSWCAnimBase9LoadModelERK7CResRefh");
  if (lm) {
    LoadModel_orig = (void *(*)(void *, const void *, unsigned))
                         build_thumb_trampoline(lm, thumb_patch_len(lm));
    if (LoadModel_orig) {
      hook_thumb(lm, (uintptr_t)&LoadModel_probe);
      log_printf("[model] CSWCAnimBase::LoadModel PROBED: 0x%08x", (unsigned)lm);
    }
  } else {
    log_printf("[model] CSWCAnimBase::LoadModel symbol missing");
  }

  uintptr_t sx = so_symbol(&kotor_mod, "_ZN12CSWGuiObject24ScaleExtentForResolutionEf");
  if (sx) {
    ScaleExt_orig = (void (*)(void *, uint32_t))
                        build_thumb_trampoline(sx, thumb_patch_len(sx));
    if (ScaleExt_orig) {
      hook_thumb(sx, (uintptr_t)&ScaleExt_probe);
      log_printf("[gui] ScaleExtentForResolution PROBED: 0x%08x", (unsigned)sx);
    }
  } else {
    log_printf("[gui] ScaleExtentForResolution symbol missing");
  }

  uintptr_t xl = so_symbol(&kotor_mod, "_ZN12CSWGuiExtent4LoadEP7CResGFFR10CResStruct");
  if (xl) {
    ExtLoad_orig = (int (*)(void *, void *, void *))build_thumb_trampoline(xl, thumb_patch_len(xl));
    if (ExtLoad_orig) {
      hook_thumb(xl, (uintptr_t)&ExtLoad_probe);
      log_printf("[gui] CSWGuiExtent::Load PROBED: 0x%08x", (unsigned)xl);
    }
  } else {
    log_printf("[gui] CSWGuiExtent::Load symbol missing");
  }

  uintptr_t u = so_symbol(&kotor_mod, "_ZN12CAurGUIImage21cm_nGUIBufferSizeUsedE");
  g_gui_buf_used = (const volatile int32_t *)u;
  log_printf("[gui] cm_nGUIBufferSizeUsed @ 0x%08x", (unsigned)u);

  g_scr_w   = (const volatile int32_t *)so_symbol(&kotor_mod, "g_nScreenWidth");
  g_scr_h   = (const volatile int32_t *)so_symbol(&kotor_mod, "g_nScreenHeight");
  g_scr_wp2 = (const volatile int32_t *)so_symbol(&kotor_mod, "_ZN8GLRender19cm_nScreenWidthPow2E");
  g_scr_hp2 = (const volatile int32_t *)so_symbol(&kotor_mod, "_ZN8GLRender20cm_nScreenHeightPow2E");
  log_printf("[gui] screen globals @ w=%p h=%p wp2=%p hp2=%p",
             (void *)g_scr_w, (void *)g_scr_h, (void *)g_scr_wp2, (void *)g_scr_hp2);

  uintptr_t ini = so_symbol(&kotor_mod, "_ZN11CSWGuiImage10InitializeERK12CSWGuiExtentRK17CSWGuiImageParams");
  if (ini) {
    ImgInit_orig = (void (*)(void *, const void *, const void *))
                       build_thumb_trampoline(ini, thumb_patch_len(ini));
    if (ImgInit_orig) {
      hook_thumb(ini, (uintptr_t)&ImgInit_probe);
      log_printf("[gui] CSWGuiImage::Initialize PROBED: 0x%08x", (unsigned)ini);
    }
  } else {
    log_printf("[gui] CSWGuiImage::Initialize symbol missing");
  }

  uintptr_t se = so_symbol(&kotor_mod, "_ZN11CSWGuiImage9SetExtentERK12CSWGuiExtent");
  if (se) {
    size_t se_len = thumb_patch_len(se);
    SetExtent_orig = (void (*)(void *, const void *))build_thumb_trampoline(se, se_len);
    if (SetExtent_orig) {
      hook_thumb(se, (uintptr_t)&SetExtent_probe);
      // patchLen MUST be sampled before hook_thumb -- re-reading it afterwards
      // walks the patched NOP+LDR and reports 10 instead of the 12 actually used
      // (log56 showed exactly that; it was a logging artifact, not a bug).
      log_printf("[gui] CSWGuiImage::SetExtent PROBED: 0x%08x patchLen=%u (text_base=0x%08x)",
                 (unsigned)se, (unsigned)se_len, (unsigned)kotor_mod.text_base);
    }
  } else {
    log_printf("[gui] CSWGuiImage::SetExtent symbol missing");
  }

  uintptr_t d = so_symbol(&kotor_mod, "_ZN11CSWGuiImage4DrawEf");
  if (d) {
    SWImgDraw_orig = (void (*)(void *, uint32_t))build_thumb_trampoline(d, thumb_patch_len(d));
    if (SWImgDraw_orig) {
      hook_thumb(d, (uintptr_t)&SWImgDraw_probe);
      log_printf("[gui] CSWGuiImage::Draw(float) PROBED: 0x%08x", (unsigned)d);
    }
  } else {
    log_printf("[gui] CSWGuiImage::Draw(float) symbol missing");
  }

  uintptr_t f = so_symbol(&kotor_mod, "_ZN12CAurGUIImage11FlushBufferEf");
  if (f) {
    FlushBuf_orig = (void (*)(void *, uint32_t))build_thumb_trampoline(f, thumb_patch_len(f));
    if (FlushBuf_orig) {
      hook_thumb(f, (uintptr_t)&FlushBuf_probe);
      log_printf("[gui] CAurGUIImage::FlushBuffer(float) PROBED: 0x%08x", (unsigned)f);
    }
  } else {
    log_printf("[gui] CAurGUIImage::FlushBuffer(float) symbol missing");
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

  log_printf(">>> init vitaGL on game thread");
  vglSetupRuntimeShaderCompiler(SHARK_OPT_UNSAFE, SHARK_ENABLE, SHARK_ENABLE, SHARK_ENABLE);
  vglInitExtended(0, SCREEN_W, SCREEN_H, MEMORY_VITAGL_THRESHOLD_MB * 1024 * 1024, GL_MSAA_MODE);

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
  sceKernelChangeThreadCpuAffinityMask(0, 0x40000);

  sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
  /* Back panel deliberately NOT sampled: it is where fingers rest while
   * holding the console, and any sampling port becomes SDL finger events. */
  sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &panelInfoFront);
  sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &panelInfoBack);

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
  install_head_probe();
  install_load_probe();
  install_sound_probe();

  // NOTE: vitaGL is initialised on the game thread (see game_main_thread), not
  // here -- GXM context must live on the thread that issues GL calls.

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
