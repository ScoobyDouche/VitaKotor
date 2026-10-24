/* gl_thread.c -- run the game's GL stream on a second core.
 *
 * The slow areas are CPU-bound on the game thread: ~550 draws a frame, and
 * vitaGL spends ~12 us of game-thread time on each (log 2026-10-22). Nothing
 * about the draws needs the game thread; it only needs them to happen in
 * order. So the GL calls that just set state or draw are written into a ring
 * and a render thread on another core replays them, while the game thread
 * goes on walking the scene.
 *
 * Three kinds of call:
 *   queued -- glq_put/glq_put_copy: by-value state and draws. Arguments that
 *             point at game memory (uniform arrays, buffer data, client index
 *             lists) are copied into the ring with the call.
 *   RPC    -- glq_rpc: anything that returns a value or reads memory we
 *             cannot size (texture uploads, glGen*, shader setup). The game
 *             thread waits while the render thread catches up and runs it,
 *             so pointers stay valid and the answer is exact.
 *   inline -- before gl_thread_start(), after gl_thread_stop(), and inside an
 *             RPC, every call runs on the calling thread as it always did.
 *
 * vitaGL itself only ever runs on one thread at a time: the render thread
 * while queuing is on, the game thread (or the loader's own screens) before.
 *
 * The ring is single-producer (the game thread) single-consumer. Records:
 *   [fn][hdr: n | words << 8 | RPC bit][args...][payload...]
 * A zero fn word means "wrapped: continue at the start". */

#include <vitasdk.h>
#include <kubridge.h>
#include <vitaGL.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdio.h>

#include "config.h"
#include "gl_thread.h"
#include "threads.h"
#include "log.h"

#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX 0x0C20D050
#endif

#define RING_WORDS  GLQ_RING_WORDS                       /* power of two */
#define RING_MASK   (RING_WORDS - 1u)
#define REC_MAX     (RING_WORDS / 4u)
#define HDR_RPC     0x80000000u
#define RPC_WORDS   12u                                  /* fn, hdr, 9 args, ret */
#define RPC_STATS   16

typedef void (*fn6_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef uint32_t (*fn9_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                          uint32_t, uint32_t, uint32_t, uint32_t);

/* Every variable one core writes while the other reads gets a cache line of
 * its own. The first build kept g_tail beside the producer's state and read
 * it on every call while the consumer rewrote it on every record: the line
 * bounced between cores ~10k times a frame and the queuing cost as much as
 * the GL it moved (first hardware test: frame times unchanged). */
#define LINE __attribute__((aligned(64)))

LINE volatile int g_glq_on = 0;          /* read per call, written twice  */
LINE volatile int g_glq_in_rpc = 0;      /* consumer, around RPCs only    */
void (*g_glq_after_rpc)(void) = NULL;

LINE static volatile uint32_t g_head;    /* producer publishes            */
LINE static volatile uint32_t g_tail;    /* consumer publishes, batched   */
LINE static volatile int g_cons_sleeping;
LINE static volatile int g_prod_waiting;
LINE static volatile uint32_t g_swaps_done;

/* Producer-private, shared with glq_put_fast() in gl_thread.h. tail_seen is
 * the last g_tail we read: space is checked against it and g_tail is only
 * re-read when it says the ring is full. */
LINE glq_prod_t g_glqp;
#define g_ring      g_glqp.ring
#define g_wpos      g_glqp.wpos
#define g_pub       g_glqp.pub
#define g_tail_seen g_glqp.tail_seen
#define g_cmds      g_glqp.cmds
#if GL_THREAD_ASYNC_SWAP
static uint32_t g_swaps_queued;
#endif
static SceUID g_work_sema = -1, g_prod_sema = -1;

/* Window stats. Producer-side ones are game-thread only; the consumer's sleep
 * total is read racily by the report, which is fine for a log line. */
enum { W_SPACE, W_DRAIN, W_SWAP, W_KINDS };
static uint64_t g_wait_us[W_KINDS];
static unsigned g_flushes, g_rpcs, g_copies, g_copy_bytes;
static uint32_t g_ring_hw;
LINE static volatile uint64_t g_sleep_us;   /* consumer */
static volatile unsigned g_sleeps;
static struct { uintptr_t fn; unsigned n; uint64_t us; } g_rpc_by[RPC_STATS];

static inline uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

/* ---- render thread -------------------------------------------------------- */

#if GL_THREAD

static inline void wake_producer(void) {
  if (g_prod_waiting) {
    g_prod_waiting = 0;
    sceKernelSignalSema(g_prod_sema, 1);
  }
}

static int render_thread(SceSize args, void *argp) {
  (void)args; (void)argp;
  uint32_t t = g_tail;
  unsigned k = 0;
  for (;;) {
    uint32_t h = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
    if (t == h) {
      __atomic_store_n(&g_tail, t, __ATOMIC_RELEASE);  /* caught up: say so */
      __sync_synchronize();
      wake_producer();
      for (unsigned i = 0; i < GL_THREAD_SPIN; i++)
        if (__atomic_load_n(&g_head, __ATOMIC_ACQUIRE) != t) break;
      if (__atomic_load_n(&g_head, __ATOMIC_ACQUIRE) != t) continue;
      g_cons_sleeping = 1;
      __sync_synchronize();
      if (__atomic_load_n(&g_head, __ATOMIC_ACQUIRE) == t) {
        uint64_t t0 = now_us();
        sceKernelWaitSema(g_work_sema, 1, NULL);
        g_sleep_us += now_us() - t0;
        g_sleeps++;
      }
      g_cons_sleeping = 0;
      continue;
    }
    while (t != h) {
      uint32_t *r = &g_ring[t & RING_MASK];
      if (!r[0]) {                                   /* wrap marker */
        t += RING_WORDS - (t & RING_MASK);
        continue;
      }
      uint32_t hdr = r[1];
      if (hdr & HDR_RPC) {
        g_glq_in_rpc = 1;
        r[11] = ((fn9_t)r[0])(r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10]);
        g_glq_in_rpc = 0;
      } else {
        ((fn6_t)r[0])(r[2], r[3], r[4], r[5], r[6], r[7]);
      }
      t += (hdr >> 8) & 0x7fffffu;
      /* Publish progress every 64 records, or at once when the producer is
       * waiting for it (an RPC, a drain, a full ring). */
      if (g_prod_waiting || !(++k & 63)) {
        __atomic_store_n(&g_tail, t, __ATOMIC_RELEASE);
        wake_producer();
      }
    }
  }
  return 0;
}
#endif

/* ---- producer side -------------------------------------------------------- */

static inline int cond_met(int kind, uint32_t arg) {
  switch (kind) {
  case W_SPACE:
    if (g_wpos + arg - g_tail_seen <= RING_WORDS) return 1;
    g_tail_seen = __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE);
    return g_wpos + arg - g_tail_seen <= RING_WORDS;
  case W_DRAIN:
    g_tail_seen = __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE);
    return g_tail_seen == g_wpos;
  default:
    return __atomic_load_n(&g_swaps_done, __ATOMIC_ACQUIRE) >= arg;
  }
}

/* Block until the condition holds. The consumer signals after every record
 * while g_prod_waiting is set; the timeout only bounds a missed wake. */
static void wait_until(int kind, uint32_t arg) {
  if (cond_met(kind, arg)) return;
  uint64_t t0 = now_us();
  for (unsigned i = 0; i < GL_THREAD_SPIN && !cond_met(kind, arg); i++) {}
  while (!cond_met(kind, arg)) {
    g_prod_waiting = 1;
    __sync_synchronize();
    if (cond_met(kind, arg)) { g_prod_waiting = 0; break; }
    SceUInt to = 1000;
    sceKernelWaitSema(g_prod_sema, 1, &to);
  }
  g_wait_us[kind] += now_us() - t0;
}

void glq_flush(void) {
  if (g_pub == g_wpos) return;
  g_pub = g_wpos;
  g_flushes++;
  __atomic_store_n(&g_head, g_wpos, __ATOMIC_RELEASE);
  __sync_synchronize();
  if (g_cons_sleeping) {
    g_cons_sleeping = 0;
    sceKernelSignalSema(g_work_sema, 1);
  }
}

static uint32_t *reserve(uint32_t need) {
  uint32_t idx = g_wpos & RING_MASK;
  if (idx + need > RING_WORDS) {                 /* would straddle the end */
    uint32_t pad = RING_WORDS - idx;
    if (!cond_met(W_SPACE, pad + need)) { glq_flush(); wait_until(W_SPACE, pad + need); }
    g_ring[idx] = 0;
    g_wpos += pad;
    idx = 0;
  } else if (!cond_met(W_SPACE, need)) {
    glq_flush();
    wait_until(W_SPACE, need);
  }
  return &g_ring[idx];
}

static inline void commit(uint32_t words) {
  g_wpos += words;
  g_cmds++;
  uint32_t used = g_wpos - g_tail_seen;     /* upper bound; no cross-core read */
  if (used > g_ring_hw) g_ring_hw = used;
  if (g_wpos - g_pub >= GL_THREAD_FLUSH_WORDS) glq_flush();
}

void glq_put(const void *fn, unsigned n, uint32_t a0, uint32_t a1, uint32_t a2,
             uint32_t a3, uint32_t a4, uint32_t a5) {
  if (!g_glq_on || g_glq_in_rpc) {
    ((fn6_t)fn)(a0, a1, a2, a3, a4, a5);
    return;
  }
  uint32_t *r = reserve(2 + n);
  r[0] = (uint32_t)fn;
  r[1] = n | ((2u + n) << 8);
  uint32_t a[6] = { a0, a1, a2, a3, a4, a5 };
  for (unsigned i = 0; i < n; i++) r[2 + i] = a[i];
  commit(2 + n);
}

void glq_put_copy(const void *fn, unsigned n, const uint32_t *args,
                  unsigned ptr_arg, const void *src, uint32_t bytes) {
  uint32_t a[9] = { 0 };
  for (unsigned i = 0; i < n && i < 9; i++) a[i] = args[i];
  uint32_t words = 2 + n + ((bytes + 3u) >> 2);
  if (!g_glq_on || g_glq_in_rpc || n > 6 || words > REC_MAX) {
    glq_rpc(fn, a);                              /* runs inline when off */
    return;
  }
  if (!src || !bytes) {                          /* nothing to copy */
    glq_put(fn, n, a[0], a[1], a[2], a[3], a[4], a[5]);
    return;
  }
  uint32_t *r = reserve(words);
  uint32_t *payload = r + 2 + n;
  memcpy(payload, src, bytes);
  a[ptr_arg] = (uint32_t)payload;
  r[0] = (uint32_t)fn;
  r[1] = n | (words << 8);
  for (unsigned i = 0; i < n; i++) r[2 + i] = a[i];
  g_copies++;
  g_copy_bytes += bytes;
  commit(words);
}

static void rpc_note(const void *fn, uint64_t us) {
  unsigned i;
  for (i = 0; i < RPC_STATS && g_rpc_by[i].fn && g_rpc_by[i].fn != (uintptr_t)fn; i++) {}
  if (i == RPC_STATS) i = RPC_STATS - 1;         /* full: lump into the last */
  if (!g_rpc_by[i].fn) g_rpc_by[i].fn = (uintptr_t)fn;
  g_rpc_by[i].n++;
  g_rpc_by[i].us += us;
}

uint32_t glq_rpc(const void *fn, const uint32_t *a) {
  if (!g_glq_on || g_glq_in_rpc)
    return ((fn9_t)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
  uint64_t t0 = now_us();
  uint32_t *r = reserve(RPC_WORDS);
  r[0] = (uint32_t)fn;
  r[1] = HDR_RPC | 9u | (RPC_WORDS << 8);
  for (unsigned i = 0; i < 9; i++) r[2 + i] = a[i];
  r[11] = 0;
  g_rpcs++;
  commit(RPC_WORDS);
  glq_flush();
  wait_until(W_DRAIN, 0);
  uint32_t ret = r[11];                          /* we wrote nothing since */
  rpc_note(fn, now_us() - t0);
  if (g_glq_after_rpc) g_glq_after_rpc();
  return ret;
}

void glq_sync(void) {
  if (!g_glq_on || g_glq_in_rpc) return;
  glq_flush();
  wait_until(W_DRAIN, 0);
}

/* ---- presenting ------------------------------------------------------------ */

#if GL_THREAD_ASYNC_SWAP
static void swap_async(void) {
  vglSwapBuffers(GL_FALSE);
  __atomic_store_n(&g_swaps_done, g_swaps_done + 1, __ATOMIC_RELEASE);
}
#endif
static uint32_t swap_sync(uint32_t ime) {
  vglSwapBuffers(ime ? GL_TRUE : GL_FALSE);
  return 0;
}

void gl_thread_swap(int ime) {
  if (!g_glq_on) {
    vglSwapBuffers(ime ? GL_TRUE : GL_FALSE);
    return;
  }
#if GL_THREAD_ASYNC_SWAP
  if (!ime) {
    /* One frame in flight: the render thread may still be drawing and
     * presenting frame N while the game builds N+1, but no further. */
    wait_until(W_SWAP, g_swaps_queued);
    g_swaps_queued++;
    glq_put((const void *)swap_async, 0, 0, 0, 0, 0, 0, 0);
    glq_flush();
    return;
  }
#endif
  uint32_t a[9] = { (uint32_t)(ime != 0) };
  glq_rpc((const void *)swap_sync, a);
}

/* ---- lifecycle ------------------------------------------------------------- */

void gl_thread_init(void) {
#if GL_THREAD
  if (g_ring) return;
  g_ring = (uint32_t *)memalign(64, RING_WORDS * 4u + 64u);  /* +slack: fn6 reads 6 args */
  if (!g_ring) {
    log_printf("[glq] no memory for a %u KB ring -- GL stays on the game thread",
               (unsigned)GL_THREAD_RING_KB);
    return;
  }
  memset(g_ring, 0, RING_WORDS * 4u + 64u);
  g_work_sema = sceKernelCreateSema("glq_work", 0, 0, 0x7fffffff, NULL);
  g_prod_sema = sceKernelCreateSema("glq_prod", 0, 0, 0x7fffffff, NULL);
  SceUID thid = sceKernelCreateThread("gl_render", render_thread, GL_THREAD_PRIORITY,
                                 GL_THREAD_STACK, 0, thread_mask(CPU_AUX_B), NULL);
  if (g_work_sema < 0 || g_prod_sema < 0 || thid < 0 ||
      sceKernelStartThread(thid, 0, NULL) < 0) {
    log_printf("[glq] render thread setup failed (sema 0x%x/0x%x thread 0x%x) -- "
               "GL stays on the game thread",
               (unsigned)g_work_sema, (unsigned)g_prod_sema, (unsigned)thid);
    free(g_ring);
    g_ring = NULL;
    return;
  }
  thread_census_add(thid, "gl-render");
  log_printf("[glq] render thread ready: %u KB ring, core mask 0x%x, async swap %d",
             (unsigned)GL_THREAD_RING_KB, (unsigned)thread_mask(CPU_AUX_B),
             GL_THREAD_ASYNC_SWAP);
#endif
}

void gl_thread_start(void) {
  if (!g_ring || g_glq_on) return;
  g_glq_on = 1;
  log_printf("[glq] GL calls now run on the render thread");
}

void gl_thread_stop(void) {
  if (!g_glq_on) return;
  if (!g_glq_in_rpc) glq_sync();   /* on the render thread there is nothing to wait for */
  g_glq_on = 0;
}

/* ---- thunks ---------------------------------------------------------------- */

/* Each thunk is 12 ARM words. push {r0-r3} lays the register arguments right
 * below the caller's stack arguments, so sp then points at args[0..8] in
 * order, whatever the signature:
 *     push {r0-r3}; push {r4, lr}; add r1, sp, #8
 *     ldr r0, =target; ldr ip, =glq_rpc; blx ip
 *     pop {r4, lr}; add sp, sp, #16; bx lr                              */
int glq_make_rpc_thunks(const uintptr_t *targets, uintptr_t *out, unsigned n) {
  size_t sz = ((size_t)n * 48 + 0xfff) & ~(size_t)0xfff;
  SceKernelAllocMemBlockKernelOpt opt;
  memset(&opt, 0, sizeof opt);
  opt.size = sizeof opt;
  SceUID blk = kuKernelAllocMemBlock("glq_thunks", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, sz, &opt);
  void *base = NULL;
  if (blk < 0 || sceKernelGetMemBlockBase(blk, &base) < 0 || !base) {
    log_printf("[glq] thunk page failed (0x%08x)", (unsigned)blk);
    return 0;
  }
  uint32_t *code = (uint32_t *)malloc(sz);
  if (!code) return 0;
  memset(code, 0, sz);
  for (unsigned i = 0; i < n; i++) {
    uint32_t *t = code + i * 12;
    t[0]  = 0xe92d000f;                    /* push {r0-r3}         */
    t[1]  = 0xe92d4010;                    /* push {r4, lr}        */
    t[2]  = 0xe28d1008;                    /* add  r1, sp, #8      */
    t[3]  = 0xe59f0010;                    /* ldr  r0, [pc, #16]   */
    t[4]  = 0xe59fc010;                    /* ldr  ip, [pc, #16]   */
    t[5]  = 0xe12fff3c;                    /* blx  ip              */
    t[6]  = 0xe8bd4010;                    /* pop  {r4, lr}        */
    t[7]  = 0xe28dd010;                    /* add  sp, sp, #16     */
    t[8]  = 0xe12fff1e;                    /* bx   lr              */
    t[9]  = (uint32_t)targets[i];
    t[10] = (uint32_t)&glq_rpc;
    out[i] = (uintptr_t)base + i * 48;     /* ARM entry */
  }
  kuKernelCpuUnrestrictedMemcpy(base, code, sz);
  kuKernelFlushCaches(base, sz);
  free(code);
  return 1;
}

/* ---- report ---------------------------------------------------------------- */

void gl_thread_report(unsigned frames, const char *(*name_of)(uintptr_t fn)) {
  static uint64_t prev_sleep;
  static unsigned prev_sleeps;
  if (!g_ring || !frames) return;
  uint64_t sl = g_sleep_us;
  unsigned sn = g_sleeps;
  char line[480];
  int o = snprintf(line, sizeof line,
                   "[glq] per frame: cmds=%u rpc=%u copies=%u(%u B) flush=%u | game waited "
                   "drain=%.2f space=%.2f swap=%.2f ms | render slept %.2f ms (%u) | ring hw %u KB | rpc:",
                   g_cmds / frames, g_rpcs / frames, g_copies / frames, g_copy_bytes / frames,
                   g_flushes / frames,
                   (double)g_wait_us[W_DRAIN] / 1000.0 / frames,
                   (double)g_wait_us[W_SPACE] / 1000.0 / frames,
                   (double)g_wait_us[W_SWAP] / 1000.0 / frames,
                   (double)(sl - prev_sleep) / 1000.0 / frames, (sn - prev_sleeps) / frames,
                   (unsigned)(g_ring_hw * 4u / 1024u));
  for (unsigned i = 0; i < RPC_STATS && g_rpc_by[i].fn; i++) {
    if (!g_rpc_by[i].n) continue;
    const char *nm = g_rpc_by[i].fn == (uintptr_t)swap_sync ? "swap"
                     : name_of ? name_of(g_rpc_by[i].fn) : NULL;
    if (o > (int)sizeof line - 48) break;
    if (nm)
      o += snprintf(line + o, sizeof line - o, " %s=%u/%.2fms", nm, g_rpc_by[i].n,
                    (double)g_rpc_by[i].us / 1000.0);
    else
      o += snprintf(line + o, sizeof line - o, " %p=%u/%.2fms", (void *)g_rpc_by[i].fn,
                    g_rpc_by[i].n, (double)g_rpc_by[i].us / 1000.0);
  }
  log_printf("%s", line);
  prev_sleep = sl;
  prev_sleeps = sn;
  memset(g_wait_us, 0, sizeof g_wait_us);
  memset(g_rpc_by, 0, sizeof g_rpc_by);
  g_cmds = g_flushes = g_rpcs = g_copies = g_copy_bytes = 0;
  g_ring_hw = 0;
}
