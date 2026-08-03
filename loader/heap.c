/* heap.c -- newlib heap telemetry, and surviving an exhausted heap
 *
 * log140 ended at t=926.9s with UNDEFINED_INSTRUCTION at _kill_r+0x42, which is
 * newlib's `udf #255` for a signal it cannot deliver -- r2=6, SIGABRT. r6 held
 * the "std::bad_alloc" what() string and r7 std::bad_alloc::~bad_alloc, and the
 * throw site in operator new builds the exception with exactly those two values.
 * So: an allocation failed, operator new threw bad_alloc, nothing in KOTOR
 * catches it, and terminate called abort. It happened 13s into loading
 * tar_m02aa, after the run had already loaded tar_m02af successfully -- so it is
 * cumulative pressure, not one oversized area.
 *
 * The game reaches our C++ runtime because dynlib.c binds _Znwj/_Znaj to it, and
 * GCC's operator new is a retry loop around malloc:
 *
 *     while ((p = malloc(sz)) == 0) {
 *       handler = std::get_new_handler();
 *       if (!handler) throw bad_alloc();      <-- what killed log140
 *       handler();
 *     }
 *
 * That loop is the supported place to intervene, and it covers new[] too since
 * _Znaj tail-calls _Znwj. We install a handler that hands back the decoded-audio
 * cache -- pure speed, worst case the next createSound decodes again -- and lets
 * the allocation retry. If there is nothing left to give it removes itself, so
 * the allocation throws exactly as it does today rather than spinning forever on
 * a handler that cannot make progress.
 *
 * The telemetry is the point of this pass. We have never had a number for heap
 * occupancy, so it is unknown whether the audio cache (22 MB held, 40 MB
 * allowed) is the difference between fitting and not. `headroom` below is the
 * number that settles it: unallocated sbrk room plus what is free inside the
 * arena.
 */

#include <vitasdk.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>

#include "heap.h"
#include "audio_patch.h"
#include "config.h"
#include "log.h"

volatile unsigned g_heap_last_new = 0;

/* std::set_new_handler. Declared by mangled name for the same reason dynlib.c
 * takes _Znwj that way: this is a C translation unit and the symbol is C++. */
extern void *_ZSt15set_new_handlerPFvvE(void (*)(void));
#define set_new_handler _ZSt15set_new_handlerPFvvE

#define HEAP_LIMIT_BYTES ((unsigned)MEMORY_NEWLIB_MB * 1024u * 1024u)

/* Tenths of a MB, so the log can show one decimal without pulling in float
 * formatting. */
static unsigned tenths_mb(unsigned bytes) {
  return (unsigned)(((uint64_t)bytes * 10u) / (1024u * 1024u));
}

void heap_log(const char *why) {
  struct mallinfo mi = mallinfo();
  unsigned arena = (unsigned)mi.arena;
  unsigned used  = (unsigned)mi.uordblks;
  unsigned freed = (unsigned)mi.fordblks;
  /* Room the heap could still take from sbrk, plus what is already free inside
   * it. This is the only figure that says whether we are actually close. */
  unsigned headroom = (arena < HEAP_LIMIT_BYTES ? HEAP_LIMIT_BYTES - arena : 0) + freed;
  unsigned pcm = audio_cache_bytes();

  log_printf("[heap] %s%u.%u MB used, %u.%u free in arena (%d blocks), "
             "arena %u.%u of %u MB -> headroom %u.%u MB; audio cache %u.%u MB",
             why ? why : "", tenths_mb(used) / 10, tenths_mb(used) % 10,
             tenths_mb(freed) / 10, tenths_mb(freed) % 10, mi.ordblks,
             tenths_mb(arena) / 10, tenths_mb(arena) % 10,
             (unsigned)MEMORY_NEWLIB_MB,
             tenths_mb(headroom) / 10, tenths_mb(headroom) % 10,
             tenths_mb(pcm) / 10, tenths_mb(pcm) % 10);
}

static void heap_new_handler(void) {
  log_printf("[heap] ALLOCATION FAILED (last new through dynlib was %u bytes)",
             g_heap_last_new);
  heap_log("at failure: ");

  unsigned got = audio_cache_purge();
  if (got) {
    log_printf("[heap] released %u.%u MB of decoded audio -- retrying",
               tenths_mb(got) / 10, tenths_mb(got) % 10);
    heap_log("after purge: ");
    return;                       /* operator new loops and tries malloc again */
  }

  /* Returning without freeing anything would spin operator new forever, which
   * is worse than the crash. Stand down and let it throw, as it did before. */
  log_printf("[heap] nothing left to release -- operator new will throw "
             "bad_alloc and the game does not catch it");
  set_new_handler(NULL);
}

void heap_init(void) {
  set_new_handler(heap_new_handler);
  heap_log("startup: ");
}
