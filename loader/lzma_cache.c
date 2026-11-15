/* lzma_cache.c -- see lzma_cache.h
 *
 * Storage is a single ring. Entries are appended at the head and wrap; anything
 * the head runs over is forgotten. That makes it FIFO rather than LRU, so a hit
 * on an entry in the older half of the ring is re-appended to keep the sounds
 * the game replays from aging out behind a burst of one-off loads.
 *
 * Why a ring in one block rather than malloc per entry: these are 10 KB-1 MB
 * buffers with long and unpredictable lifetimes, which is exactly the mix that
 * shredded newlib's arena (log199). One allocation at boot cannot fragment
 * anything.
 *
 * The key is a 64-bit hash of every compressed byte plus both lengths. A hit
 * hands the game bytes it did not decompress, so the hash covers the whole
 * input, never a sample of it.
 */

#include <stdlib.h>
#include <string.h>

#include "lzma_cache.h"

#ifdef LZMA_CACHE_HOST_TEST
#include <stdio.h>
#define log_printf(...) (printf(__VA_ARGS__), putchar('\n'))
static void lock(void) {}
static void unlock(void) {}
static void mutex_init(void) {}
#else
#include <vitasdk.h>
#include "log.h"
static SceUID s_mutex = -1;
static void mutex_init(void) { s_mutex = sceKernelCreateMutex("lzma_cache", 0, 0, NULL); }
static void lock(void)   { if (s_mutex >= 0) sceKernelLockMutex(s_mutex, 1, NULL); }
static void unlock(void) { if (s_mutex >= 0) sceKernelUnlockMutex(s_mutex, 1); }
#endif

#define MAX_ENTRIES 512
/* Bigger outputs are area-load textures and models, decompressed once per load
 * (2 MB, not 1, so a long ambient such as the ship-attack rumbles still fits):
 * caching them would only flush the small, often-replayed ones. */
#define MAX_ENTRY_BYTES (2u * 1024u * 1024u)
#define MIN_ENTRY_BYTES 1024u

typedef struct {
  uint64_t key;
  uint32_t dst_in, src_in;      /* lengths the caller passed in */
  uint32_t dst_out, src_out;    /* what LzmaUncompress reported back */
  uint32_t off;                 /* in the ring */
  uint32_t seq;                 /* append order, for "older half" */
  int      used;
} Entry;

typedef struct {
  unsigned char *ring;
  uint32_t cap, head, seq;
  Entry    e[MAX_ENTRIES];
  unsigned calls, hits, stores, refreshed, last_calls;
  uint64_t saved_bytes;
} Pool;
static Pool s_pool[LZMA_POOLS];
static uint64_t s_miss_us;
static const char *const s_pool_name[LZMA_POOLS] = { "cache", "sound cache" };

static uint64_t hash_bytes(const unsigned char *p, size_t n) {
  /* FNV-1a, 64-bit, over 32-bit words then the tail: a few hundred us at most
   * for the sizes cached, against the tens of ms a decompression costs. */
  uint64_t h = 0xcbf29ce484222325ull;
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    uint32_t w;
    memcpy(&w, p + i, 4);
    h = (h ^ w) * 0x100000001b3ull;
  }
  for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
  return h ^ (uint64_t)n;
}

int lzma_cache_init(int pool, size_t bytes) {
  Pool *c = &s_pool[pool];
  if (c->ring) return 1;
  if (pool == 0) mutex_init();
  c->ring = (unsigned char *)malloc(bytes);
  if (!c->ring) {
    log_printf("[lzma] %s: could not take %u KB -- running without it",
               s_pool_name[pool], (unsigned)(bytes / 1024));
    return 0;
  }
  c->cap = (uint32_t)bytes;
  log_printf("[lzma] %s armed: %u KB ring, %u entries, %u B-%u KB per entry",
             s_pool_name[pool], (unsigned)(bytes / 1024), MAX_ENTRIES, MIN_ENTRY_BYTES,
             MAX_ENTRY_BYTES / 1024);
  return 1;
}

static Entry *find(Pool *c, uint64_t key, uint32_t dst_in, uint32_t src_in) {
  for (int i = 0; i < MAX_ENTRIES; i++) {
    Entry *e = &c->e[i];
    if (e->used && e->key == key && e->dst_in == dst_in && e->src_in == src_in) return e;
  }
  return NULL;
}

/* Reserve `n` ring bytes at the head, forgetting whatever they overlap.
 * Called with the lock held. */
static uint32_t reserve(Pool *c, uint32_t n) {
  if (c->head + n > c->cap) c->head = 0;
  uint32_t lo = c->head, hi = c->head + n;
  for (int i = 0; i < MAX_ENTRIES; i++) {
    Entry *e = &c->e[i];
    if (e->used && e->off < hi && e->off + e->dst_out > lo) e->used = 0;
  }
  c->head = hi;
  return lo;
}

static Entry *free_slot(Pool *c) {
  Entry *oldest = &c->e[0];
  for (int i = 0; i < MAX_ENTRIES; i++) {
    if (!c->e[i].used) return &c->e[i];
    if ((int32_t)(c->e[i].seq - oldest->seq) < 0) oldest = &c->e[i];
  }
  oldest->used = 0;
  return oldest;
}

uint64_t lzma_cache_key(const unsigned char *src, size_t srcLen,
                        const unsigned char *props, size_t propsSize) {
  if (!src || !props || propsSize > 16) return 0;
  uint64_t key = hash_bytes(src, srcLen) ^ (hash_bytes(props, propsSize) * 0x9e3779b97f4a7c15ull);
  return key ? key : 1;                         /* 0 means "no key" */
}

int lzma_cache_has(int pool, uint64_t key, size_t destLenIn, size_t srcLenIn) {
  Pool *c = &s_pool[pool];
  if (!c->ring || !key) return 0;
  lock();
  int r = find(c, key, (uint32_t)destLenIn, (uint32_t)srcLenIn) != NULL;
  unlock();
  return r;
}

int lzma_cache_get(int pool, uint64_t k, unsigned char *dest, size_t *destLen, size_t *srcLen) {
  Pool *c = &s_pool[pool];
  const uint64_t *key = &k;
  if (!c->ring || !k || !dest || !destLen || !srcLen) return 0;
  size_t dst_in = *destLen, src_in = *srcLen;
  if (dst_in < MIN_ENTRY_BYTES || dst_in > MAX_ENTRY_BYTES) return 0;

  lock();
  c->calls++;
  Entry *e = find(c, *key, (uint32_t)dst_in, (uint32_t)src_in);
  if (!e) { unlock(); return 0; }
  memcpy(dest, c->ring + e->off, e->dst_out);
  *destLen = e->dst_out;
  *srcLen  = e->src_out;
  c->hits++;
  c->saved_bytes += e->dst_out;

  /* Keep it from being the next thing the head runs over. */
  if (c->seq - e->seq > (uint32_t)(MAX_ENTRIES / 2) ||
      ((c->head >= e->off) ? c->head - e->off : c->cap - e->off + c->head) > c->cap / 2) {
    uint32_t n = e->dst_out;
    Entry copy = *e;
    e->used = 0;
    uint32_t off = reserve(c, n);
    memmove(c->ring + off, dest, n);         /* dest holds the bytes already */
    Entry *ne = free_slot(c);
    *ne = copy;
    ne->off = off;
    ne->seq = ++c->seq;
    ne->used = 1;
    c->refreshed++;
  }
  unlock();
  return 1;
}

void lzma_cache_put(int pool, uint64_t key, size_t destLenIn, size_t srcLenIn,
                    const unsigned char *dest, size_t destLenOut, size_t srcLenOut) {
  Pool *c = &s_pool[pool];
  if (!c->ring || !key || destLenOut == 0 || destLenOut > destLenIn ||
      destLenIn < MIN_ENTRY_BYTES || destLenIn > MAX_ENTRY_BYTES || destLenOut > c->cap)
    return;
  lock();
  if (!find(c, key, (uint32_t)destLenIn, (uint32_t)srcLenIn)) {
    uint32_t off = reserve(c, (uint32_t)destLenOut);
    memcpy(c->ring + off, dest, destLenOut);
    Entry *e = free_slot(c);
    e->key = key;
    e->dst_in = (uint32_t)destLenIn;  e->src_in = (uint32_t)srcLenIn;
    e->dst_out = (uint32_t)destLenOut; e->src_out = (uint32_t)srcLenOut;
    e->off = off;
    e->seq = ++c->seq;
    e->used = 1;
    c->stores++;
  }
  unlock();
}

void lzma_cache_note_miss_us(uint64_t us) {
  lock();
  s_miss_us += us;
  unlock();
}

void lzma_cache_report(void) {
  for (int p = 0; p < LZMA_POOLS; p++) {
    Pool *c = &s_pool[p];
    if (!c->ring || c->calls == c->last_calls) continue;
    c->last_calls = c->calls;
    unsigned live = 0;
    uint64_t bytes = 0;
    lock();
    for (int i = 0; i < MAX_ENTRIES; i++)
      if (c->e[i].used) { live++; bytes += c->e[i].dst_out; }
    unsigned calls = c->calls, hits = c->hits, stores = c->stores, refr = c->refreshed;
    uint64_t saved = c->saved_bytes, miss_us = s_miss_us;
    unlock();
    log_printf("[lzma] %s: %u lookups, %u hits (%u%%), %u stored, %u refreshed; "
               "%u entries / %u KB held; %u MB served from cache; %u ms spent decompressing misses",
               s_pool_name[p], calls, hits, calls ? hits * 100u / calls : 0u, stores, refr, live,
               (unsigned)(bytes >> 10), (unsigned)(saved >> 20), (unsigned)(miss_us / 1000));
  }
}
