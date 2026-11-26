/* pcsamp.h -- a true program-counter profile of the game thread.
 *
 * sampler.c can only see the game thread when it makes a call, so a long
 * stretch of leaf code (a matrix loop, a culling test, a skinning pass) shows
 * up only as the call site that eventually follows it. This one sees the
 * instruction itself.
 *
 * Every tick (default 1 ms) a thread makes libKOTOR's text readable but not
 * executable. The next instruction any thread fetches from it raises a
 * prefetch abort; the handler records that thread's PC and LR, gives execute
 * back, and returns, so the instruction simply runs again. Only aborts on the
 * game thread's stack are tallied. Time spent outside libKOTOR (vitaGL, libc,
 * the port layer) is not sampled, so a sample lands on the next libKOTOR
 * instruction after it -- usually the return from that call.
 *
 * Off unless swkotor.ini [Vita Options] Profile=3. Needs a kubridge with
 * kuKernelMemProtect (the same one the crash handler uses); it checks at
 * install and stays off if the text cannot be protected. Reports every 15 s:
 *   [pcs]   pc k+<offset>:<count> ...   (4-byte buckets in libKOTOR)
 *   [pcs]   lr <addr>:<count> ...       (the return address at each sample)
 * Aggregate them by function with tools/samp_report.py.
 */
#pragma once

#include <vitasdk.h>

/* Call after crash_init (the crash handler stays behind this one) and after
 * libKOTOR is loaded. `game_thid` may still be unset now. */
void pcsamp_install(volatile SceUID *game_thid, unsigned tick_us);
