/* bink_patch.c -- stub Bink video (see bink_patch.h) */

#include <vitasdk.h>
#include <stdint.h>
#include <string.h>
#include "bink_patch.h"
#include "config.h"
#include "dynlib.h"
#include "opensl_patch.h"
#include "so_util.h"
#include "log.h"

// MacPlayBinkGL(const char *path, bool a, bool &finished, int c):
// report the movie as finished immediately so callers advance past it.
static int MacPlayBinkGL_stub(const char *path, int a, unsigned char *finished, int c) {
  log_printf("[BINK] skip movie: %s", path ? path : "(null)");
  if (finished)
    *finished = 1;
  return 0;
}

static int bink_stub(void) {
  return 0;
}

typedef struct {
  int active;
  unsigned swaps, frame_intervals, texture_uploads;
  uint64_t start_us, last_swap_us, frame_sum_us, frame_max_us, texture_bytes;
} bink_perf_t;

static bink_perf_t g_bink_perf;

#if BINK_MODE == BINK_MODE_OPENSL_TEST
static void bink_snd_pump_on_swap(void);
#endif

void bink_patch_note_texture_upload(unsigned width, unsigned height) {
  if (!g_bink_perf.active) return;
  g_bink_perf.texture_uploads++;
  g_bink_perf.texture_bytes += (uint64_t)width * height;
}

void bink_patch_on_swap(uint64_t swap_end_us) {
  if (!g_bink_perf.active) return;
  if (g_bink_perf.last_swap_us) {
    uint64_t frame_us = swap_end_us - g_bink_perf.last_swap_us;
    g_bink_perf.frame_sum_us += frame_us;
    if (frame_us > g_bink_perf.frame_max_us) g_bink_perf.frame_max_us = frame_us;
    g_bink_perf.frame_intervals++;
  }
  g_bink_perf.last_swap_us = swap_end_us;
  g_bink_perf.swaps++;
#if BINK_MODE == BINK_MODE_OPENSL_TEST
  bink_snd_pump_on_swap();
#endif
}

#if BINK_MODE == BINK_MODE_OPENSL_TEST
/* The companion's async audio worker services this pump once before the first
 * decoded-frame sequence is published, then never services it again on Vita.
 * Capture the handle at that first call; bink_snd_pump_on_swap drives the same
 * internally locked pump once per later frame-sequence advance. */
#define BINK_SND_PUMP_OFF    0x85670u

static void (*BinkSndPump_orig)(void *bink);

static unsigned g_snd_pump_calls, g_snd_forced_pumps, g_snd_gate_max;
static void *g_snd_bink;
static unsigned g_snd_last_gate;
#define BINK_SND_STOPPED ((void *)(uintptr_t)1)

void bink_patch_stop_audio_pump(void) {
  __atomic_store_n(&g_snd_bink, BINK_SND_STOPPED, __ATOMIC_RELEASE);
}

static void BinkSndPump_trap(void *bink) {
  unsigned n = __atomic_add_fetch(&g_snd_pump_calls, 1, __ATOMIC_RELAXED);
  void *expected = NULL;
  __atomic_compare_exchange_n(&g_snd_bink, &expected, bink, 0,
                              __ATOMIC_RELEASE, __ATOMIC_RELAXED);
  if (n <= 2 && bink) {
    unsigned tracks = *(unsigned *)((char *)bink + 0x11c);
    void *sounds = *(void **)((char *)bink + 0x184);
    unsigned gate = *(unsigned *)((char *)bink + 0x570);
    void *ready = sounds ? *(void **)((char *)sounds + 0x158) : NULL;
    unsigned active = sounds ? *(unsigned char *)((char *)sounds + 0x8f) : 0;
    log_printf("[BINK:PUMP] initial call #%u bink=%p tracks=%u sounds=%p gate=%u "
               "Ready=%p active=%u", n, bink, tracks, sounds, gate, ready, active);
  }
  BinkSndPump_orig(bink);
}

/* The companion starts its async audio worker before the first decoded-frame
 * sequence is published. On Vita that worker services the handle once at gate
 * zero and is never signalled again. The existing pump is internally locked;
 * service it from the movie's owning thread whenever the sequence advances. */
static void bink_snd_pump_on_swap(void) {
  void *bink = __atomic_load_n(&g_snd_bink, __ATOMIC_ACQUIRE);
  if (!bink || bink == BINK_SND_STOPPED || !BinkSndPump_orig) return;
  unsigned gate = *(volatile unsigned *)((char *)bink + 0x570);
  if (gate > g_snd_gate_max) g_snd_gate_max = gate;
  if (!gate || gate == g_snd_last_gate) return;
  g_snd_last_gate = gate;
  g_snd_forced_pumps++;
  BinkSndPump_orig(bink);
}

static void bink_pump_log(void) {
  log_printf("[BINK:PUMP] calls=%u forced=%u gateMax=%u",
              __atomic_load_n(&g_snd_pump_calls, __ATOMIC_RELAXED),
              g_snd_forced_pumps, g_snd_gate_max);
}

static void install_bink_snd_pump(so_module *port_mod) {
  uintptr_t fn = (port_mod->text_base + BINK_SND_PUMP_OFF) | 1u;
  size_t len = thumb_patch_len(fn);
  BinkSndPump_orig = (void *)build_thumb_trampoline(fn, len);
  if (!BinkSndPump_orig) {
    log_printf("[BINK:PUMP] trampoline FAILED @0x%08x", (unsigned)fn);
    return;
  }
  hook_thumb(fn, (uintptr_t)&BinkSndPump_trap);
  log_printf("[BINK:PUMP] hooked @0x%08x len=%u tramp=%p", (unsigned)fn,
             (unsigned)len, (void *)BinkSndPump_orig);
}
#endif

#if BINK_MODE == BINK_MODE_LEGAL_VIDEO || BINK_MODE == BINK_MODE_OPENSL_TEST
#if BINK_MODE == BINK_MODE_OPENSL_TEST
#define BINK_TEST_MOVIE ".\\movies\\01c.bik"
#define BINK_TEST_NAME  "OpenSL"
#else
#define BINK_TEST_MOVIE ".\\movies\\legal.bik"
#define BINK_TEST_NAME  "legal"
#endif

static void (*MacPlayBinkGL_orig)(const char *path, int can_skip,
                                  unsigned char *finished, int arg) = NULL;
static int g_bink_test_played = 0;

static int MacPlayBinkGL_single_test(const char *path, int can_skip,
                                     unsigned char *finished, int arg) {
  if (!g_bink_test_played && MacPlayBinkGL_orig) {
    g_bink_test_played = 1;
    memset(&g_bink_perf, 0, sizeof g_bink_perf);
    g_bink_perf.active = 1;
    g_bink_perf.start_us = sceKernelGetProcessTimeWide();
    int open_before = io_open_count();
    log_printf("[BINK] %s test begin: requested=\"%s\" substitute=\"%s\" "
               "canSkip=%d arg=%d finished=%d",
               BINK_TEST_NAME, path ? path : "(null)", BINK_TEST_MOVIE, can_skip, arg,
               finished ? *finished : -1);
#if BINK_MODE == BINK_MODE_OPENSL_TEST
    __atomic_store_n(&g_snd_bink, NULL, __ATOMIC_RELEASE);
#endif
    MacPlayBinkGL_orig(BINK_TEST_MOVIE, can_skip, finished, arg);
#if BINK_MODE == BINK_MODE_OPENSL_TEST
    bink_patch_stop_audio_pump();
#endif
    g_bink_perf.active = 0;
    if (finished) *finished = 1;
    log_printf("[BINK] %s test end: elapsed=%u ms swaps=%u frame avg/max=%u/%u ms "
               "lumaUploads=%u/%u KB files=%d->%d finished=%d",
               BINK_TEST_NAME,
               (unsigned)((sceKernelGetProcessTimeWide() - g_bink_perf.start_us) / 1000u),
               g_bink_perf.swaps,
               g_bink_perf.frame_intervals ?
                 (unsigned)((g_bink_perf.frame_sum_us / g_bink_perf.frame_intervals) / 1000u) : 0,
               (unsigned)(g_bink_perf.frame_max_us / 1000u),
               g_bink_perf.texture_uploads,
               (unsigned)(g_bink_perf.texture_bytes / 1024u),
                open_before, io_open_count(), finished ? *finished : -1);
#if BINK_MODE == BINK_MODE_OPENSL_TEST
    bink_opensl_log_stats();
    bink_pump_log();
#endif
    log_flush();
    return 0;
  }
  return MacPlayBinkGL_stub(path, can_skip, finished, arg);
}
#endif

void bink_patch(so_module *port_mod) {
  if (!port_mod)
    return;

  uintptr_t play = so_symbol(port_mod, "_Z13MacPlayBinkGLPKcbRbi");
  uintptr_t shaders = so_symbol(port_mod, "_Z20MacCreateBinkShadersv");
  const char *mode = BINK_MODE == BINK_MODE_OPENSL_TEST ? "OPENSL_TEST" :
                     BINK_MODE == BINK_MODE_LEGAL_VIDEO ? "LEGAL_VIDEO" :
                     BINK_MODE == BINK_MODE_SHADER_TEST ? "SHADER_TEST" : "SKIP";
  log_printf("[BINK] mode=%s play=0x%08x shaders=0x%08x",
             mode, (unsigned)play, (unsigned)shaders);

  if (!play || !shaders) {
    log_printf("[BINK] missing companion export -- forcing available hooks to skip");
    hook_addr(play, (uintptr_t)&MacPlayBinkGL_stub);
    hook_addr(shaders, (uintptr_t)&bink_stub);
    return;
  }

  // Shader-test mode leaves the real shader initializer enabled while every
  // movie is skipped. Legal-video mode additionally permits exactly one call to
  // the original player, substituting the only shipped movie with no audio.
#if BINK_MODE == BINK_MODE_SKIP
  hook_addr(play, (uintptr_t)&MacPlayBinkGL_stub);
  hook_addr(shaders, (uintptr_t)&bink_stub);
#elif BINK_MODE == BINK_MODE_SHADER_TEST
  hook_addr(play, (uintptr_t)&MacPlayBinkGL_stub);
  log_printf("[BINK] real shader initialization enabled; movie playback still skipped");
#elif BINK_MODE == BINK_MODE_LEGAL_VIDEO || BINK_MODE == BINK_MODE_OPENSL_TEST
  size_t patch_len = thumb_patch_len(play);
  MacPlayBinkGL_orig = (void *)build_thumb_trampoline(play, patch_len);
  if (!MacPlayBinkGL_orig) {
    log_printf("[BINK] player trampoline failed -- all movies will be skipped");
    hook_addr(play, (uintptr_t)&MacPlayBinkGL_stub);
    return;
  }
  hook_addr(play, (uintptr_t)&MacPlayBinkGL_single_test);
  log_printf("[BINK] one-movie test enabled: trampoline=%p patchLen=%u; "
             "first request becomes %s, later requests skipped",
             (void *)MacPlayBinkGL_orig, (unsigned)patch_len, BINK_TEST_MOVIE);
#if BINK_MODE == BINK_MODE_OPENSL_TEST
  install_bink_snd_pump(port_mod);
#endif
#endif

  // DO NOT stub MacDecompress. The `Mac` prefix makes it look like a sibling of
  // MacPlayBinkGL/MacCreateBinkShaders, and bring-up stubbed it on that basis --
  // it is actually the OBB's *resource* decompressor, and stubbing it silently
  // broke every LZMA-compressed asset in the game:
  //   MacDecompress(dest, destLen, src, srcLen)  @ libandroid_port+0x57244
  //     -> LzmaUncompress(dest, &destLen, src+5, &srcLen-5, props=src, 5)
  //     -> returns 1 on success, 0 on failure
  // `data/*.bzf` in the OBB are LZMA-compressed BIFs (5-byte props + stream per
  // entry), so every .bzf resource routes
  // through here. bink_stub returns 0 (= failure) and never touches `dest`, so
  // the resource manager handed the game a correctly-sized, correctly-tagged
  // block still holding newlib free-list bytes (`10 40 40 81 ...`). That is why
  // chargen's models were NULL: IODispatcher::ReadSync rejects the .mdl because
  // byte 0 of that garbage is not the required 0x00. Only assets served from
  // uncompressed RIM/module containers (gui3D_room, mainmenu) survived, which is
 // why the main menu looked fine.
}
