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
#define TOP_SITES    500
#define TOP_CALLEES  150

typedef struct { uint32_t key, n; } Slot;
static Slot g_site[TAB_N], g_callee[TAB_N];
static volatile uint32_t g_flag;
static volatile uint32_t g_samples, g_dropped;
/* Samples by where the call site and the callee live: libKOTOR, the port
 * layer, or neither (the loader: GL queue, libc, libm). */
static volatile uint32_t g_cls[3][3];
static int cls_of(uint32_t a) {
  if (a - kotor_mod.text_base < kotor_mod.text_size) return 0;
  if (a - port_mod.text_base < port_mod.text_size) return 1;
  return 2;
}

static void name_addr(uint32_t a, char *out, size_t n);

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
/* Stall finder. A flag raised by the tick thread and not taken for many ticks
 * means the game thread ran that long without one PLT call: a loop with no
 * calls in it, or one long call into code that is not instrumented (the
 * loader, libc). Its one eventual sample says nothing about how long it took,
 * so time it here: the call that finally takes the flag is the first one after
 * the stretch, and the previous sample is the last thing seen before it. */
#define STALL_LOG_MS 100
static volatile uint32_t g_ticks_total, g_raised_at;
static uint32_t g_prev_lr, g_prev_target;
static unsigned g_stall_logs;

__attribute__((used)) void samp_record(uint32_t lr, uint32_t target) {
  if (sceKernelGetThreadId() != *g_game_thid_p) return;
  g_flag = 0;
  uint32_t stall = g_ticks_total - g_raised_at;
  if (stall >= STALL_LOG_MS && g_stall_logs < 400) {
    char a[24], b[24], c[24], d[24];
    g_stall_logs++;
    name_addr(lr, a, sizeof a);
    name_addr(target, b, sizeof b);
    name_addr(g_prev_lr, c, sizeof c);
    name_addr(g_prev_target, d, sizeof d);
    log_printf("[samp] STALL %u ms with no call: ended at site %s -> %s; last sample before was %s -> %s",
               (unsigned)stall, a, b, c, d);
  }
  g_prev_lr = lr;
  g_prev_target = target;
  tally(g_site, lr);
  tally(g_callee, target);
  g_cls[cls_of(lr)][cls_of(target)]++;
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
  for (int i = 0; i < top && copy[i].n >= 3; i++) {
    char nm[24];
    name_addr(copy[i].key, nm, sizeof nm);
    len += snprintf(line + len, sizeof line - (size_t)len, " %s:%u", nm, (unsigned)copy[i].n);
    if (len > 300 || i == top - 1 || copy[i + 1].n < 3) {
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
    g_ticks_total++;
    if (!g_flag) { g_raised_at = g_ticks_total; g_flag = 1; }
    if (++ticks < WINDOW_TICKS) continue;
    ticks = 0;
    uint32_t n = g_samples, d = g_dropped;
    g_samples = 0; g_dropped = 0;
    log_printf("[samp] window %u: %u samples in %u ticks (%u not tallied: table full)",
               ++window, (unsigned)n, WINDOW_TICKS, (unsigned)d);
    log_printf("[samp]   from libKOTOR -> libKOTOR %u, port %u, loader %u | from port -> libKOTOR %u, "
               "port %u, loader %u | from loader -> %u/%u/%u",
               (unsigned)g_cls[0][0], (unsigned)g_cls[0][1], (unsigned)g_cls[0][2],
               (unsigned)g_cls[1][0], (unsigned)g_cls[1][1], (unsigned)g_cls[1][2],
               (unsigned)g_cls[2][0], (unsigned)g_cls[2][1], (unsigned)g_cls[2][2]);
    memset((void *)g_cls, 0, sizeof g_cls);
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

/* ---- Profile=2: a census of the calls libKOTOR makes into the port layer ----
 * Every such slot gets a counting stub instead; a report every 15 s names each
 * function and how often it was called. This is the list of what would have
 * to cross to the render thread, and how hot each crossing is. */
#define CENSUS_MAX 400
static uint32_t g_census_n[CENSUS_MAX];
static const char *g_census_name[CENSUS_MAX];
static unsigned g_ncensus;

static int census_thread(SceSize args, void *argp) {
  (void)args; (void)argp;
  static uint32_t snap[CENSUS_MAX];
  static uint16_t order[CENSUS_MAX];
  unsigned window = 0;
  for (;;) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    sceKernelDelayThread(15 * 1000 * 1000);
    for (unsigned i = 0; i < g_ncensus; i++) { snap[i] = g_census_n[i]; g_census_n[i] = 0; order[i] = (uint16_t)i; }
    for (unsigned i = 1; i < g_ncensus; i++) {            /* insertion sort, by count */
      uint16_t k = order[i];
      unsigned j = i;
      for (; j > 0 && snap[order[j - 1]] < snap[k]; j--) order[j] = order[j - 1];
      order[j] = k;
    }
    log_printf("[census] window %u: %u ms", ++window,
               (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));
    char line[400];
    int len = 0;
    for (unsigned i = 0; i < g_ncensus && snap[order[i]]; i++) {
      const char *nm = g_census_name[order[i]];
      if (!strncmp(nm, "android_port_", 13)) nm += 13;
      len += snprintf(line + len, sizeof line - (size_t)len, " %.40s:%u", nm, (unsigned)snap[order[i]]);
      if (len > 300 || i == g_ncensus - 1 || !snap[order[i + 1]]) {
        log_printf("[census]  %s", line);
        len = 0;
      }
    }
  }
  return 0;
}

static void census_install(void) {
  so_module *m = &kotor_mod;
  static uint16_t slot_of[CENSUS_MAX];
  for (int i = 0; i < m->num_relplt && g_ncensus < CENSUS_MAX; i++) {
    uintptr_t target = *(uintptr_t *)(m->text_base + m->relplt[i].r_offset);
    if (target - port_mod.text_base >= port_mod.text_size) continue;
    slot_of[g_ncensus] = (uint16_t)i;
    g_census_name[g_ncensus++] = m->dynstr + m->dynsym[ELF32_R_SYM(m->relplt[i].r_info)].st_name;
  }
  size_t bytes = ((size_t)g_ncensus * STUB_WORDS * 4 + 0xFFF) & ~(size_t)0xFFF;
  uint32_t *code = (uint32_t *)calloc(1, bytes);
  SceKernelAllocMemBlockKernelOpt opt;
  memset(&opt, 0, sizeof opt);
  opt.size = sizeof opt;
  SceUID blk = kuKernelAllocMemBlock("census_stubs", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, bytes, &opt);
  void *base = NULL;
  if (!code || blk < 0 || sceKernelGetMemBlockBase(blk, &base) < 0 || !base) {
    log_printf("[census] could not take %u KB for stubs -- off", (unsigned)(bytes >> 10));
    free(code);
    return;
  }
  for (unsigned k = 0; k < g_ncensus; k++) {
    uint32_t *w = code + k * STUB_WORDS;
    w[0] = 0xE52D0004;                 /* push {r0} */
    w[1] = 0xE59FC010;                 /* ldr  ip, [pc, #16]   -> &count */
    w[2] = 0xE59C0000;                 /* ldr  r0, [ip] */
    w[3] = 0xE2800001;                 /* add  r0, r0, #1 */
    w[4] = 0xE58C0000;                 /* str  r0, [ip] */
    w[5] = 0xE49D0004;                 /* pop  {r0} */
    w[6] = 0xE59FF000;                 /* ldr  pc, [pc, #0]    -> target */
    w[7] = (uint32_t)(uintptr_t)&g_census_n[k];
    w[8] = (uint32_t)*(uintptr_t *)(m->text_base + m->relplt[slot_of[k]].r_offset);
  }
  kuKernelCpuUnrestrictedMemcpy(base, code, bytes);
  kuKernelFlushCaches(base, bytes);
  free(code);
  for (unsigned k = 0; k < g_ncensus; k++) {
    uintptr_t stub = (uintptr_t)base + (size_t)k * STUB_WORDS * 4;
    kuKernelCpuUnrestrictedMemcpy((void *)(m->text_base + m->relplt[slot_of[k]].r_offset),
                                  &stub, sizeof stub);
  }
  SceUID th = sceKernelCreateThread("kotor_census", census_thread, 0x10000110, 0x4000, 0,
                                    thread_mask(CPU_AUX_B), NULL);
  if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0)
    log_printf("[census] report thread did not start (0x%x)", (unsigned)th);
  log_printf("[census] armed: %u libKOTOR call slots that land in the port layer", g_ncensus);
}

void sampler_install(volatile SceUID *game_thid, int mode) {
  g_game_thid_p = game_thid;
  if (mode == 2) { census_install(); return; }
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

  SceUID th = sceKernelCreateThread("kotor_samp", tick_thread, 0x100000F0, 0x4000, 0,
                                    thread_mask(CPU_AUX_B), NULL);
  if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0)
    log_printf("[samp] tick thread did not start (0x%x)", (unsigned)th);
  else
    thread_census_add(th, "sampler");
  log_printf("[samp] armed: %u libKOTOR + %u port call slots, 1 ms tick, report every %u s",
             nk, np, WINDOW_TICKS / 1000);
}
