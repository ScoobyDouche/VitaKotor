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
