# Per-handle read buffering for shared .obb handles

**Status:** implemented, MEASURED, and REJECTED. Superseded by the replay cache
described under "Outcome" below. Kept because the negative result is the reason
the working fix looks the way it does.
**Date:** 2026-08-01

## Problem

Launching the game takes roughly two and a half minutes before the main menu
appears. Measured from log121:

| Phase | Window | Duration |
|---|---|---|
| Loader start, native libraries loaded | 0.58 - 2.4s | ~1.8s |
| `SDL_main` thread starts | 5.09s | |
| `mountObb("main.obb")` | 7.44 - 103.05s | **95.6s** |
| `mountPatchObb("patch.obb")` | 103.05 - 107.89s | 4.8s |
| `SDL_main` entered | 108.00s | |
| "Main Menu" reported | 142.72s | 34.7s |

Mounting `main.obb` is 96 seconds, about two thirds of the total. It is not
data transfer -- the archive's central directory is only a few megabytes. It is
syscall count.

## Root cause

`.obb` opens hand out virtual handles that share one real `FILE*` (added to fix
the 54-concurrent-handle exhaustion freeze; see the comment block at
`loader/dynlib.c:314`). Each virtual handle keeps its own position, and
`fread_diag` seeks the shared handle before reading whenever another reader has
moved it:

```c
if (sf->rpos != v->pos) {          /* dynlib.c:535 */
  g_vseeks++;
  if (fseek(sf->real, v->pos, SEEK_SET) == 0) sf->rpos = v->pos;
```

Two virtual handles are live during the mount, and they interleave, so that
branch is taken on essentially every read. **`fseek` discards newlib's stdio
read buffer**, which defeats the 64KB `setvbuf` applied at `dynlib.c:399`. Every
read becomes a fresh card round trip.

The instrumentation already in the code says so directly:

```
[34.44] shared reads=8192   seeks=8192 (100%)
[60.23] shared reads=16384  seeks=16384 (100%)
[85.18] shared reads=24576  seeks=24576 (100%)
```

Roughly 30,000 seek+read pairs at ~330/sec, about 3ms each. The existing comment
at `dynlib.c:547` anticipated this exact failure: *"Seeks near 0% means stdio
buffering survived; anywhere near 100% means readers are interleaving and we are
back to attempt #1's unbuffered crawl."*

`fgetc_diag` and `fgets_diag` both route through `fread_diag`
(`dynlib.c:594`, `dynlib.c:604`), so `fgets` on a virtual handle currently costs
one seek+read **per byte**.

## Approach

Give each virtual handle its own read-ahead buffer, so interleaving between
handles no longer destroys locality. Each handle refills in whole blocks and
serves sequential reads from RAM.

Alternatives considered:

- **Shared LRU block cache across handles.** Bounded memory regardless of handle
  count, and dedups overlapping reads. Rejected for now: it is a real cache with
  eviction and more ways to be subtly wrong, and handles mostly read disjoint
  regions, so the gain over per-handle buffers is small.
- **A real file descriptor per handle.** Fastest, and precisely what caused the
  54-handle exhaustion freeze that the sharing layer was written to fix.
  Rejected.

## Design

Confined to `loader/dynlib.c`: the `VFile` struct and `fread_diag`. The sharing
layer itself -- `fopen_shared`, `share_this`, `SharedFile`, the `no_obb_share`
escape hatch -- is unchanged. The buffer sits underneath the existing
seek-then-read as a pure accelerator; with it disabled or unallocated, behaviour
is byte-for-byte what ships today.

### State

```c
typedef struct {
  int used; SharedFile *sf; long pos; int eof, err;
  unsigned char *buf;   /* lazily allocated; NULL means unbuffered */
  long  bufstart;       /* absolute archive offset that buf[0] maps to */
  int   buflen;         /* valid bytes in buf; 0 means empty */
} VFile;
```

The buffer is keyed by **absolute archive offset**, not by relative position.
That is what keeps `fseeko_diag` unchanged: it continues to move `v->pos` only,
and the next read tests whether `v->pos` falls inside
`[bufstart, bufstart + buflen)`.

### Read path

For a request of `want` bytes in `fread_diag`:

1. **Buffer hit.** If `v->pos` lies within the buffered range, `memcpy` the
   overlap out, advance `v->pos`, and return. No lock, no syscall.
2. **Bulk bypass.** If the remaining request is at least one block, read it
   straight into the caller's buffer through the existing seek-then-read path.
   Large asset reads should not pay a copy through the buffer.
3. **Refill.** Otherwise take `io_lock`, seek the shared handle to `v->pos`,
   read one full block into `v->buf`, set `bufstart` and `buflen`, then serve
   from it.

Refills still update `sf->rpos` and still increment `g_vreads` / `g_vseeks`, so
the existing counters keep reporting real syscall traffic rather than buffer
hits. A short read at end of archive sets `buflen` to what was actually read;
`v->eof` is set when a request cannot be satisfied.

### Memory

- Block size 32KB, allocated lazily on the first read through a handle and freed
  in `fclose_diag`.
- `VF_MAX` is 160, so the unbounded ceiling would be 5MB. A global budget caps
  total buffer bytes at 2MB; handles that cannot get a buffer run unbuffered --
  slower, still correct.
- In practice only two handles are live during the mount. The 54-handle peak
  seen in log113 is gameplay audio streaming, not the mount.
- Both constants live in `loader/config.h` beside the existing tunables. A block
  size of 0 disables buffering entirely.

### Correctness

- Archives are opened read-only; `fwrite_diag` already refuses virtual handles,
  so there is no write-back path.
- `feof_diag` / `ferror_diag` keep reading `v->eof` / `v->err`, which the read
  path still sets.
- `fileno_diag` returns the shared descriptor and is unaffected.
- Buffer fills happen under the existing `io_lock`. Each buffer belongs to
  exactly one handle, so there is no cross-handle sharing to get wrong.
- `fgetc` and `fgets` need no changes; they inherit buffering through
  `fread_diag`.

## Verification

The existing log line is the instrument -- no new tooling required:

```
[io] shared reads=N seeks=M (X%), K virtual handles live
```

| | current | target |
|---|---|---|
| Seek ratio | 100% | low single digits |
| Seeks during mount | ~30,000 | hundreds |
| `mountObb` wall clock | 95.6s | to be measured |

Wall clock comes from the existing `>>> mountObb` / `<<< mountObb returned`
timestamps. Compare against log121 as the baseline.

No numeric speedup is promised here. The mechanism is well understood, but the
size of the win depends on the central directory being read roughly
sequentially. If miniz seeks randomly across the archive instead, read-ahead
helps much less -- and the seek ratio in the first test run reveals that
immediately, before anything is built on top of it.

## Out of scope

- The 34.7s between `SDL_main` and the main menu. That window is dense with
  per-call `[JNI]` logging and a 3-second `[wd]` watchdog poll; reducing log
  volume is a separate change with a separate measurement.
- Caching the parsed archive index to the memory card. Worth revisiting only if
  buffering alone leaves the mount unacceptably slow, since it adds a cache
  invalidation problem and does nothing for first launch.

## Rollback

- Set the block size to 0 in `loader/config.h` and rebuild.
- Or, without reinstalling, create `ux0:data/kotor/no_obb_share` on the card to
  bypass the sharing layer entirely.

---

## Outcome (2026-08-01, log122)

**The design above was built, measured, and rejected.** It made startup slower:
the mount went from 95.6s to 103.7s, and the seek ratio barely moved, 100% to
99%.

The estimate at the end of "Verification" was the thing that turned out to be
wrong. Read-ahead assumed the archive is read roughly sequentially. It is not.
Measured offline against the real `main.obb` (15,984 entries, all stored, 1.79 MB
contiguous central directory):

| Gap between consecutive local headers | |
|---|---|
| mean | 107 KB |
| median | 38 KB |

A 32 KB block sits just below the median, which is the worst possible place.
Simulating the real offsets:

| Block | Backing reads | Bytes moved |
|---|---|---|
| unbuffered | 15,984 | **1.0 MB** |
| 32 KB (shipped) | 11,383 | **373 MB** |
| 64 KB | 7,032 | 461 MB |
| 1 MB | 792 | 830 MB |

The mount needs ~46 bytes out of every ~107 KB. Prefetching is the wrong tool
for access that sparse: 30% fewer reads cost 373x the bytes. Even 1 MB blocks
only reach 20x fewer reads by moving 830 MB, which on a memory card costs more
than it saves. No block size wins.

### What replaced it

Two changes, both in the commit "Cut startup from ~142s to ~54s":

1. **`sceIoPread` instead of `fseek` + `fread`.** One syscall against an
   absolute offset instead of two plus a discarded newlib buffer. Cold mount
   95.6s -> 70.5s on its own.
2. **A replay cache** (`loader/obb_index.c`). The mount reads the same bytes
   every boot, so record them once into `main.obb.idx` (906 KB, 30,658 ranges)
   and serve them from memory afterwards. Mount 70.5s -> **2.0s**, logged as
   "4 card reads, 31788 served from cache".

Startup to first frame: ~142s -> ~54s.

### What to take from this

The instrumentation was right and the conclusion drawn from it was wrong. A
100% seek ratio was read as "buffering is being defeated, so add buffering",
when it actually meant "these reads are scattered". The distinguishing
measurement -- the distribution of gaps between reads -- was available offline
from the archive itself the whole time, and would have killed the design in
minutes without a hardware round trip. Characterise the access pattern before
choosing a caching strategy, not after.
