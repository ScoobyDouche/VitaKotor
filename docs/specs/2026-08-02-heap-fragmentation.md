# Area transitions crash on a fragmented heap, not a full one

**Status:** measured on hardware (log141). Cause identified, **not yet fixed**.
**Date:** 2026-08-02

Follow-on from `2026-08-02-sound-decay-end-callback.md`. With the audio leak
fixed, log140 still died at an area transition, so `loader/heap.c` was added to
report occupancy every 3s and to hand the decoded-audio cache back when an
allocation fails. log141 is the first run with those numbers.

## The numbers

| t (s) | used | free | free blocks | arena | headroom | audio cache |
|---|---|---|---|---|---|---|
| 41 | 91.0 | 1.3 | 224 | 92.4 | 100.9 | 0.0 |
| 197 | 153.4 | 3.8 | 435 | 157.2 | 38.5 | 15.0 |
| 432 | 128.5 | 34.3 | 2540 | 162.8 | 63.4 | 17.2 |
| 627 | 116.1 | 55.4 | **9461** | 171.6 | 75.8 | 22.8 |
| 850 | 106.1 | 85.6 | 7878 | **191.8** | 85.8 | 3.6 |
| 886 | 128.9 | 62.8 | 3991 | **191.8** | 63.0 | 3.6 |

All figures MB except the block count. Three things fall out of this:

**There is no leak.** `used` oscillates between 97 and 155 MB across fifteen
minutes and keeps coming back down. Whatever is wrong, memory is being freed.

**The free-block count explodes.** 224 free blocks at t=41, 9461 at t=627. The
heap is being shredded into small pieces.

**The arena ratchets to the ceiling and stays.** dlmalloc extends via `sbrk`
whenever the free list cannot serve a request, so every failure to find a fit
grows the arena permanently. It reaches 191.8 of the 192 MB cap around t=850 and
can never extend again.

## The failure

Three allocation failures, all of them large requests:

```
[687.068984] ALLOCATION FAILED (last new through dynlib was 1048576 bytes)
   at failure:  154.1 MB used, 37.6 free (3479 blocks), arena 191.8 -> headroom 37.8
   released 29.9 MB of decoded audio -- retrying                       [survived]

[835.630929] ALLOCATION FAILED (last new through dynlib was 1572892 bytes)
   at failure:  154.4 MB used, 37.3 free (2317 blocks), arena 191.8 -> headroom 37.5
   released 10.7 MB of decoded audio -- retrying                       [survived]

[853.177611] ALLOCATION FAILED (last new through dynlib was 1048576 bytes)
   at failure:  128.9 MB used, 62.8 free (3994 blocks), arena 191.8 -> headroom 63.0
   nothing left to release                                             [crash]
```

**A 1 MB allocation failed with 62.8 MB free.** That is the whole finding. The
memory exists; no 1 MB of it is contiguous, and the arena is at the cap so
`sbrk` cannot supply fresh space. 62.8 MB across 3994 free blocks averages 16 KB
a block.

So the earlier reading — "the leaked sources ate the heap" — was right about
log139, where the audio leak really was consuming memory, and wrong as a general
account. Once that leak was gone the same crash came back with 63 MB free.

The new-handler bought two saves and about 166 seconds of play. It is a useful
backstop and it proved the audio cache is not the problem: at the fatal failure
the cache held 3.6 MB, so purging it could not have helped even in principle.

## What this rules in and out

- **Not a leak.** `used` is stable across the run.
- **Not the audio cache.** 3.6 MB held when it mattered.
- **Not one oversized area.** `tar_m02af` loaded earlier in the same session and
  was fine; the crash is cumulative.
- **Fragmentation, with a hard ceiling behind it.** The repeated 1 MB and
  1.5 MB requests are both the victim and, plausibly, the cause: allocating and
  freeing megabyte blocks against a general-purpose heap that also serves
  thousands of small game objects is what interleaves them.

## Candidate fixes, not yet chosen

1. **Recycle large blocks.** Intercept allocations above a threshold in the
   dynlib shims and serve them from size-classed free lists that are never
   returned to the general heap. The repeated 1 MB / 1.5 MB churn then stops
   touching, and stops shredding, the main arena. Needs `free`/`operator delete`
   interception that can recognise its own pointers, which is the risky part.
2. **Raise `MEMORY_NEWLIB_MB`.** Cheap, and directly addresses "the arena cannot
   extend". Postpones rather than solves, and how much room is actually
   available above 192 MB has not been established.
3. Both. (1) is the fix; (2) buys margin while (1) is proven.

The measurement that would decide between them, and is missing: **the largest
single free block**, which `mallinfo` does not report. A short binary search for
it in the heap sampler would say how far from a fit we actually are, and whether
a modest ceiling raise would have covered the failing request.

## log142 (2026-08-15): the arena ratchet has a named driver

The measurement build never reached the card -- log142 is stamped
`Aug 2 2026 22:16:46`, the same build as log141 -- so `largest block` and the
large-allocation census are **still unmeasured**. What log142 does add is a
third failure set and a correlation the earlier logs did not isolate.

Same ending as before: bad_alloc at t=921 during a save at an area transition,
the crash handler parks the faulting thread, and the last frame stays on screen.
That parked frame is what "it froze on the saving screen" looks like.

| t | failing request | arena | outcome |
|---|---|---|---|
| 455 | **292,854 B (286 KB)** | 191.7 / 192 | survived, purge freed 25.0 MB |
| 822 | 1,572,892 B (1.5 MB) | 187.0 / 192 | survived, purge freed 21.0 MB |
| 921 | 2,627,595 B (2.5 MB) | 190.6 / 192 | fatal, 2.5 MB left to release |

**A 286 KB request failed.** That retires candidate 1 as a sufficient fix on its
own: a recycler with a ~512 KB threshold would not have been in the path. The
only condition present at all three failures is the arena at 187-191.7 of 192.

**The arena climbed in lockstep with the decoded-audio cache:**

| arena reaches | t | used | audio cache |
|---|---|---|---|
| 130 MB | 68 | 132.6 | 4.5 |
| 150 MB | 137 | 147.8 | 9.8 |
| 170 MB | 264 | 166.3 | 18.7 |
| 190 MB | 441 | 186.7 | **27.7** |

Peak `used` was 186.7 of 192 MB at t=444. Of the 60 MB of arena growth in that
window, ~23 MB is cache. Since newlib never returns arena, holding it is
permanent even after the bytes are freed.

This does not contradict the log141 reading that the cache is innocent -- at the
fatal allocation it held 7.1 MB, and purging it could not have helped. The
mechanism is different: it is not consuming the heap when you crash, it is
ratcheting the arena to the ceiling on the way there.

The cache was not earning that arena. `PCM_BUDGET_BYTES` was 40 MB, and the
new-handler wiped the cache **entirely, twice** in this run while the hit rate
climbed straight through both wipes: 86% -> 88% -> 89%, ending at 1777 hits /
211 misses with zero cache-full drops. Two total wipes cost nothing measurable.

### Change made

`PCM_BUDGET_BYTES` split into two numbers in `audio_patch.c`:

- `PCM_CACHE_KEEP` (8 MB) -- soft retention. `cache_insert` trims toward it on a
  best-effort basis and **never refuses because of it**.
- `PCM_BUDGET_BYTES` (40 MB, unchanged) -- the hard ceiling that still refuses.

The split is the whole point. Simply lowering the one constant would have armed
`g_pcm_bytes + need > PCM_BUDGET_BYTES` in the stream path, which substitutes
**timed silence** for a track; all 9 silence events in log142 tripped on
`STREAM_PCM_MAX` with "in use 0 KB", so that arm has never fired and must not
start firing now. Likewise the eviction loop no longer returns NULL when only
referenced entries remain, because that drops a sound the game is playing.

Two honest caveats for the next log:

- More eviction means more churn of multi-hundred-KB PCM buffers, which could
  itself fragment. `largest block` is the number that will say.
- The new-handler backstop now has ~8 MB to give instead of 25, so if the crash
  survives this change it may arrive with less warning than it did at t=455.

## log144 (2026-08-16): the cache split works, and the probe was never running

31 minutes of play, ending the same way at an area transition (the apartment
exit to Upper City, Taris) at t=1723: a 4,194,304-byte `operator new` fails, the
purge hands back 3.6 MB, the retry fails, and the handler stands down.
`PC = _kill_r+0x42` with `r2 = 6` -- SIGABRT from `terminate` after an uncaught
`bad_alloc`, the identical signature to log140. The crash handler parks the
faulting thread, so the last frame stays on screen: that is the "freeze".

**`PCM_CACHE_KEEP` did its job.** The cache held 7.6-8.1 MB for the whole run
against 27.7 MB in log142, with 7334 hits / 346 misses (95%), zero decode
failures and zero cache-full drops. The soft/hard split is worth keeping.

**It did not stop the arena ratchet.** The arena still climbed to 191.6 of
192 MB by t=1540 and was at 190.9 when the allocation failed, so the cache was
never the driver -- it was a passenger. Whatever holds the arena open is
something else.

**The teardown is what shreds the heap.** Free blocks sat between 800 and 1900
for the whole session and then jumped to **10741 at t=1721**, two seconds before
the failing request. The old area's unload is what fragments; the new area's
first big allocation is what dies. `used` at that moment was 92.6 MB -- there
was no shortage of bytes, only of adjacent ones.

**The large-allocation census is real and says candidate 1 is not enough.**
Over the run: 971 requests ≥256 KB, 118 ≥512 KB, 234 ≥1 MB, 17 ≥2 MB, 4 ≥4 MB.
A recycler above 512 KB would have covered 373 of them and missed the other 971,
which matches log142's 286 KB failure: the threshold has to be low, and a low
threshold means a recycler that intercepts well over a thousand allocations.

### The measurement never ran

`largest block the heap can still serve: 32.0 MB` appears 67 times in log144 and
10 times in log143, always exactly 32.0 MB -- at startup with a 0.0 MB arena, and
again 0.5 ms after a 4 MB allocation had failed. Both cannot be true.

They are not. `heap_largest_block` was compiled out. GCC's allocation DCE (on at
`-O2`) removes a `malloc`/`free` pair whose result is otherwise unused and folds
the null test to taken, so the function reduced to `return 32u << 20`.
Disassembly of the shipped `heap_log_full` confirms it: `heap_log`, `log_printf`,
`snprintf`, and no `malloc` anywhere in the body.

So the number that was supposed to choose between "recycle large blocks" and
"raise the ceiling" has never been collected, and the two logs that were supposed
to deliver it are silent on it. Fixed in `heap.c` with an `asm` barrier that
makes the pointer opaque; the ladder is now also capped at free-in-arena, since
a probe larger than that can only be answered by `sbrk`, which measures headroom
rather than contiguity and grows the arena for real while doing it. `heap_log`
now also reports `keepcost`, the top chunk -- the one contiguity figure mallinfo
gives away for free.

Still open, and unchanged: which of the two candidate fixes to take.

## log145 (2026-08-16): measured at last, and it retires candidate 2

34 minutes, ending on the Upper City cantina load. The probe works now, and the
run produced the curve the whole investigation was missing.

| t | largest servable | free in arena | arena |
|---|---|---|---|
| 727-998 | **32 MB** | 34-68 | 155-169 |
| 1028-1329 | **16 MB** | 55-60 | 169-178 |
| 1359-1660 | **8 MB** | 37-42 | 178-185 |
| 1690-1811 | **4 MB** | 45-47 | 185-191 |
| 1841-1871 | **2 MB** | 46-49 | 191.3 |
| 1901-2022 | **1 MB** | 45-47 | 191.4 |
| 2052+ | **0.5 MB** | 47-50 | 191.2 |

**The largest block halves roughly every 150 seconds while the free total does
not move.** 46 MB free, 792 blocks, top chunk 3 KB, largest servable 512 KB.
That is fragmentation measured directly, with a leak and exhaustion both ruled
out by the same table.

Each area load takes a step out of it -- `tar_m02af` at t=1013 (32 -> 16 MB),
`tar_m02aa` at t=1268 (16 -> 8 MB) -- and ordinary play erodes the rest.

### Both of the session's symptoms are this one curve

- **t=1989, 2001, 2019 -- three dialogue lines played silent.** `OUT OF MEMORY:
  1334 KB / 1140 KB / 1287 KB for 1 ch @ 32000 Hz`, at a moment when the largest
  servable block was 1.0 MB. VO of 8-9 seconds needs 1.1-1.3 MB of PCM; the
  curve had just crossed under it.
- **t=2062 -- bad_alloc entering the cantina.** 1,326,383 bytes, then 2,356,153
  on the retry, with the largest block down to 512 KB.

The user reported these as two separate faults ("some voices were silent", "then
it black-screened"). They are the same number crossing two thresholds 70 seconds
apart.

### Candidate 2 (raise `MEMORY_NEWLIB_MB`) is retired

At t=1690 the largest servable block was already 4 MB **while the arena was
184.6 of 192** -- 7 MB of `sbrk` room still unspent. Contiguity decays whether or
not there is headroom, so more ceiling does not preserve it.

What more ceiling does buy is measurable. Live `used` sat at ~140 MB the whole
session while the arena climbed 137 -> 191 MB: **54 MB of arena consumed by
fragmentation loss in 34 minutes, ~1.6 MB/min.** So roughly 37 seconds of extra
play per extra MB of ceiling. A 32 MB raise, if the Vita even has it to give
next to vitaGL's pools, is about twenty more minutes -- a stopgap with a price
tag, not a fix.

### What is left

Keep large allocations out of the general heap. Two populations feed it:

- **The game's**, via dynlib's shims: 1047 requests ≥256 KB, 128 ≥512 KB, 267
  ≥1 MB, 12 ≥2 MB, 4 ≥4 MB over the run. The 286 KB failure in log142 sets the
  threshold at ~256 KB, not 512 KB.
- **Ours**, and it does not appear in that census at all: decoded-audio PCM is
  allocated with the loader's own `malloc`, not through a shim. 389 cache misses
  in this run, each a 100 KB - 1.3 MB buffer held for a while and then freed —
  the exact shape that leaves medium holes.

## The fix: `loader/bigalloc.c`

Everything at or above 256 KB is served from segments of our own —
`sceKernelAllocMemBlock`, up to 4 x 8 MB, taken on demand — instead of newlib's
arena. Both populations above go through it: the game's via dynlib's shims, ours
via `big_malloc` in `audio_mp3.c` and `audio_patch.c`.

**Identifying our own pointers is the whole risk, and owning whole segments
reduces it to a bounds test.** `free`, `operator delete` and `operator delete[]`
ask `bigalloc_owns(p)` first; nothing reads a header until the range has already
said the block is ours, so a pointer from any other allocator is never
dereferenced. When the pool cannot serve a request it returns NULL, the caller
falls back to `malloc`, and the same bounds test later routes that block back to
`free` correctly. Degrading to exactly today's behaviour is always available.

The allocator itself is a best-fit implicit list with boundary tags: 16-byte
headers, 16-aligned payloads, splitting above a 64 KB remainder, coalescing both
ways on free. The population is tiny by construction — 8 MB of >=256 KB blocks
is a few dozen — so the walk is cheap and best fit is affordable, which matters
because leaving the wrong-sized remainder behind is the exact failure this file
exists to prevent.

### Budgets moved, not added

`MEMORY_NEWLIB_MB` 192 -> 160 and `MEMORY_VITAGL_THRESHOLD_MB` 16 -> 48. Both
halves matter: vitaGL claims all free RAM at `vglInitExtended`, before the game
runs, so shrinking newlib without raising vitaGL's leave-alone threshold would
simply hand the 32 MB to vitaGL and the pool would find nothing to allocate.

Peak `used` in log145 was 145.9 MB, so 160 is only affordable if the pool
absorbs enough of it. That is the open question this build answers, and both
answers are one constant away:

- `[big] ... N fell back to malloc` climbing, pool live near 32/32 -> pool too
  small, move the line.
- newlib failing at a lower `used` than 145 MB -> the reduction was too deep,
  move it back.

### Verification

`loader/bigalloc.c` compiles on the host with `-DBIGALLOC_HOST_TEST`, which
swaps the segment source for `malloc`. 75 checks under ASan+UBSan: threshold,
foreign pointers, payload integrity and alignment across 16 live blocks,
coalescing (fill the pool, free three adjacent, ask for their sum), best fit
leaving a larger hole intact, exhaustion and fallback, realloc in both
directions and small-to-pool migration, and 4000 rounds of random churn checking
every live block's byte pattern. ASan caught one real bug on the first run: the
small-to-pool realloc path copied the *new* size out of the old block, fixed
with `malloc_usable_size`.
