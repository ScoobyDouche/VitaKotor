/* srv_overlap.c -- see srv_overlap.h */

#include <vitasdk.h>
#include <kubridge.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "threads.h"
#include "srv_overlap.h"

#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)   /* as so_util.c */
#endif

volatile int g_ov_active;
int g_srv_overlap_enabled = SRV_OVERLAP;

#if SRV_OVERLAP_BUILD

typedef void (*ai_fn_t)(void *);
typedef uint32_t (*fn1_t)(void *);

static ai_fn_t g_ai_orig;            /* CServerAIMaster::UpdateState */
static fn1_t g_render_orig;          /* Camera::RenderScene */
static fn1_t g_srvloop_orig;         /* CServerExoApp::MainLoop */

/* The owed tick. Only the game thread touches these outside an overlap. */
static void *g_owed_this;
static int g_owed;
static int g_srvloop_depth;
static int g_render_depth;

static volatile int g_render_done;
static SceUID g_sema_kick, g_sema_done, g_sema_render;
static SceUID g_worker_thid;
static uintptr_t g_worker_lo, g_worker_hi;
static void *volatile g_work_this;
static volatile uint64_t g_work_us;      /* worker's last tick, us */
static volatile uint64_t g_work_wait_us; /* of which parked at a gate */

/* ---- stubs -------------------------------------------------------------- */

typedef struct {
  const char *name;
  uintptr_t orig;
  uint32_t waits;        /* times it parked the worker mid-render (this window) */
  uint32_t waits_total;
  uint8_t flush;         /* lifecycle point: run an owed tick first */
} stub_t;

static stub_t *g_stub;
static unsigned g_nstub;

#define STUB_BYTES 80
static void gate_check(unsigned idx, uintptr_t lr);

/* ARM stub, 72 bytes:
 *   ldr ip,[lo]; cmp sp,ip; bls fast; ldr ip,[hi]; cmp sp,ip; bhs fast
 *   push {r0-r3,ip,lr}; mov r1,lr; ldr r0,[idx]; ldr ip,[check]; blx ip
 *   pop {r0-r3,ip,lr}
 * fast:
 *   ldr pc,[orig]
 *   .word lo, hi, orig, idx, check
 * Only a thread whose sp is inside [lo,hi) -- the AI worker, or every thread
 * for a lifecycle point (lo=0, hi=~0) -- takes the call; everyone else pays
 * six instructions. Stack arguments and VFP registers are untouched (the
 * stub never moves sp across the tail jump), so any signature passes through. */
static void emit_stub(uint32_t *w, uintptr_t lo, uintptr_t hi, uintptr_t orig, unsigned idx) {
  w[0]  = 0xe59fc02c;  /* ldr ip, [pc, #0x2c]  -> lo    */
  w[1]  = 0xe15d000c;  /* cmp sp, ip                    */
  w[2]  = 0x9a000008;  /* bls fast                      */
  w[3]  = 0xe59fc024;  /* ldr ip, [pc, #0x24]  -> hi    */
  w[4]  = 0xe15d000c;  /* cmp sp, ip                    */
  w[5]  = 0x2a000005;  /* bhs fast                      */
  w[6]  = 0xe92d500f;  /* push {r0-r3, ip, lr}          */
  w[7]  = 0xe1a0100e;  /* mov r1, lr                    */
  w[8]  = 0xe59f0018;  /* ldr r0, [pc, #0x18]  -> idx   */
  w[9]  = 0xe59fc018;  /* ldr ip, [pc, #0x18]  -> check */
  w[10] = 0xe12fff3c;  /* blx ip                        */
  w[11] = 0xe8bd500f;  /* pop {r0-r3, ip, lr}           */
  w[12] = 0xe59ff004;  /* fast: ldr pc, [pc, #4] -> orig */
  w[13] = (uint32_t)lo;
  w[14] = (uint32_t)hi;
  w[15] = (uint32_t)orig;
  w[16] = idx;
  w[17] = (uint32_t)(uintptr_t)&gate_check;
}

/* ---- which engine functions are render-side ------------------------------ */

static int has_prefix(const char *s, size_t n, const char *p) {
  size_t k = strlen(p);
  return n >= k && !memcmp(s, p, k);
}

static int render_class(const char *c, size_t n) {
  static const char *const exact[] = {
    "Gob", "Scene", "Camera", "GLRender", "Room", "Emitter",
    "CSWCModule", "CExoResMan", "CRes",
  };
  static const char *const prefix[] = {
    "Part", "Mdl", "Model", "Material", "Light", "Vertex", "CAur", "Texture",
    "Particle", "Lightning", "Shadow", "Grass", "Sky", "Wind", "CSWCamera",
    "CSWBehavior",
  };
  for (unsigned i = 0; i < sizeof exact / sizeof *exact; i++)
    if (strlen(exact[i]) == n && !memcmp(c, exact[i], n)) return 1;
  for (unsigned i = 0; i < sizeof prefix / sizeof *prefix; i++)
    if (has_prefix(c, n, prefix[i])) return 1;
  if (n > 3 && !memcmp(c + n - 3, "Gob", 3)) return 1;     /* StringGob, ... */
  return 0;
}

static int is_upper_or_digit(char ch) {
  return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
}

/* Render-family functions the worker may run during the render pass anyway.
 * log-2026-10-31-overlap: these parked the AI in 94% of ticks (SetPosition
 * 5481, GetResObject 2900 of ~10k), costing most of the overlap.
 *   Gob::SetPosition / SetOrientation store 3 / 4 floats and return
 *     (0x44a020); a render that reads one mid-store draws that object a few
 *     cm off for one frame.
 *   CExoResMan lookups walk the key tables, which only change at module load
 *     (no indirect calls); the script VM looks up its .ncs this way. */
static const char *const g_allow[] = {
  "_ZN3Gob11SetPositionE6Vector",
  "_ZN3Gob14SetOrientationE10Quaternion",
  "_ZN10CExoResMan12GetResObjectERK7CResReft",
  "_ZN10CExoResMan11GetKeyEntryERK7CResReftPP12CExoKeyTablePP14CKeyTableEntry",
  "_ZN10CExoResMan6ExistsERK7CResReftm",
  "_ZN10CExoResMan6ExistsERK7CResReftPm",
  "_ZN10CExoResMan6ExistsERK7CResReftR10CExoString",
  "_ZN10CExoResMan8GetResIDERK7CResReft",
};

/* Mangled name -> 1 if the worker must not run it during the render pass. */
static int gated_name(const char *m) {
  for (unsigned i = 0; i < sizeof g_allow / sizeof *g_allow; i++)
    if (!strcmp(m, g_allow[i])) return 0;
  if (!strncmp(m, "_ZNK", 4)) return 0;                    /* const: a read */
  if (!strncmp(m, "_ZN", 3)) {
    const char *s = m + 3, *id[8];
    size_t len[8];
    int k = 0;
    while (*s >= '0' && *s <= '9' && k < 8) {
      size_t n = (size_t)strtoul(s, (char **)&s, 10);
      if (strnlen(s, n) < n) return 0;
      id[k] = s; len[k] = n; k++;
      s += n;
    }
    if (k >= 1 && (*s == 'C' || *s == 'D') && s[1] >= '0' && s[1] <= '2') {
      id[k] = s; len[k] = 2; k++;                           /* ctor / dtor */
    }
    if (k < 2) return 0;
    const char *c = id[k - 2], *f = id[k - 1];
    size_t cn = len[k - 2], fn = len[k - 1];
    if (!render_class(c, cn)) return 0;
    int resman = (cn == 10 && !memcmp(c, "CExoResMan", 10)) || (cn == 4 && !memcmp(c, "CRes", 4));
    if (!resman && ((fn > 3 && !memcmp(f, "Get", 3) && is_upper_or_digit(f[3])) ||
                    (fn > 2 && !memcmp(f, "Is", 2) && is_upper_or_digit(f[2])) ||
                    (fn > 3 && !memcmp(f, "Has", 3) && is_upper_or_digit(f[3]))))
      return 0;                                             /* plain reads */
    return 1;
  }
  const char *f = m;
  if (!strncmp(m, "_Z", 2) && m[2] >= '0' && m[2] <= '9') {
    f = m + 2;
    while (*f >= '0' && *f <= '9') f++;
  } else if (!strncmp(m, "_Z", 2)) {
    return 0;
  }
  return !strncmp(f, "Aur", 3) || !strncmp(f, "NewCAur", 7) || !strncmp(f, "DeleteCAur", 10) ||
         !strncmp(f, "android_port_", 13) || !strncmp(f, "SDL_GL_", 7) ||
         (f[0] == 'g' && f[1] == 'l' && f[2] >= 'A' && f[2] <= 'Z');
}

static const char *const g_flush_syms[] = {
  "_ZN11CAppManager13DestroyServerEv",
  "_ZN13CServerExoApp10LoadModuleERK10CExoStringP10CSWSPlayer",
  "_ZN13CServerExoApp12UnloadModuleEv",
  "_ZN13CServerExoApp14StartNewModuleERK10CExoString",
  "_ZN13CServerExoApp17ExecuteLoadModuleERK10CExoString",
  "_ZN13CServerExoApp18ShutDownToMainMenuEv",
  "_ZN13CServerExoApp8LoadGameEmRK10CExoStringS2_P10CSWSPlayer",
  "_ZN13CServerExoApp8SaveGameEmRK10CExoStringP10CSWSPlayeri",
  "_ZN13CServerExoApp8ShutdownEii",
  "_ZN21CClientExoAppInternal18ShutDownToMainMenuEv",
  "_ZN21CServerExoAppInternal10LoadModuleERK10CExoStringP10CSWSPlayer",
  "_ZN21CServerExoAppInternal12UnloadModuleEv",
  "_ZN21CServerExoAppInternal14StartNewModuleERK10CExoString",
  "_ZN21CServerExoAppInternal17ExecuteLoadModuleERK10CExoString",
  "_ZN21CServerExoAppInternal18StallEventSaveGameEv",
  "_ZN21CServerExoAppInternal8LoadGameEmRK10CExoStringS2_P10CSWSPlayer",
  "_ZN21CServerExoAppInternal8SaveGameEmRK10CExoStringP10CSWSPlayeri",
  "_ZN21CServerExoAppInternal8ShutdownEii",
};
#define NFLUSH (sizeof g_flush_syms / sizeof *g_flush_syms)

/* ---- stats --------------------------------------------------------------- */

static struct {
  unsigned kicks, sync_runs, nested_skips;
  uint64_t ai_us, wait_us, render_us, join_us;
  unsigned gated_ticks, gl_waits;
} g_st;
static unsigned g_logged_first;

/* ---- the overlap --------------------------------------------------------- */

static void park_until_render_done(void) {
  uint64_t t0 = sceKernelGetProcessTimeWide();
  while (!g_render_done) sceKernelWaitSema(g_sema_render, 1, NULL);
  g_work_wait_us += sceKernelGetProcessTimeWide() - t0;
}

static void run_owed_sync(void) {
  if (!g_owed) return;
  g_owed = 0;
  g_st.sync_runs++;
  g_ai_orig(g_owed_this);
}

static void gate_check(unsigned idx, uintptr_t lr) {
  stub_t *s = &g_stub[idx];
  if (s->flush) {
    /* lifecycle point: the game thread runs an owed tick before going on */
    if (!g_ov_active && sceKernelGetThreadId() != g_worker_thid) run_owed_sync();
    return;
  }
  if (!g_ov_active || g_render_done || sceKernelGetThreadId() != g_worker_thid) return;
  s->waits++;
  s->waits_total++;
  if (g_logged_first < 40 && s->waits_total == 1) {
    g_logged_first++;
    log_printf("[ov] gate: AI reached %s mid-render (from kotor+0x%x); parked till render ends",
               s->name, (unsigned)(lr - kotor_mod.text_base));
  }
  park_until_render_done();
}

void srv_overlap_gl_guard(void) {
  if (!g_ov_active || g_render_done || sceKernelGetThreadId() != g_worker_thid) return;
  g_st.gl_waits++;
  if (g_logged_first < 40) {
    g_logged_first++;
    log_printf("[ov] gate: AI issued GL mid-render (from %p); parked till render ends",
               __builtin_return_address(0));
  }
  park_until_render_done();
}

static void *worker_thread(void *arg) {
  (void)arg;
  volatile uint32_t here = 0;
  uintptr_t sp = (uintptr_t)&here;
  g_worker_thid = sceKernelGetThreadId();
  /* everything below our first frame, less a margin, is this stack */
  g_worker_hi = (sp + 0x1000) & ~(uintptr_t)0xfff;
  g_worker_lo = sp - SRV_OVERLAP_STACK + 0x10000;
  thread_pin_self(CPU_AUX_A, "srv-ai");
  thread_census_add(g_worker_thid, "srv-ai");
  sceKernelSignalSema(g_sema_done, 1);           /* bounds published */
  for (;;) {
    sceKernelWaitSema(g_sema_kick, 1, NULL);
    g_work_wait_us = 0;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    g_ai_orig(g_work_this);
    g_work_us = sceKernelGetProcessTimeWide() - t0;
    __sync_synchronize();
    sceKernelSignalSema(g_sema_done, 1);
  }
  return NULL;
}

/* CServerAIMaster::UpdateState, called from CServerExoAppInternal::MainLoop */
static void ai_wrap(void *self) {
  if (!g_srv_overlap_enabled || g_srvloop_depth != 1 || g_ov_active) {
    g_ai_orig(self);
    return;
  }
  run_owed_sync();               /* never two ticks owed */
  g_owed_this = self;
  g_owed = 1;
}

/* CServerExoApp::MainLoop: GameUpdate, UpdateScreen, GameEnteredBackground */
static uint32_t srvloop_wrap(void *self) {
  run_owed_sync();
  g_srvloop_depth++;
  uint32_t r = g_srvloop_orig(self);
  g_srvloop_depth--;
  return r;
}

/* Camera::RenderScene, through every vtable that holds it */
static uint32_t render_wrap(void *self) {
  if (!g_owed || g_render_depth || g_ov_active) {
    if (g_render_depth || g_ov_active) g_st.nested_skips++;
    g_render_depth++;
    uint32_t r = g_render_orig(self);
    g_render_depth--;
    return r;
  }
  g_owed = 0;
  g_work_this = g_owed_this;
  g_render_done = 0;
  while (sceKernelPollSema(g_sema_render, 1) == 0) {}
  g_ov_active = 1;
  __sync_synchronize();
  sceKernelSignalSema(g_sema_kick, 1);

  uint64_t t0 = sceKernelGetProcessTimeWide();
  g_render_depth++;
  uint32_t r = g_render_orig(self);
  g_render_depth--;
  uint64_t t1 = sceKernelGetProcessTimeWide();

  __sync_synchronize();
  g_render_done = 1;
  sceKernelSignalSema(g_sema_render, 1);
  sceKernelWaitSema(g_sema_done, 1, NULL);
  __sync_synchronize();
  g_ov_active = 0;
  uint64_t t2 = sceKernelGetProcessTimeWide();

  g_st.kicks++;
  g_st.render_us += t1 - t0;
  g_st.join_us += t2 - t1;
  g_st.ai_us += g_work_us;
  g_st.wait_us += g_work_wait_us;
  if (g_work_wait_us) g_st.gated_ticks++;
  return r;
}

/* ---- install ------------------------------------------------------------- */

typedef struct { uintptr_t key; const char *name; int flush; unsigned stub; } ovkey_t;

static int key_cmp(const void *a, const void *b) {
  uintptr_t x = ((const ovkey_t *)a)->key, y = ((const ovkey_t *)b)->key;
  return x < y ? -1 : x > y;
}

static void slot_write(uintptr_t *p, uintptr_t v) {
  kuKernelCpuUnrestrictedMemcpy(p, &v, sizeof v);
}

#define NRELS(m) ((m)->num_reldyn + (m)->num_relplt)
static Elf32_Rel *rel_at(so_module *m, int i) {
  return i < m->num_reldyn ? &m->reldyn[i] : &m->relplt[i - m->num_reldyn];
}

/* Point every relocated slot (GOT, vtable) holding `from` at `to`. */
static unsigned redirect_value(so_module *m, uintptr_t from, uintptr_t to) {
  unsigned n = 0;
  for (int i = 0; i < NRELS(m); i++) {
    uintptr_t *p = (uintptr_t *)(m->text_base + rel_at(m, i)->r_offset);
    if (*p == from) { slot_write(p, to); n++; }
  }
  return n;
}

void srv_overlap_install(void) {
  if (!g_srv_overlap_enabled) {
    log_printf("[ov] server/render overlap off (ServerOverlap=0)");
    return;
  }
  so_module *m = &kotor_mod;
  uintptr_t ai = so_symbol(m, "_ZN15CServerAIMaster11UpdateStateEv");
  uintptr_t rs = so_symbol(m, "_ZN6Camera11RenderSceneEv");
  uintptr_t sl = so_symbol(m, "_ZN13CServerExoApp8MainLoopEv");
  if (!ai || !rs || !sl) {
    log_printf("[ov] symbols missing (ai=%p render=%p loop=%p); overlap off",
               (void *)ai, (void *)rs, (void *)sl);
    g_srv_overlap_enabled = 0;
    return;
  }

  g_sema_kick = sceKernelCreateSema("ov_kick", 0, 0, 1, NULL);
  g_sema_done = sceKernelCreateSema("ov_done", 0, 0, 1, NULL);
  g_sema_render = sceKernelCreateSema("ov_render", 0, 0, 1, NULL);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, SRV_OVERLAP_STACK);
  pthread_t th;
  if (g_sema_kick < 0 || g_sema_done < 0 || g_sema_render < 0 ||
      pthread_create(&th, &attr, worker_thread, NULL) != 0) {
    log_printf("[ov] worker setup failed; overlap off");
    g_srv_overlap_enabled = 0;
    return;
  }
  sceKernelWaitSema(g_sema_done, 1, NULL);       /* stack bounds are in */

  /* Pass 1: every slot that should go through a stub, keyed by its value. */
  int nrel = NRELS(m);
  ovkey_t *keys = malloc(sizeof(ovkey_t) * (size_t)nrel);
  unsigned nk = 0;
  uintptr_t *flush_addr = calloc(NFLUSH, sizeof(uintptr_t));
  for (unsigned i = 0; i < NFLUSH; i++) flush_addr[i] = so_symbol(m, g_flush_syms[i]);

  /* defined functions by address, for slots that carry no symbol (RELATIVE) */
  ovkey_t *defs = malloc(sizeof(ovkey_t) * (size_t)m->num_dynsym);
  unsigned nd = 0;
  for (int i = 0; i < m->num_dynsym; i++) {
    Elf32_Sym *s = &m->dynsym[i];
    if (s->st_shndx == SHN_UNDEF || ELF32_ST_TYPE(s->st_info) != STT_FUNC) continue;
    const char *nm = m->dynstr + s->st_name;
    uintptr_t a = m->text_base + s->st_value;
    if (a == ai || a == rs || a == sl) continue;
    int fl = 0;
    for (unsigned j = 0; j < NFLUSH; j++) if (flush_addr[j] == a) fl = 1;
    if (!fl && !gated_name(nm)) continue;
    defs[nd].key = a; defs[nd].name = nm; defs[nd].flush = fl; nd++;
  }
  qsort(defs, nd, sizeof *defs, key_cmp);

  for (int i = 0; i < nrel; i++) {
    Elf32_Rel *r = rel_at(m, i);
    uintptr_t v = *(uintptr_t *)(m->text_base + r->r_offset);
    Elf32_Sym *s = &m->dynsym[ELF32_R_SYM(r->r_info)];
    ovkey_t probe = { v, NULL, 0, 0 };
    ovkey_t *d = nd ? bsearch(&probe, defs, nd, sizeof *defs, key_cmp) : NULL;
    if (d) { keys[nk++] = *d; continue; }
    if (ELF32_R_SYM(r->r_info) && s->st_shndx == SHN_UNDEF && v) {
      const char *nm = m->dynstr + s->st_name;
      if (gated_name(nm)) { keys[nk].key = v; keys[nk].name = nm; keys[nk].flush = 0; nk++; }
    }
  }
  qsort(keys, nk, sizeof *keys, key_cmp);
  unsigned nu = 0;
  for (unsigned i = 0; i < nk; i++)
    if (!nu || keys[nu - 1].key != keys[i].key) keys[nu++] = keys[i];

  /* Stubs, in one RX block. */
  size_t bytes = ((size_t)nu * STUB_BYTES + 0xfff) & ~(size_t)0xfff;
  SceKernelAllocMemBlockKernelOpt opt;
  memset(&opt, 0, sizeof opt);
  opt.size = sizeof opt;
  SceUID blk = kuKernelAllocMemBlock("ov_stubs", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, bytes, &opt);
  void *base = NULL;
  g_stub = calloc(nu ? nu : 1, sizeof *g_stub);
  uint32_t *buf = malloc(bytes);
  if (blk < 0 || sceKernelGetMemBlockBase(blk, &base) < 0 || !base || !g_stub || !buf || !keys) {
    log_printf("[ov] stub memory failed (%u stubs); overlap off", nu);
    g_srv_overlap_enabled = 0;
    return;
  }
  for (unsigned i = 0; i < nu; i++) {
    g_stub[i].name = keys[i].name;
    g_stub[i].orig = keys[i].key;
    g_stub[i].flush = (uint8_t)keys[i].flush;
    keys[i].stub = i;
    uintptr_t lo = keys[i].flush ? 0 : g_worker_lo;
    uintptr_t hi = keys[i].flush ? ~(uintptr_t)0 : g_worker_hi;
    emit_stub(buf + i * (STUB_BYTES / 4), lo, hi, keys[i].key, i);
  }
  kuKernelCpuUnrestrictedMemcpy(base, buf, (size_t)nu * STUB_BYTES);
  kuKernelFlushCaches(base, (size_t)nu * STUB_BYTES);
  free(buf);
  g_nstub = nu;

  /* Pass 2: write the slots. The three wrappers first, by value. */
  g_ai_orig = (ai_fn_t)ai;
  g_render_orig = (fn1_t)rs;
  g_srvloop_orig = (fn1_t)sl;
  unsigned n_ai = redirect_value(m, ai, (uintptr_t)&ai_wrap);
  unsigned n_rs = redirect_value(m, rs, (uintptr_t)&render_wrap);
  unsigned n_sl = redirect_value(m, sl, (uintptr_t)&srvloop_wrap);
  unsigned n_slots = 0, n_flush = 0;
  for (int i = 0; i < nrel; i++) {
    uintptr_t *p = (uintptr_t *)(m->text_base + rel_at(m, i)->r_offset);
    ovkey_t probe = { *p, NULL, 0, 0 };
    ovkey_t *k = nu ? bsearch(&probe, keys, nu, sizeof *keys, key_cmp) : NULL;
    if (!k) continue;
    slot_write(p, (uintptr_t)base + k->stub * STUB_BYTES);
    n_slots++;
    if (k->flush) n_flush++;
  }
  free(keys);
  free(defs);
  free(flush_addr);
  log_printf("[ov] armed: AI tick slots=%u, render slots=%u, server-loop slots=%u; "
             "%u stubs over %u slots (%u lifecycle); worker stack %p-%p",
             n_ai, n_rs, n_sl, nu, n_slots, n_flush, (void *)g_worker_lo, (void *)g_worker_hi);
  if (!n_ai || !n_rs || !n_sl)
    log_printf("[ov] WARNING: a wrapper found no slot -- the overlap will not engage");
}

void srv_overlap_window_report(void) {
  if (!g_srv_overlap_enabled) return;
  unsigned k = g_st.kicks;
  if (k || g_st.sync_runs) {
    log_printf("[ov] %u overlapped ticks, %u run in order; per overlapped frame: render %.1f ms, "
               "AI %.1f ms (parked %.1f), game thread waited %.1f ms after render; "
               "%u ticks hit a gate, %u GL gates, %u nested renders",
               k, g_st.sync_runs,
               k ? g_st.render_us / 1000.0 / k : 0.0, k ? g_st.ai_us / 1000.0 / k : 0.0,
               k ? g_st.wait_us / 1000.0 / k : 0.0, k ? g_st.join_us / 1000.0 / k : 0.0,
               g_st.gated_ticks, g_st.gl_waits, g_st.nested_skips);
    /* the gates that parked the AI most this window */
    char b[480];
    int o = snprintf(b, sizeof b, "[ov]   gates:");
    unsigned shown = 0;
    for (int pass = 0; pass < 6; pass++) {
      int best = -1;
      for (unsigned i = 0; i < g_nstub; i++)
        if (g_stub[i].waits && (best < 0 || g_stub[i].waits > g_stub[best].waits)) best = (int)i;
      if (best < 0 || o > (int)sizeof b - 96) break;
      o += snprintf(b + o, sizeof b - o, " %s=%u", g_stub[best].name, g_stub[best].waits);
      g_stub[best].waits = 0;
      shown++;
    }
    if (shown) log_printf("%s", b);
  }
  for (unsigned i = 0; i < g_nstub; i++) g_stub[i].waits = 0;
  memset(&g_st, 0, sizeof g_st);
}

#else
void srv_overlap_install(void) {}
void srv_overlap_window_report(void) {}
void srv_overlap_gl_guard(void) {}
#endif
