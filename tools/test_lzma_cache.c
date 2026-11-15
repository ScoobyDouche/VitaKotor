/* Host test for loader/lzma_cache.c: keys, both pools, ring wrap. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../loader/lzma_cache.h"

static unsigned char src[4096], out[300000], got[300000];

static uint64_t put(int pool, unsigned seed, size_t n) {
  for (size_t i = 0; i < sizeof src; i++) src[i] = (unsigned char)(seed * 31 + i * 7);
  for (size_t i = 0; i < n; i++) out[i] = (unsigned char)(seed + i);
  uint64_t k = lzma_cache_key(src, sizeof src, (const unsigned char *)"props", 5);
  lzma_cache_put(pool, k, n, sizeof src, out, n, sizeof src - 3);
  return k;
}
static int get(int pool, uint64_t k, unsigned seed, size_t n) {
  size_t dl = n, sl = sizeof src;
  if (!lzma_cache_get(pool, k, got, &dl, &sl)) return 0;
  assert(dl == n && sl == sizeof src - 3);
  for (size_t i = 0; i < n; i++) assert(got[i] == (unsigned char)(seed + i));
  return 1;
}

int main(void) {
  assert(lzma_cache_init(LZMA_POOL_GENERAL, 1 << 20));
  assert(lzma_cache_init(LZMA_POOL_SOUND, 1 << 20));
  assert(lzma_cache_key(src, 16, (const unsigned char *)"a", 1) !=
         lzma_cache_key(src, 16, (const unsigned char *)"b", 1));

  uint64_t snd = put(LZMA_POOL_SOUND, 1, 200000);
  assert(get(LZMA_POOL_SOUND, snd, 1, 200000));
  assert(!get(LZMA_POOL_GENERAL, snd, 1, 200000));       /* pools are separate */
  assert(lzma_cache_has(LZMA_POOL_SOUND, snd, 200000, sizeof src));
  assert(!lzma_cache_has(LZMA_POOL_SOUND, snd, 200001, sizeof src));

  /* A burst through the general ring forgets its own old entries... */
  uint64_t first = put(LZMA_POOL_GENERAL, 2, 250000);
  for (unsigned s = 3; s < 20; s++) put(LZMA_POOL_GENERAL, s, 250000);
  assert(!get(LZMA_POOL_GENERAL, first, 2, 250000));
  /* ...the newest survives, and the sound is untouched. */
  uint64_t last = put(LZMA_POOL_GENERAL, 20, 250000);
  assert(get(LZMA_POOL_GENERAL, last, 20, 250000));
  assert(get(LZMA_POOL_SOUND, snd, 1, 200000));
  assert(!get(LZMA_POOL_SOUND, 0, 1, 200000));
  puts("ok");
  return 0;
}
