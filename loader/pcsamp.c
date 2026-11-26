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
#define BUCKET_SHIFT 2           /* 4-byte buckets: one Thumb-2 instruction */
#define WINDOW_US    (15u * 1000u * 1000u)
#define TOP_PC       400
#define TOP_LR       150

typedef struct { uint32_t key, n; } Slot;
static Slot g_pc[TAB_N], g_lr[TAB_N];

static volatile SceUID *g_game_thid_p;
static uintptr_t g_lo, g_hi;             /* the protected range: libKOTOR's text */
static uintptr_t g_stk_lo, g_stk_hi;     /* the game thread's stack */
static unsigned g_tick_us;

static volatile uint32_t g_armed;        /* text is (being) made non-executable */
static volatile uint32_t g_game, g_other, g_dropped, g_spurious;
static volatile uint32_t g_ticks, g_missed;
static KuKernelExceptionHandler g_prev;

static int protect(int exec) {
  return kuKernelMemProtect((void *)g_lo, g_hi - g_lo,
                            KU_KERNEL_PROT_READ | (exec ? KU_KERNEL_PROT_EXEC : 0));
}

static void tally(Slot *t, uint32_t key) {
  uint32_t i = (key * 2654435761u) >> 19;
  for (unsigned probe = 0; probe < 64; probe++, i = (i + 1) & (TAB_N - 1)) {
    if (t[i].key == key) { t[i].n++; return; }
    if (!t[i].key) { t[i].key = key; t[i].n = 1; return; }
  }
  g_dropped++;
}

/* Every prefetch abort lands here first. One inside libKOTOR's text while the
 * sampler has the text locked is a sample: give execute back and return, and
 * the instruction runs again. Any thread can take it; only the game thread's
 * are tallied. Two threads can fault on the same lock, so a fault in the text
 * after the lock has been lifted is let through as well -- but only a few in
 * a row, so a real jump into garbage still reaches the crash handler. */
static void pcs_handler(KuKernelExceptionContext *c) {
  uint32_t pc = c->pc;
  if (pc - g_lo < g_hi - g_lo) {
    if (g_armed) {
      protect(1);
      g_armed = 0;
      g_spurious = 0;
      if (c->sp - g_stk_lo < g_stk_hi - g_stk_lo) {
        tally(g_pc, (pc - g_lo) >> BUCKET_SHIFT);
        tally(g_lr, c->lr);
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
static void report(const char *what, Slot *t, int top, int is_pc) {
  static Slot copy[TAB_N];
  memcpy(copy, t, sizeof copy);
  memset(t, 0, sizeof copy);
  qsort(copy, TAB_N, sizeof *copy, by_n);
  char line[400];
  int len = 0;
  for (int i = 0; i < top && copy[i].n >= 2; i++) {
    char nm[24];
    if (is_pc) snprintf(nm, sizeof nm, "k+%x", (unsigned)(copy[i].key << BUCKET_SHIFT));
    else name_addr(copy[i].key, nm, sizeof nm);
    len += snprintf(line + len, sizeof line - (size_t)len, " %s:%u", nm, (unsigned)copy[i].n);
    if (len > 300 || i == top - 1 || copy[i + 1].n < 2) {
      log_printf("[pcs]   %s%s", what, line);
      len = 0;
    }
  }
}

static int find_game_stack(void) {
  SceUID th = *g_game_thid_p;
  if (!th) return 0;
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
  uint64_t start = sceKernelGetProcessTimeWide();
  unsigned window = 0;
  for (;;) {
    sceKernelDelayThread(g_tick_us);
    g_ticks++;
    if (g_armed) {
      g_missed++;                    /* no thread ran libKOTOR for a whole tick */
    } else {
      g_armed = 1;
      __sync_synchronize();
      if (protect(0) < 0) { g_armed = 0; g_missed++; }
    }
    uint64_t now = sceKernelGetProcessTimeWide();
    if (now - start < WINDOW_US) continue;
    log_printf("[pcs] window %u: %u samples in %u ticks (other threads %u, idle ticks %u, "
               "not tallied %u)", ++window, (unsigned)g_game, (unsigned)g_ticks,
               (unsigned)g_other, (unsigned)g_missed, (unsigned)g_dropped);
    g_game = g_other = g_ticks = g_missed = g_dropped = 0;
    report("pc", g_pc, TOP_PC, 1);
    report("lr", g_lr, TOP_LR, 0);
    start = sceKernelGetProcessTimeWide();
  }
  return 0;
}

void pcsamp_install(volatile SceUID *game_thid, unsigned tick_us) {
  g_game_thid_p = game_thid;
  g_tick_us = tick_us < 500 ? 500 : tick_us;
  g_lo = kotor_mod.text_base & ~(uintptr_t)0xFFF;
  g_hi = (kotor_mod.text_base + kotor_mod.text_size + 0xFFF) & ~(uintptr_t)0xFFF;

  /* A no-op protect first: an older kubridge without kuKernelMemProtect, or
   * one that will not change this block, says so here and not mid-game. */
  int r = protect(1);
  if (r < 0) {
    log_printf("[pcs] kuKernelMemProtect refused libKOTOR's text (0x%x): PC sampler off "
               "(needs a kubridge with memory protection)", (unsigned)r);
    return;
  }
  KuKernelExceptionHandlerOpt opt;
  opt.size = sizeof opt;
  r = kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT, pcs_handler,
                                       &g_prev, &opt);
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
  log_printf("[pcs] armed: libKOTOR text %p-%p, tick %u us, report every %u s",
             (void *)g_lo, (void *)g_hi, g_tick_us, WINDOW_US / 1000000u);
}
