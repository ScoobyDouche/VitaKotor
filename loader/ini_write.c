/* ini_write.c -- skip settings writes that change nothing.
 *
 * log245: entering the equip tab calls CExoIniInternal::WriteIniEntry 54
 * times, and every call rewrites the whole of swkotor.ini on the card (one
 * read of the file the first time, then an SDL_RWFromFile "w" and an fputs
 * per line each call) -- ~430 ms of the ~1.1 s freeze, the rest the card
 * flushing. The values are the same every time.
 *
 * Remember each (4 strings) call that wrote successfully, and answer a repeat
 * of one with the 1 a successful write returns, without touching the card.
 * The argument order is not assumed: when a call really writes, every
 * remembered call that matches it in three of the four strings is forgotten
 * first, since whichever argument is the value, that is the same setting with
 * another value. So a setting changed and changed back is written both times.
 */
#include <vitasdk.h>
#include <stdint.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "ini_write.h"

typedef int (*write_fn)(void *self, void *a, void *b, void *c, void *d);
static write_fn g_orig;

#define MEMO_N 512
static uint64_t g_memo[MEMO_N][4];
static int g_memo_used[MEMO_N];
static SceUID g_mtx = -1;
static unsigned g_calls, g_skipped, g_written;

/* CExoString keeps its char* at offset 0; NULL is the empty string. */
static uint64_t str_hash(void *exo) {
  const char *s = exo ? *(const char **)exo : NULL;
  uint64_t h = 1469598103934665603ull;
  if (s)
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ull;
  return h;
}

static const char *str_of(void *exo) {
  const char *s = exo ? *(const char **)exo : NULL;
  return s ? s : "";
}

static int WriteIniEntry_hook(void *self, void *a, void *b, void *c, void *d) {
  uint64_t t[4] = { str_hash(a), str_hash(b), str_hash(c), str_hash(d) };
  if (g_mtx >= 0) sceKernelLockMutex(g_mtx, 1, NULL);
  g_calls++;
  int hit = -1;
  for (int i = 0; i < MEMO_N && hit < 0; i++)
    if (g_memo_used[i] && !memcmp(g_memo[i], t, sizeof t)) hit = i;
  if (hit >= 0) {
    g_skipped++;
    if (g_mtx >= 0) sceKernelUnlockMutex(g_mtx, 1);
    if (g_skipped <= 8 || (g_skipped & 255) == 0)
      log_printf("[ini] write skipped, unchanged: \"%.24s\" \"%.24s\" \"%.24s\" \"%.24s\" "
                 "(%u skipped of %u)", str_of(a), str_of(b), str_of(c), str_of(d),
                 g_skipped, g_calls);
    return 1;
  }
  if (g_mtx >= 0) sceKernelUnlockMutex(g_mtx, 1);

  int r = g_orig(self, a, b, c, d);
  g_written++;
  if (g_written <= 120)
    log_printf("[ini] write #%u: \"%.24s\" \"%.24s\" \"%.24s\" \"%.24s\" -> %d",
               g_written, str_of(a), str_of(b), str_of(c), str_of(d), r);

  if (g_mtx >= 0) sceKernelLockMutex(g_mtx, 1, NULL);
  int free_slot = -1;
  for (int i = 0; i < MEMO_N; i++) {
    if (!g_memo_used[i]) { if (free_slot < 0) free_slot = i; continue; }
    int same = 0;
    for (int k = 0; k < 4; k++) same += g_memo[i][k] == t[k];
    if (same >= 3) {                     /* this setting, any older value */
      g_memo_used[i] = 0;
      if (free_slot < 0) free_slot = i;
    }
  }
  if (r == 1 && free_slot >= 0) {
    memcpy(g_memo[free_slot], t, sizeof t);
    g_memo_used[free_slot] = 1;
  }
  if (g_mtx >= 0) sceKernelUnlockMutex(g_mtx, 1);
  return r;
}

void ini_write_install(void) {
#if INI_SKIP_UNCHANGED_WRITES
  uintptr_t f = so_symbol(&kotor_mod, "_ZN15CExoIniInternal13WriteIniEntryERK10CExoStringS2_S2_S2_");
  if (!f) { log_printf("[ini] WriteIniEntry not found -- writes not filtered"); return; }
  g_mtx = sceKernelCreateMutex("kotor_iniwrite", 0, 0, NULL);
  /* Prologue checked against the disassembly: push / add r7 / stmdb, no pc. */
  g_orig = (write_fn)build_thumb_trampoline(f, thumb_patch_len(f));
  if (!g_orig) { log_printf("[ini] WriteIniEntry trampoline FAILED"); return; }
  hook_thumb(f, (uintptr_t)WriteIniEntry_hook);
  log_printf("[ini] unchanged settings writes are skipped");
#endif
}
