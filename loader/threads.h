/* threads.h -- which CPU core each thread runs on, and how busy each one is
 *
 * The Vita gives an app three cores (USER_0..2). The game's own loop and every
 * GL call live on one thread inside libKOTOR.so, which we cannot split, so
 * that core is the frame-time bottleneck. What we can do is keep everything
 * else off it: the game thread gets core 0 to itself, and every other thread
 * -- the game's workers, our audio mixer, the log writer, vitaGL's garbage
 * collector -- is pinned to core 1 or 2.
 *
 * The census half records every thread we start or see started, and the
 * watchdog logs each one's share of the last window, so a log says which
 * core is busy and with what.
 */

#ifndef __THREADS_H__
#define __THREADS_H__

#include <psp2/kernel/cpu.h>
#include <psp2/types.h>

#define CPU_GAME   SCE_KERNEL_CPU_MASK_USER_0   /* SDL_main + GL, nothing else */
#define CPU_AUX_A  SCE_KERNEL_CPU_MASK_USER_1   /* audio mixer, half the workers */
#define CPU_AUX_B  SCE_KERNEL_CPU_MASK_USER_2   /* parked main, log, GC, the rest */

/* The core mask to create a helper thread with: `mask` when pinning is on,
 * 0 (the kernel default) when THREAD_PINNING is off. */
int thread_mask(int mask);

/* The next core for a game worker, alternating CPU_AUX_B and CPU_AUX_A. */
int thread_next_worker_mask(void);

/* Move the CALLING thread to `mask` (a no-op with pinning off) and record it
 * under `tag`. Safe from any thread. */
void thread_pin_self(int mask, const char *tag);

/* Record a thread we created with sceKernelCreateThread. */
void thread_census_add(SceUID thid, const char *tag);

/* Log every recorded thread: core, priority and its share of the time since
 * the last call, plus a per-core total. Called from the watchdog. */
void thread_census_log(void);

#endif
