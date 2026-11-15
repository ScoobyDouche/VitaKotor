/* sampler.c -- see sampler.h */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vitasdk.h>
#include <kubridge.h>

#include "config.h"
#include "log.h"
#include "sampler.h"
#include "so_util.h"
#include "threads.h"

extern so_module kotor_mod, port_mod;
static volatile SceUID *g_game_thid_p;

#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX 0x0C20D050   /* as so_util.c */

#define TAB_N        4096          /* power of two */
#define STUB_WORDS   9
#define TICK_US      1000
#define WINDOW_TICKS 15000         /* a report every ~15 s */
#define TOP_SITES    60
#define TOP_CALLEES  30

typedef struct { uint32_t key, n; } Slot;
static Slot g_site[TAB_N], g_callee[TAB_N];
static volatile uint32_t g_flag;
static volatile uint32_t g_samples, g_dropped;

static void tally(Slot *t, uint32_t key) {
  uint32_t i = (key * 2654435761u) >> 20;
  for (unsigned probe = 0; probe < 64; probe++, i = (i + 1) & (TAB_N - 1)) {
    if (t[i].key == key) { t[i].n++; return; }
    if (!t[i].key) { t[i].key = key; t[i].n = 1; return; }
  }
  g_dropped++;
}

/* Reached from samp_slow with the flag raised: r0 = the caller's return
 * address, r1 = the function it is calling. Any thread can land here; only
 * the game thread's call takes the sample. */
__attribute__((used)) void samp_record(uint32_t lr, uint32_t target) {
  if (sceKernelGetThreadId() != *g_game_thid_p) return;
  g_flag = 0;
  tally(g_site, lr);
  tally(g_callee, target);
  g_samples++;
}

/* The slow half of every stub: ip holds the real target. Six words pushed,
 * so sp stays 8-aligned for the C call. */
__attribute__((naked, target("arm"), used)) static void samp_slow(void) {
  __asm__ volatile(
    "push {r0-r3, ip, lr}\n"
    "mov  r0, lr\n"
    "mov  r1, ip\n"
    "bl   samp_record\n"
    "pop  {r0-r3, ip, lr}\n"
    "bx   ip\n");
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

/* Snapshot, clear and print one table. The game thread may be mid-tally; a
 * count that is off by one in a 15000-sample window does not matter. */
static void report(const char *what, Slot *t, int top) {
  static Slot copy[TAB_N];
  memcpy(copy, t, sizeof copy);
  memset(t, 0, sizeof copy);
  qsort(copy, TAB_N, sizeof *copy, by_n);
  char line[400];
  int len = 0;
  for (int i = 0; i < top && copy[i].n; i++) {
    char nm[24];
    name_addr(copy[i].key, nm, sizeof nm);
    len += snprintf(line + len, sizeof line - (size_t)len, " %s:%u", nm, (unsigned)copy[i].n);
    if (len > 300 || i == top - 1 || !copy[i + 1].n) {
      log_printf("[samp]   %s%s", what, line);
      len = 0;
    }
  }
}

static int tick_thread(SceSize args, void *argp) {
  (void)args; (void)argp;
  unsigned ticks = 0, window = 0;
  for (;;) {
    sceKernelDelayThread(TICK_US);
    g_flag = 1;
    if (++ticks < WINDOW_TICKS) continue;
    ticks = 0;
    uint32_t n = g_samples, d = g_dropped;
    g_samples = 0; g_dropped = 0;
    log_printf("[samp] window %u: %u samples in %u ticks (%u not tallied: table full)",
               ++window, (unsigned)n, WINDOW_TICKS, (unsigned)d);
    report("site", g_site, TOP_SITES);
    report("callee", g_callee, TOP_CALLEES);
  }
  return 0;
}

static unsigned wrap_module(so_module *m, uint32_t *code, unsigned at) {
  unsigned n = 0;
  for (int i = 0; i < m->num_relplt; i++) {
    uintptr_t *slot = (uintptr_t *)(m->text_base + m->relplt[i].r_offset);
    uintptr_t target = *slot;
    if (!target) continue;
    uint32_t *w = code + (at + n) * STUB_WORDS;
    w[0] = 0xE59FC010;                 /* ldr  ip, [pc, #16]   -> &g_flag */
    w[1] = 0xE59CC000;                 /* ldr  ip, [ip] */
    w[2] = 0xE35C0000;                 /* cmp  ip, #0 */
    w[3] = 0xE59FC008;                 /* ldr  ip, [pc, #8]    -> target */
    w[4] = 0x012FFF1C;                 /* bxeq ip */
    w[5] = 0xE59FF004;                 /* ldr  pc, [pc, #4]    -> samp_slow */
    w[6] = (uint32_t)(uintptr_t)&g_flag;
    w[7] = (uint32_t)target;
    w[8] = (uint32_t)(uintptr_t)&samp_slow & ~1u;
    n++;
  }
  return n;
}

void sampler_install(volatile SceUID *game_thid) {
  g_game_thid_p = game_thid;
  unsigned total = (unsigned)(kotor_mod.num_relplt + port_mod.num_relplt);
  size_t bytes = ((size_t)total * STUB_WORDS * 4 + 0xFFF) & ~(size_t)0xFFF;
  uint32_t *code = (uint32_t *)calloc(1, bytes);
  SceKernelAllocMemBlockKernelOpt opt;
  memset(&opt, 0, sizeof opt);
  opt.size = sizeof opt;
  SceUID blk = kuKernelAllocMemBlock("samp_stubs", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, bytes, &opt);
  void *base = NULL;
  if (!code || blk < 0 || sceKernelGetMemBlockBase(blk, &base) < 0 || !base) {
    log_printf("[samp] could not take %u KB for stubs -- profile off", (unsigned)(bytes >> 10));
    free(code);
    return;
  }
  unsigned nk = wrap_module(&kotor_mod, code, 0);
  unsigned np = wrap_module(&port_mod, code, nk);
  kuKernelCpuUnrestrictedMemcpy(base, code, bytes);
  kuKernelFlushCaches(base, bytes);
  free(code);

  /* Only now, with the stubs executable, point the slots at them. */
  so_module *mods[2] = { &kotor_mod, &port_mod };
  unsigned at = 0;
  for (int k = 0; k < 2; k++) {
    so_module *m = mods[k];
    for (int i = 0; i < m->num_relplt; i++) {
      uintptr_t *slot = (uintptr_t *)(m->text_base + m->relplt[i].r_offset);
      if (!*slot) continue;
      uintptr_t stub = (uintptr_t)base + (size_t)at++ * STUB_WORDS * 4;
      kuKernelCpuUnrestrictedMemcpy(slot, &stub, sizeof stub);
    }
  }

  SceUID th = sceKernelCreateThread("kotor_samp", tick_thread, 0x100000A0, 0x4000, 0,
                                    thread_mask(CPU_AUX_B), NULL);
  if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0)
    log_printf("[samp] tick thread did not start (0x%x)", (unsigned)th);
  else
    thread_census_add(th, "sampler");
  log_printf("[samp] armed: %u libKOTOR + %u port call slots, 1 ms tick, report every %u s",
             nk, np, WINDOW_TICKS / 1000);
}
