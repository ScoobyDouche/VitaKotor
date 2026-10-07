/* obb_cache.c -- see obb_cache.h
 *
 * The same ring as lzma_cache.c: one block, entries appended at the head and
 * forgotten when the head runs over them, and a hit in the older half copied
 * forward so what the game keeps re-reading does not age out behind a burst of
 * one-off loads. One allocation cannot fragment the heap.
 *
 * Only reads of 4 KB-1 MB are kept. Smaller ones are the zip's local headers,
 * cheap and mostly served by obb_index already; bigger ones are area-load
 * bulk that would only flush the ring.
 */

#include <stdlib.h>
#include <string.h>

#include "obb_cache.h"

#ifdef OBB_CACHE_HOST_TEST
#include <stdio.h>
#define log_printf(...) (printf(__VA_ARGS__), putchar('\n'))
#else
#include "log.h"
#endif

#define MAX_ENTRIES 1024
#define MIN_BYTES   4096u
#define MAX_BYTES   (1024u * 1024u)

typedef struct {
  long     off;
  uint32_t len;
  uint32_t ring;     /* offset in the ring */
  uint32_t seq;      /* append order */
  int      file;
  int      used;
} Entry;

static unsigned char *s_ring;
static uint32_t s_cap, s_head, s_seq;
static Entry    s_e[MAX_ENTRIES];
static unsigned s_lookups, s_hits, s_stores, s_refreshed;
static uint64_t s_saved;

int obb_cache_init(size_t kb) {
  if (s_ring || !kb) {
    if (!kb) log_printf("[obbcache] off (ObbCacheKB=0)");
    return s_ring != NULL;
  }
  s_ring = (unsigned char *)malloc(kb * 1024u);
  if (!s_ring) {
    log_printf("[obbcache] could not take %u KB -- running uncached", (unsigned)kb);
    return 0;
  }
  s_cap = (uint32_t)(kb * 1024u);
  log_printf("[obbcache] armed: %u KB ring, %u entries, %u KB-%u KB per read",
             (unsigned)kb, MAX_ENTRIES, MIN_BYTES / 1024, MAX_BYTES / 1024);
  return 1;
}

static int in_band(long len) {
  return len >= (long)MIN_BYTES && len <= (long)MAX_BYTES;
}

static Entry *find(int file, long off, uint32_t len) {
  for (int i = 0; i < MAX_ENTRIES; i++) {
    Entry *e = &s_e[i];
    if (e->used && e->off == off && e->len == len && e->file == file) return e;
  }
  return NULL;
}

/* Reserve n ring bytes at the head, forgetting whatever they overlap. */
static uint32_t reserve(uint32_t n) {
  if (s_head + n > s_cap) s_head = 0;
  uint32_t lo = s_head, hi = s_head + n;
  for (int i = 0; i < MAX_ENTRIES; i++) {
    Entry *e = &s_e[i];
    if (e->used && e->ring < hi && e->ring + e->len > lo) e->used = 0;
  }
  s_head = hi;
  return lo;
}

static Entry *free_slot(void) {
  Entry *oldest = &s_e[0];
  for (int i = 0; i < MAX_ENTRIES; i++) {
    if (!s_e[i].used) return &s_e[i];
    if ((int32_t)(s_e[i].seq - oldest->seq) < 0) oldest = &s_e[i];
  }
  oldest->used = 0;
  return oldest;
}

static void insert(int file, long off, uint32_t len, const void *src) {
  uint32_t at = reserve(len);
  memmove(s_ring + at, src, len);       /* src may already be in the ring */
  Entry *e = free_slot();
  e->file = file; e->off = off; e->len = len;
  e->ring = at;
  e->seq = ++s_seq;
  e->used = 1;
}

int obb_cache_get(int file, long off, long len, void *dst) {
  if (!s_ring || !dst || !in_band(len)) return 0;
  s_lookups++;
  Entry *e = find(file, off, (uint32_t)len);
  if (!e) return 0;
  memcpy(dst, s_ring + e->ring, (size_t)len);
  s_hits++;
  s_saved += (uint64_t)len;

  uint32_t age = (s_head >= e->ring) ? s_head - e->ring : s_cap - e->ring + s_head;
  if (s_seq - e->seq > MAX_ENTRIES / 2 || age > s_cap / 2) {
    e->used = 0;                        /* re-append from dst, which holds it */
    insert(file, off, (uint32_t)len, dst);
    s_refreshed++;
  }
  return 1;
}

void obb_cache_put(int file, long off, long len, const void *src) {
  if (!s_ring || !src || !in_band(len) || (uint32_t)len > s_cap) return;
  if (find(file, off, (uint32_t)len)) return;
  insert(file, off, (uint32_t)len, src);
  s_stores++;
}

void obb_cache_report(void) {
  static unsigned last;
  if (!s_ring || s_lookups == last) return;
  last = s_lookups;
  unsigned live = 0;
  uint64_t bytes = 0;
  for (int i = 0; i < MAX_ENTRIES; i++)
    if (s_e[i].used) { live++; bytes += s_e[i].len; }
  log_printf("[obbcache] %u lookups, %u hits (%u%%), %u stored, %u refreshed; "
             "%u entries / %u KB held; %u MB served from RAM",
             s_lookups, s_hits, s_lookups ? s_hits * 100u / s_lookups : 0u,
             s_stores, s_refreshed, live, (unsigned)(bytes >> 10),
             (unsigned)(s_saved >> 20));
}
