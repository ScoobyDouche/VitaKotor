/* obb_cache.h -- keep recent archive reads in RAM.
 *
 * log_4: every hover on the equipment screen re-read the same ~1.3 MB of
 * textures from the card (15-18 reads, ~140 ms) for the character preview the
 * game had just thrown away, then did it again on the next hover. The OBBs are
 * read-only, so a read at the same offset and length always returns the same
 * bytes and can be served from memory.
 *
 * All calls are made with dynlib.c's io lock held; there is no locking here.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* One fixed ring, allocated once. 0 KB, or a failed allocation, leaves the
 * cache off and every lookup misses. */
int obb_cache_init(size_t kb);

/* Copy a cached read of archive `file` at `off` for `len` bytes into dst.
 * Returns 1 on a hit. */
int obb_cache_get(int file, long off, long len, void *dst);

/* Remember a completed read. Sizes outside the cached band are ignored. */
void obb_cache_put(int file, long off, long len, const void *src);

/* One line of stats if anything happened since the last call. */
void obb_cache_report(void);
