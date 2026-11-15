/* snd_prefetch.c -- see snd_prefetch.h
 *
 * Learning: fread on an OBB handle notes (archive, offset, buffer, length).
 * When LzmaUncompress then misses the cache on the game thread and its output
 * is audio, the compressed input is looked up among those notes; if it lies
 * inside one, its archive offset is known and a record is kept under the
 * current module's name.
 *
 * Replaying: when a module's .rim is opened (the start of every area load) the
 * worker wakes, reads each of that module's records straight from the archive
 * and decompresses it into the sound pool. By the time the area is playable
 * the game's own request is a cache hit: a memcpy instead of 80-360 ms.
 *
 * A stale record (other APK, patched archive) is harmless: the cache is keyed
 * by a hash of the compressed bytes, so data the game never asks for is never
 * handed to it. It only costs the worker a wasted decompression.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <vitasdk.h>

#include "config.h"
#include "log.h"
#include "lzma_cache.h"
#include "snd_prefetch.h"
#include "threads.h"

#if SND_PREFETCH && LZMA_SOUND_CACHE_KB

#define SPF_PATH    DATA_PATH "/sndprefetch.dat"
#define SPF_MAGIC   0x32465053u   /* "SPF2": records carry the read range */
#define SPF_MAX     4096
#define SPF_MAX_SRC (2u * 1024u * 1024u)
#define SPF_MAX_DST (2u * 1024u * 1024u)
/* Stop a module's replay short of the ring's size, or the last sounds in
 * would run over the first. */
#define SPF_BUDGET  ((size_t)LZMA_SOUND_CACHE_KB * 1024u * 3u / 4u)

typedef struct {
  char     mod[16];
  uint8_t  which;      /* 0 main.obb, 1 patch.obb */
  uint8_t  props[5];
  uint16_t pad;
  uint32_t off, src_len, dst_len;
  uint32_t read_off, read_len;   /* the game's own fread that held the input */
} Rec;

/* The compressed bytes of the current module's sounds, as the game will read
 * them. A cache hit still cost 20-30 ms (log perf-c) because the game re-reads
 * the 150-600 KB file from the card before LzmaUncompress is even called;
 * answering that read from here leaves a memcpy. One block, taken at boot. */
#define SPF_STORE_BYTES (4u * 1024u * 1024u)
#define SPF_STORE_N     256
typedef struct { uint8_t which; uint32_t off, len, at; } Held;
static unsigned char *g_store;
static uint32_t g_store_used;
static Held     g_held[SPF_STORE_N];
static unsigned g_nheld, g_served;

typedef struct { int which; long off, len; const unsigned char *buf; } ReadNote;

static Rec     *g_rec;
static unsigned g_nrec;
static int      g_dirty;
static char     g_mod[16];
static volatile unsigned g_gen;      /* bumps on every module open */
static ReadNote g_note[4];
static unsigned g_note_i;
static SceUID   g_mtx = -1, g_wake = -1;
static snd_prefetch_decode_fn g_decode;
static unsigned g_learned, g_unlocated;

static void lock(void)   { if (g_mtx >= 0) sceKernelLockMutex(g_mtx, 1, NULL); }
static void unlock(void) { if (g_mtx >= 0) sceKernelUnlockMutex(g_mtx, 1); }

void snd_prefetch_note_read(int which, long off, const void *buf, long len) {
  if (!g_rec || len < 1024) return;
  lock();
  ReadNote *n = &g_note[g_note_i++ & 3];
  n->which = which; n->off = off; n->len = len; n->buf = (const unsigned char *)buf;
  unlock();
}

int snd_prefetch_serve(int which, long off, void *buf, long len) {
  if (!g_nheld) return 0;
  int hit = 0;
  lock();
  for (unsigned i = 0; i < g_nheld; i++) {
    const Held *h = &g_held[i];
    if (h->off == (uint32_t)off && h->len == (uint32_t)len && h->which == which) {
      memcpy(buf, g_store + h->at, (size_t)len);
      g_served++;
      hit = 1;
      break;
    }
  }
  unlock();
  return hit;
}

void snd_prefetch_learn(const unsigned char *src, size_t src_len, size_t dst_len,
                        const unsigned char *props, size_t props_size) {
  if (!g_rec || props_size != 5 || !src_len || src_len > SPF_MAX_SRC || dst_len > SPF_MAX_DST)
    return;
  lock();
  if (!g_mod[0]) { unlock(); return; }
  const ReadNote *hit = NULL;
  for (int i = 0; i < 4; i++) {
    const ReadNote *n = &g_note[i];
    if (n->buf && src >= n->buf && src + src_len <= n->buf + n->len) { hit = n; break; }
  }
  if (!hit) {
    if (g_unlocated++ < 8)
      log_printf("[sndpf] cannot place a %u KB sound in the archive (not inside a recent read)",
                 (unsigned)(src_len >> 10));
    unlock();
    return;
  }
  uint32_t off = (uint32_t)(hit->off + (src - hit->buf));
  for (unsigned i = 0; i < g_nrec; i++) {
    const Rec *r = &g_rec[i];
    if (r->off == off && r->which == hit->which && r->src_len == src_len &&
        !strcmp(r->mod, g_mod)) { unlock(); return; }
  }
  if (g_nrec < SPF_MAX) {
    Rec *r = &g_rec[g_nrec++];
    memset(r, 0, sizeof *r);
    strcpy(r->mod, g_mod);
    r->which = (uint8_t)hit->which;
    memcpy(r->props, props, 5);
    r->off = off; r->src_len = (uint32_t)src_len; r->dst_len = (uint32_t)dst_len;
    r->read_off = (uint32_t)hit->off; r->read_len = (uint32_t)hit->len;
    if (r->read_len > SPF_MAX_SRC) {      /* part of a huge read: fetch just the input */
      r->read_off = off; r->read_len = (uint32_t)src_len;
    }
    g_dirty = 1;
    g_learned++;
  }
  unlock();
}

void snd_prefetch_module(const char *path) {
  if (!g_rec || !path) return;
  const char *b = strrchr(path, '/');
  b = b ? b + 1 : path;
  char name[16];
  size_t n = 0;
  while (b[n] && b[n] != '.' && n < sizeof name - 1) { name[n] = (char)tolower((unsigned char)b[n]); n++; }
  name[n] = 0;
  if (n > 2 && !strcmp(name + n - 2, "_s")) name[n - 2] = 0;   /* tar_m04aa_s.rim */
  if (!name[0]) return;
  lock();
  strcpy(g_mod, name);
  g_gen++;
  unlock();
  sceKernelSignalSema(g_wake, 1);
}

static void save_file(void) {
  lock();
  unsigned n = g_nrec;
  size_t bytes = 8 + (size_t)n * sizeof(Rec);
  unsigned char *out = g_dirty ? (unsigned char *)malloc(bytes) : NULL;
  if (out) {
    uint32_t hdr[2] = { SPF_MAGIC, n };
    memcpy(out, hdr, 8);
    memcpy(out + 8, g_rec, (size_t)n * sizeof(Rec));
    g_dirty = 0;
  }
  unlock();
  if (!out) return;
  SceUID fd = sceIoOpen(SPF_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd >= 0) { sceIoWrite(fd, out, (SceSize)bytes); sceIoClose(fd); }
  free(out);
}

static void load_file(void) {
  SceUID fd = sceIoOpen(SPF_PATH, SCE_O_RDONLY, 0);
  if (fd < 0) return;
  uint32_t hdr[2] = { 0, 0 };
  if (sceIoRead(fd, hdr, 8) == 8 && hdr[0] == SPF_MAGIC && hdr[1] <= SPF_MAX) {
    int want = (int)(hdr[1] * sizeof(Rec));
    if (sceIoRead(fd, g_rec, (SceSize)want) == want) g_nrec = hdr[1];
  }
  sceIoClose(fd);
  for (unsigned i = 0; i < g_nrec; i++) g_rec[i].mod[sizeof g_rec[i].mod - 1] = 0;
}

static void replay(void) {
  static Rec todo[256];
  unsigned n = 0, gen;
  uint32_t max_read = 0, max_dst = 0;
  char mod[16];
  lock();
  gen = g_gen;
  strcpy(mod, g_mod);
  for (unsigned i = 0; i < g_nrec && n < 256; i++)
    if (!strcmp(g_rec[i].mod, mod)) {
      todo[n++] = g_rec[i];
      if (g_rec[i].read_len > max_read) max_read = g_rec[i].read_len;
      if (g_rec[i].dst_len > max_dst) max_dst = g_rec[i].dst_len;
    }
  unlock();
  if (!n || !g_decode) return;

  lock();                       /* the last module's reads are no use now */
  g_nheld = 0;
  g_store_used = 0;
  unlock();

  unsigned char *src = (unsigned char *)malloc(max_read);
  unsigned char *dst = (unsigned char *)malloc(max_dst);
  SceUID fd[2] = { -1, -1 };
  unsigned done = 0, had = 0, failed = 0, kept = 0;
  size_t total = 0;
  uint64_t t0 = sceKernelGetProcessTimeWide();
  for (unsigned i = 0; src && dst && i < n && g_gen == gen && total < SPF_BUDGET; i++) {
    const Rec *r = &todo[i];
    int w = r->which ? 1 : 0;
    uint32_t in = r->off - r->read_off;          /* input's place inside the read */
    if (r->off < r->read_off || in + r->src_len > r->read_len) { failed++; continue; }
    if (fd[w] < 0) fd[w] = sceIoOpen(w ? OBB_PATCH_PATH : OBB_MAIN_PATH, SCE_O_RDONLY, 0);
    if (fd[w] < 0 ||
        sceIoPread(fd[w], src, r->read_len, (SceOff)r->read_off) != (int)r->read_len) { failed++; continue; }
    lock();
    if (g_gen == gen && g_store && g_nheld < SPF_STORE_N &&
        g_store_used + r->read_len <= SPF_STORE_BYTES) {
      Held *h = &g_held[g_nheld];
      h->which = (uint8_t)w; h->off = r->read_off; h->len = r->read_len; h->at = g_store_used;
      memcpy(g_store + g_store_used, src, r->read_len);
      g_store_used += r->read_len;
      g_nheld++;
      kept++;
    }
    unlock();
    uint64_t key = lzma_cache_key(src + in, r->src_len, r->props, 5);
    if (lzma_cache_has(LZMA_POOL_SOUND, key, r->dst_len, r->src_len)) {
      had++; total += r->dst_len; continue;
    }
    size_t dl = r->dst_len, sl = r->src_len;
    if (g_decode(dst, &dl, src + in, &sl, r->props, 5) != 0 || !dl) { failed++; continue; }
    lzma_cache_put(LZMA_POOL_SOUND, key, r->dst_len, r->src_len, dst, dl, sl);
    total += dl;
    done++;
    sceKernelDelayThread(1000);           /* let the load's own card reads through */
  }
  if (fd[0] >= 0) sceIoClose(fd[0]);
  if (fd[1] >= 0) sceIoClose(fd[1]);
  free(src);
  free(dst);
  log_printf("[sndpf] %s: %u sounds known, %u decompressed ahead (%u KB), %u already held, "
             "%u failed, %u reads kept in RAM (%u KB), %u ms on the worker%s",
             mod, n, done, (unsigned)(total >> 10), had, failed, kept, (unsigned)(g_store_used >> 10),
             (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000),
             g_gen != gen ? " (cut short: another module opened)" : "");
}

static int worker(SceSize args, void *argp) {
  (void)args; (void)argp;
  for (;;) {
    sceKernelWaitSema(g_wake, 1, NULL);
    save_file();        /* what the previous module taught us */
    replay();
  }
  return 0;
}

void snd_prefetch_init(snd_prefetch_decode_fn decode) {
  if (g_rec) return;
  g_decode = decode;
  g_rec = (Rec *)malloc(SPF_MAX * sizeof(Rec));
  g_store = (unsigned char *)malloc(SPF_STORE_BYTES);   /* optional: NULL = reads stay on the card */
  g_mtx = sceKernelCreateMutex("kotor_sndpf", 0, 0, NULL);
  g_wake = sceKernelCreateSema("kotor_sndpf_wake", 0, 0, 16, NULL);
  if (!g_rec || g_mtx < 0 || g_wake < 0) { free(g_rec); g_rec = NULL; return; }
  load_file();
  /* 0x10000110 = 176: below the game, render and audio threads. */
  SceUID th = sceKernelCreateThread("kotor_sndpf", worker, 0x10000110, 0x8000, 0,
                                    thread_mask(CPU_AUX_A), NULL);
  if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
    log_printf("[sndpf] worker did not start (0x%x) -- sounds decompress on demand", (unsigned)th);
    free(g_rec); g_rec = NULL;
    return;
  }
  thread_census_add(th, "snd-prefetch");
  log_printf("[sndpf] armed: %u sounds known from earlier sessions", g_nrec);
}

void snd_prefetch_report(void) {
  static unsigned last, last_served;
  if (!g_rec || (g_learned == last && g_served == last_served)) return;
  last = g_learned; last_served = g_served;
  log_printf("[sndpf] learned %u new sounds this session (%u could not be placed), %u known; "
             "%u sound reads answered from RAM",
             g_learned, g_unlocated, g_nrec, g_served);
}

#else

void snd_prefetch_init(snd_prefetch_decode_fn decode) { (void)decode; }
void snd_prefetch_note_read(int which, long off, const void *buf, long len) {
  (void)which; (void)off; (void)buf; (void)len;
}
int snd_prefetch_serve(int which, long off, void *buf, long len) {
  (void)which; (void)off; (void)buf; (void)len; return 0;
}
void snd_prefetch_learn(const unsigned char *src, size_t src_len, size_t dst_len,
                        const unsigned char *props, size_t props_size) {
  (void)src; (void)src_len; (void)dst_len; (void)props; (void)props_size;
}
void snd_prefetch_module(const char *path) { (void)path; }
void snd_prefetch_report(void) {}

#endif
