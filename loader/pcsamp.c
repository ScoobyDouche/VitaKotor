/* pcsamp.c -- see pcsamp.h */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vitasdk.h>
#include <kubridge.h>

#include "config.h"
#include "log.h"
#include "pcsamp.h"
#include "so_util.h"
#include "threads.h"

extern so_module kotor_mod, port_mod;

#define TAB_N        8192        /* power of two */
#define WINDOW_US    (15u * 1000u * 1000u)
#define TOP_PC       400
#define TOP_LR       150
#define TOP_STK      400
#define STK_SCAN     (6u * 1024u)   /* bytes of game stack walked per sample */
#define STK_MAX      48             /* distinct return addresses kept per sample */

typedef struct { uint32_t key, n; } Slot;
static Slot g_pc[TAB_N], g_lr[TAB_N], g_stk[TAB_N];

typedef struct { uintptr_t lo, hi; int on; } Range;
static Range g_rng[2];               /* 0: libKOTOR, 1: libandroid_port */

static volatile SceUID *g_game_thid_p;
static uintptr_t g_stk_lo, g_stk_hi; /* the game thread's stack */
static unsigned g_tick_us;

static volatile uint32_t g_armed;    /* text is (being) made non-executable */
static volatile uint32_t g_game, g_other, g_dropped, g_spurious;
static volatile uint32_t g_in_mod[2], g_depth_sum;
static volatile uint32_t g_ticks, g_missed;
static KuKernelExceptionHandler g_prev;

static char g_area[24] = "?";        /* written by the opener, read by reports */
static volatile uint32_t g_swaps;

void pcsamp_note_module(const char *path) {
  if (!path) return;
  const char *b = strrchr(path, '/');
  b = b ? b + 1 : path;
  char name[sizeof g_area];
  size_t n = 0;
  while (b[n] && b[n] != '.' && n < sizeof name - 1) {
    char ch = b[n];
    name[n++] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
  }
  name[n] = 0;
  if (n > 2 && !strcmp(name + n - 2, "_s")) name[n - 2] = 0;   /* tar_m04aa_s.rim */
  if (name[0]) memcpy(g_area, name, sizeof name);
}

void pcsamp_on_swap(void) { g_swaps++; }

const char *pcsamp_area(void) { return g_area; }

static int protect(int exec) {
  int r = 0;
  for (int i = 0; i < 2; i++) {
    if (!g_rng[i].on) continue;
    int e = kuKernelMemProtect((void *)g_rng[i].lo, g_rng[i].hi - g_rng[i].lo,
                               KU_KERNEL_PROT_READ | (exec ? KU_KERNEL_PROT_EXEC : 0));
    if (e < 0) r = e;
  }
  return r;
}

static int mod_of(uint32_t a) {
  for (int i = 0; i < 2; i++)
    if (g_rng[i].on && a - g_rng[i].lo < g_rng[i].hi - g_rng[i].lo) return i;
  return -1;
}

static void tally(Slot *t, uint32_t key) {
  uint32_t i = (key * 2654435761u) >> 19;
  for (unsigned probe = 0; probe < 64; probe++, i = (i + 1) & (TAB_N - 1)) {
    if (t[i].key == key) { t[i].n++; return; }
    if (!t[i].key) { t[i].key = key; t[i].n = 1; return; }
  }
  g_dropped++;
}

/* A word on the stack is taken as a return address only if it is a Thumb
 * address in one of the two libraries and the instruction before it is a
 * call: BL / BLX imm (32-bit) or BLX reg (16-bit). Stale words left by
 * returned calls pass this too, so the walk over-counts a little; it is a
 * heuristic profile of who is on the stack, not an unwinder. */
static int is_return(uint32_t w) {
  if (!(w & 1) || mod_of(w) < 0 || mod_of(w - 5) < 0) return 0;   /* the call is inside too */
  const uint16_t *at = (const uint16_t *)(uintptr_t)(w & ~1u);
  uint16_t h = at[-1];
  if ((h & 0xFF87) == 0x4780) return 1;                         /* blx rN */
  uint16_t h1 = at[-2];
  return (h1 & 0xF800) == 0xF000 && (h & 0xC000) == 0xC000;     /* bl / blx */
}

/* Inclusive attribution: every distinct call site found on the game stack
 * gets one count for this sample. A function's inclusive share is then its
 * own PC samples plus the samples that found one of its call sites. */
static void walk_stack(uint32_t sp) {
  uint32_t seen[STK_MAX];
  unsigned n = 0;
  uintptr_t end = sp + STK_SCAN;
  if (end > g_stk_hi) end = g_stk_hi;
  for (uintptr_t p = (sp + 3) & ~(uintptr_t)3; p < end && n < STK_MAX; p += 4) {
    uint32_t w = *(const uint32_t *)p;
    if (!is_return(w)) continue;
    unsigned k = 0;
    while (k < n && seen[k] != w) k++;
    if (k < n) continue;
    seen[n++] = w;
    tally(g_stk, w);
  }
  g_depth_sum += n;
}

/* Every prefetch abort lands here first. One inside a protected library while
 * the sampler has the text locked is a sample: give execute back and return,
 * and the instruction runs again. Any thread can take it; only the game
 * thread's are tallied. Two threads can fault on the same lock, so a fault in
 * the text after the lock has been lifted is let through as well -- but only
 * a few in a row, so a real jump into garbage still reaches the crash handler. */
static void pcs_handler(KuKernelExceptionContext *c) {
  uint32_t pc = c->pc;
  int m = mod_of(pc);
  if (m >= 0) {
    if (g_armed) {
      protect(1);
      g_armed = 0;
      g_spurious = 0;
      if (c->sp - g_stk_lo < g_stk_hi - g_stk_lo) {
        tally(g_pc, pc & ~1u);
        tally(g_lr, c->lr);
        walk_stack(c->sp);
        g_in_mod[m]++;
        g_game++;
      } else {
        g_other++;
      }
      return;
    }
    if (g_spurious++ < 4) { protect(1); return; }
  }
  if (g_prev) g_prev(c);
}

static void name_addr(uint32_t a, char *out, size_t n) {
  if (a - kotor_mod.text_base < kotor_mod.text_size)
    snprintf(out, n, "k+%x", (unsigned)(a - kotor_mod.text_base));
  else if (a - port_mod.text_base < port_mod.text_size)
    snprintf(out, n, "p+%x", (unsigned)(a - port_mod.text_base));
  else
    snprintf(out, n, "%x", (unsigned)a);
}

static int by_n(const void *a, const void *b) {
  uint32_t x = ((const Slot *)a)->n, y = ((const Slot *)b)->n;
  return x < y ? 1 : x > y ? -1 : 0;
}

/* Same line format as sampler.c, so tools/samp_report.py reads both. */
static void report(const char *what, Slot *t, int top) {
  static Slot copy[TAB_N];
  memcpy(copy, t, sizeof copy);
  memset(t, 0, sizeof copy);
  qsort(copy, TAB_N, sizeof *copy, by_n);
  char line[400];
  int len = 0;
  for (int i = 0; i < top && copy[i].n >= 2; i++) {
    char nm[24];
    name_addr(copy[i].key, nm, sizeof nm);
    len += snprintf(line + len, sizeof line - (size_t)len, " %s:%u", nm, (unsigned)copy[i].n);
    if (len > 300 || i == top - 1 || copy[i + 1].n < 2) {
      log_printf("[pcs]   %s%s", what, line);
      len = 0;
    }
  }
}

static uint64_t game_run_us(void) {
  SceKernelThreadInfo ti;
  memset(&ti, 0, sizeof ti);
  ti.size = sizeof ti;
  if (sceKernelGetThreadInfo(*g_game_thid_p, &ti) < 0) return 0;
  return (uint64_t)ti.runClocks;
}

static int find_game_stack(void) {
  SceUID th = *g_game_thid_p;
  if (th <= 0) return 0;
  SceKernelThreadInfo ti;
  memset(&ti, 0, sizeof ti);
  ti.size = sizeof ti;
  if (sceKernelGetThreadInfo(th, &ti) < 0 || !ti.stack) return 0;
  g_stk_lo = (uintptr_t)ti.stack;
  g_stk_hi = g_stk_lo + ti.stackSize;
  log_printf("[pcs] game thread 0x%x stack %p+0x%x", (unsigned)th, ti.stack,
             (unsigned)ti.stackSize);
  return 1;
}

static int tick_thread(SceSize args, void *argp) {
  (void)args; (void)argp;
  while (!find_game_stack()) sceKernelDelayThread(100 * 1000);
  uint64_t start = sceKernelGetProcessTimeWide(), run0 = game_run_us();
  uint32_t swaps0 = g_swaps;
  unsigned window = 0;
  for (;;) {
    sceKernelDelayThread(g_tick_us);
    g_ticks++;
    if (g_armed) {
      g_missed++;                    /* no thread ran library code for a whole tick */
    } else {
      g_armed = 1;
      __sync_synchronize();
      if (protect(0) < 0) { g_armed = 0; g_missed++; }
    }
    uint64_t now = sceKernelGetProcessTimeWide();
    if (now - start < WINDOW_US) continue;
    uint64_t run = game_run_us();
    unsigned busy = (unsigned)((run - run0) * 100u / (now - start));
    unsigned n = g_game;
    uint32_t sw = g_swaps;
    double fps = (double)(sw - swaps0) * 1e6 / (double)(now - start);
    log_printf("[pcs] window %u: area %s, %.1f fps; %u samples in %u ticks (other threads %u, "
               "idle ticks %u, not tallied %u)", ++window, g_area, fps, n, (unsigned)g_ticks,
               (unsigned)g_other, (unsigned)g_missed, (unsigned)g_dropped);
    /* busy: the game thread's own CPU time over the window. Whatever is not
     * busy, it spent blocked (GL ring, vblank, locks). Of the busy part, the
     * samples say how much was libKOTOR, the port layer, and -- by what is
     * left over against the tick count -- the loader, vitaGL and libc. */
    log_printf("[pcs]   game thread busy %u%% of the window; samples in libKOTOR %u, "
               "port %u; stack depth found %.1f", busy, (unsigned)g_in_mod[0],
               (unsigned)g_in_mod[1], n ? (double)g_depth_sum / n : 0.0);
    g_game = g_other = g_ticks = g_missed = g_dropped = 0;
    g_in_mod[0] = g_in_mod[1] = 0;
    g_depth_sum = 0;
    report("pc", g_pc, TOP_PC);
    report("lr", g_lr, TOP_LR);
    report("stk", g_stk, TOP_STK);
    start = sceKernelGetProcessTimeWide();
    run0 = game_run_us();
    swaps0 = g_swaps;
  }
  return 0;
}

static void set_range(int i, const so_module *m, const char *name) {
  g_rng[i].lo = m->text_base & ~(uintptr_t)0xFFF;
  g_rng[i].hi = (m->text_base + m->text_size + 0xFFF) & ~(uintptr_t)0xFFF;
  g_rng[i].on = 1;
  /* A no-op protect first: an older kubridge without kuKernelMemProtect, or
   * one that will not change this block, says so here and not mid-game. */
  int r = kuKernelMemProtect((void *)g_rng[i].lo, g_rng[i].hi - g_rng[i].lo,
                             KU_KERNEL_PROT_READ | KU_KERNEL_PROT_EXEC);
  if (r < 0) {
    g_rng[i].on = 0;
    log_printf("[pcs] kuKernelMemProtect refused %s's text (0x%x)", name, (unsigned)r);
  }
}

void pcsamp_install(volatile SceUID *game_thid, unsigned tick_us) {
  g_game_thid_p = game_thid;
  g_tick_us = tick_us < 500 ? 500 : tick_us;
  set_range(0, &kotor_mod, "libKOTOR");
  set_range(1, &port_mod, "libandroid_port");
  if (!g_rng[0].on) {
    log_printf("[pcs] PC sampler off (needs a kubridge with memory protection)");
    return;
  }
  KuKernelExceptionHandlerOpt opt;
  opt.size = sizeof opt;
  int r = kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT,
                                           pcs_handler, &g_prev, &opt);
  if (r < 0) {
    log_printf("[pcs] exception handler not registered (0x%x): PC sampler off", (unsigned)r);
    return;
  }
  SceUID th = sceKernelCreateThread("kotor_pcs", tick_thread, 0x100000F0, 0x4000, 0,
                                    thread_mask(CPU_AUX_B), NULL);
  if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
    log_printf("[pcs] tick thread did not start (0x%x)", (unsigned)th);
    return;
  }
  thread_census_add(th, "pcsamp");
  log_printf("[pcs] armed: libKOTOR %p-%p%s, tick %u us, stack walk %u bytes, report every %u s",
             (void *)g_rng[0].lo, (void *)g_rng[0].hi,
             g_rng[1].on ? " + libandroid_port" : "", g_tick_us, STK_SCAN,
             WINDOW_US / 1000000u);
}
