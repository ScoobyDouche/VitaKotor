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

/* Two pools. Area loads decompress hundreds of textures and models in a burst
 * (550 stores in six minutes on Nov 15) and ran the ring over every ambient
 * sound between two plays of it, so sounds get a ring of their own. */
#define LZMA_POOL_GENERAL 0
#define LZMA_POOL_SOUND   1
#define LZMA_POOLS        2

/* One fixed ring per pool, allocated once (at boot, while the heap is still
 * whole). Returns 0 if the block could not be had; lookups then always miss. */
int lzma_cache_init(int pool, size_t bytes);

/* The key for one decompression: a hash of every compressed byte and of the
 * props, which change the output as much as the data does. 0 = not cacheable. */
uint64_t lzma_cache_key(const unsigned char *src, size_t srcLen,
                        const unsigned char *props, size_t propsSize);

/* If this exact input was decompressed before into a destination of the same
 * size, copy the output into `dest` and fill the out-parameters as
 * LzmaUncompress would. Returns 1 on a hit, 0 otherwise. */
int lzma_cache_get(int pool, uint64_t key, unsigned char *dest, size_t *destLen, size_t *srcLen);

/* Whether the pool holds it, without copying or counting a lookup. */
int lzma_cache_has(int pool, uint64_t key, size_t destLenIn, size_t srcLenIn);

/* Store a successful result under `key`. */
void lzma_cache_put(int pool, uint64_t key, size_t destLenIn, size_t srcLenIn,
                    const unsigned char *dest, size_t destLenOut, size_t srcLenOut);

/* Account decompression time actually spent on a miss, for the report. */
void lzma_cache_note_miss_us(uint64_t us);

/* One line of stats per pool if anything happened since the last call. */
void lzma_cache_report(void);
