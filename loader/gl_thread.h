/* gl_thread.h -- run the game's GL stream on a second core (see gl_thread.c) */

#ifndef __GL_THREAD_H__
#define __GL_THREAD_H__

#include <stdint.h>
#include "config.h"

/* 1 while GL calls are being queued for the render thread. Before
 * gl_thread_start() and after gl_thread_stop() every glq_* call runs inline. */
extern volatile int g_glq_on;

/* Set by the render thread while it runs an RPC: the game thread is parked
 * then, so anything the RPC'd function calls must run inline, not queue. */
extern volatile int g_glq_in_rpc;

/* Make the ring and the thread; call once vitaGL is up. Queuing starts at
 * gl_thread_start(). Both return quietly (and leave GL inline) on failure. */
void gl_thread_init(void);
void gl_thread_start(void);
/* Drain and fall back to inline GL for good (fatal_error). */
void gl_thread_stop(void);

/* Queue fn(a0..a5). fn is called with six 32-bit core-register/stack args, so
 * it must take only integers/pointers (float shims take uint32_t bit
 * patterns). Runs inline when the queue is off. */
void glq_put(const void *fn, unsigned n, uint32_t a0, uint32_t a1, uint32_t a2,
             uint32_t a3, uint32_t a4, uint32_t a5);

/* Make queued calls visible to the render thread. */
void glq_flush(void);

/* The producer's ring state, for the inline fast path below. Game thread
 * only. */
#define GLQ_RING_WORDS ((GL_THREAD_RING_KB * 1024u) / 4u)
typedef struct {
  uint32_t *ring;
  uint32_t wpos;                 /* next free word                     */
  uint32_t pub;                  /* last published wpos                */
  uint32_t tail_seen;            /* last g_tail read                   */
  unsigned cmds;
} glq_prod_t;
extern glq_prod_t g_glqp;

/* glq_put with the common case inlined at the call site: the queue is on and
 * the record fits before the ring's end in space already known to be free.
 * With a constant n the argument stores unroll. Anything else -> glq_put. */
static inline __attribute__((always_inline))
void glq_put_fast(const void *fn, unsigned n, uint32_t a0, uint32_t a1, uint32_t a2,
                  uint32_t a3, uint32_t a4, uint32_t a5) {
  uint32_t need = 2u + n, w = g_glqp.wpos, idx = w & (GLQ_RING_WORDS - 1u);
  if (__builtin_expect(g_glq_on && !g_glq_in_rpc && idx + need <= GLQ_RING_WORDS &&
                       w + need - g_glqp.tail_seen <= GLQ_RING_WORDS, 1)) {
    uint32_t *r = g_glqp.ring + idx;
    r[0] = (uint32_t)fn;
    r[1] = n | (need << 8);
    if (n > 0) r[2] = a0;
    if (n > 1) r[3] = a1;
    if (n > 2) r[4] = a2;
    if (n > 3) r[5] = a3;
    if (n > 4) r[6] = a4;
    if (n > 5) r[7] = a5;
    g_glqp.wpos = w + need;
    g_glqp.cmds++;
    if (g_glqp.wpos - g_glqp.pub >= GL_THREAD_FLUSH_WORDS) glq_flush();
    return;
  }
  glq_put(fn, n, a0, a1, a2, a3, a4, a5);
}

/* As glq_put, but first copies `bytes` from `src` into the ring and passes the
 * copy as argument `ptr_arg` -- for calls that read caller memory (uniform
 * arrays, buffer data, client indices). Too big for the ring -> RPC. */
void glq_put_copy(const void *fn, unsigned n, const uint32_t *args,
                  unsigned ptr_arg, const void *src, uint32_t bytes);

/* Run fn(args[0..8]) on the render thread after everything queued before it,
 * wait, and return its r0. Inline when the queue is off. */
uint32_t glq_rpc(const void *fn, const uint32_t *args);

/* Flush and wait until the render thread has run everything queued. */
void glq_sync(void);

/* Present. Inline when the queue is off. `ime` composites the system
 * keyboard, which keeps the swap synchronous. */
void gl_thread_swap(int ime);

/* Called after every RPC, on the game thread, with the render thread idle:
 * lets gl_patch re-read the state it mirrors. */
extern void (*g_glq_after_rpc)(void);

/* Build ARM thunks that turn a call into glq_rpc(target, args). out[i] gets
 * the thunk for targets[i]. Returns 0 on failure (out untouched). */
int glq_make_rpc_thunks(const uintptr_t *targets, uintptr_t *out, unsigned n);

/* Per-window line; `name_of` maps an RPC target back to a GL name. */
void gl_thread_report(unsigned frames, const char *(*name_of)(uintptr_t fn));

#endif
