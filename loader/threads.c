/* threads.c -- core pinning and the per-thread load census. See threads.h. */

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <string.h>

#include "config.h"
#include "threads.h"
#include "log.h"

#define CENSUS_MAX 48

typedef struct {
  volatile int used;                     /* set last, with release */
  SceUID thid;
  char tag[20];
  uint64_t last_run;
} census_t;

static census_t g_census[CENSUS_MAX];
static volatile int g_census_n = 0;
static volatile unsigned g_worker_rr = 0;
static uint64_t g_census_t0 = 0;

int thread_mask(int mask) {
#if THREAD_PINNING
  return mask;
#else
  (void)mask;
  return 0;
#endif
}

int thread_next_worker_mask(void) {
  unsigned n = __atomic_fetch_add(&g_worker_rr, 1, __ATOMIC_RELAXED);
  return (n & 1) ? CPU_AUX_A : CPU_AUX_B;
}

void thread_census_add(SceUID thid, const char *tag) {
  if (thid < 0)
    return;
  int i = __atomic_fetch_add(&g_census_n, 1, __ATOMIC_RELAXED);
  if (i >= CENSUS_MAX)
    return;
  census_t *c = &g_census[i];
  c->thid = thid;
  strncpy(c->tag, tag ? tag : "?", sizeof c->tag - 1);
  c->tag[sizeof c->tag - 1] = 0;
  c->last_run = 0;
  __atomic_store_n(&c->used, 1, __ATOMIC_RELEASE);
}

void thread_pin_self(int mask, const char *tag) {
  int r = 0;
#if THREAD_PINNING
  r = sceKernelChangeThreadCpuAffinityMask(0, mask);
#endif
  thread_census_add(sceKernelGetThreadId(), tag);
  log_printf("[cpu] %s thid=0x%08x -> core mask 0x%05x%s", tag ? tag : "?",
             (unsigned)sceKernelGetThreadId(), (unsigned)thread_mask(mask),
             r < 0 ? " (FAILED)" : "");
}

static int core_of(const SceKernelThreadInfo *ti) {
  switch (ti->currentCpuAffinityMask & SCE_KERNEL_CPU_MASK_USER_ALL) {
    case SCE_KERNEL_CPU_MASK_USER_0: return 0;
    case SCE_KERNEL_CPU_MASK_USER_1: return 1;
    case SCE_KERNEL_CPU_MASK_USER_2: return 2;
  }
  int c = ti->lastExecutedCpuId;      /* not pinned: wherever it last ran */
  return (c >= 0 && c < 3) ? c : -1;
}

void thread_census_log(void) {
  uint64_t now = sceKernelGetProcessTimeWide();
  uint64_t dt = now - g_census_t0;
  int first = (g_census_t0 == 0);
  g_census_t0 = now;
  unsigned core_pm[3] = { 0, 0, 0 };   /* per mille of one core */

  int n = g_census_n < CENSUS_MAX ? g_census_n : CENSUS_MAX;
  for (int i = 0; i < n; i++) {
    census_t *c = &g_census[i];
    if (!__atomic_load_n(&c->used, __ATOMIC_ACQUIRE))
      continue;
    SceKernelThreadInfo ti;
    memset(&ti, 0, sizeof ti);
    ti.size = sizeof ti;
    if (sceKernelGetThreadInfo(c->thid, &ti) < 0)
      continue;                        /* exited */
    uint64_t run = (uint64_t)ti.runClocks;
    uint64_t d = run - c->last_run;
    c->last_run = run;
    if (first || dt == 0)
      continue;                        /* no window yet: this call is the baseline */
    unsigned pm = (unsigned)(d * 1000u / dt);
    int core = core_of(&ti);
    if (core >= 0)
      core_pm[core] += pm;
    if (pm == 0)
      continue;                        /* idle threads would only bury the busy ones */
    log_printf("[cpu] %-16s %-20.20s core=%d mask=0x%05x prio=%d load=%u.%u%%",
               c->tag, ti.name, core, (unsigned)ti.currentCpuAffinityMask,
               (int)ti.currentPriority, pm / 10, pm % 10);
  }
  if (!first && dt)
    log_printf("[cpu] cores over %u ms: c0=%u.%u%% c1=%u.%u%% c2=%u.%u%% "
               "(recorded threads only; vitaGL GC and SDL internals not listed)",
               (unsigned)(dt / 1000),
               core_pm[0] / 10, core_pm[0] % 10, core_pm[1] / 10, core_pm[1] % 10,
               core_pm[2] / 10, core_pm[2] % 10);
}
