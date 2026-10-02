/* lzma_cache.h -- remember what LzmaUncompress produced, keyed by its input.
 *
 * log202: a sound the game replays (pl_doorstuck, the Endar Spire door sparks,
 * several times a second) costs ~36 ms of frozen frame EVERY play, and 29 of
 * those are LZMA decompressing the same 47 KB it decompressed half a second
 * earlier -- the card read is 7. The engine's resource cache does not keep it.
 * Decompression is a pure function of its input, so the output can be reused.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* One fixed ring, allocated once (at boot, while the heap is still whole).
 * Returns 0 if the block could not be had; lookups then always miss. */
int lzma_cache_init(size_t bytes);

/* If this exact input was decompressed before into a destination of the same
 * size, copy the output into `dest` and fill the out-parameters as
 * LzmaUncompress would. The props bytes are part of the key: they change the
 * output as much as the data does. Returns 1 on a hit, 0 otherwise. `*key` is set either
 * way, for lzma_cache_put. */
int lzma_cache_get(uint64_t *key, unsigned char *dest, size_t *destLen,
                   const unsigned char *src, size_t *srcLen,
                   const unsigned char *props, size_t propsSize);

/* Store a successful result under `key` (from the lzma_cache_get miss). */
void lzma_cache_put(uint64_t key, size_t destLenIn, size_t srcLenIn,
                    const unsigned char *dest, size_t destLenOut, size_t srcLenOut);

/* Account decompression time actually spent on a miss, for the report. */
void lzma_cache_note_miss_us(uint64_t us);

/* One line of stats if anything happened since the last call. */
void lzma_cache_report(void);
