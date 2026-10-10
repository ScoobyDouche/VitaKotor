/* fs_patch.c -- filesystem redirection + Android Asset Manager for KOTOR
 *
 * Writable paths originate from
 * SDL_AndroidGetExternalStoragePath, game data streams through SDL_RWFromFile
 * (resolved by Vita-native SDL2 in the entry-point phase), and a small set of
 * APK assets (shaders + a font) come through the Android Asset Manager NDK API
 * (AAssetManager_open / AAsset_*), which neither .so provides -- so we do.
 *
 * Everything here funnels paths to ux0:data/kotor/ and logs each request,
 * including misses, so the exact file the game wants is always visible.
 *
 *   ux0:data/kotor/            writable root (saves, swkotor.ini, logs)
 *   ux0:data/kotor/assets/     the 18 APK assets AAssetManager_open serves
 *   ux0:data/kotor/  (*.obb)   OBB archives / extracted game data
 */

#include <vitasdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#include "config.h"
#include "fs_patch.h"
#include "so_util.h"
#include "log.h"
#include "gameprof.h"
#include "modset.h"

#define ASSET_PATH DATA_PATH "/assets"

// Android path prefixes we rewrite onto DATA_PATH. Longest/most-specific first.
static const char *android_prefixes[] = {
  "/storage/emulated/0/Android/obb/com.aspyr.swkotor",
  "/storage/emulated/0/Android/data/com.aspyr.swkotor/files",
  "/storage/emulated/0/Android/data/com.aspyr.swkotor",
  "/data/data/com.aspyr.swkotor/files",
  "/data/data/com.aspyr.swkotor",
  "/sdcard/Android/obb/com.aspyr.swkotor",
  "/sdcard",
};

#define FS_REL_LOG_LIMIT 600
static unsigned g_rel_log_n = 0;

static const char *fs_translate_set(const char *in, char *out, int outsz,
                                    int do_log, int use_set);

static const char *fs_translate_ex(const char *in, char *out, int outsz, int do_log) {
  return fs_translate_set(in, out, outsz, do_log, 1);
}

const char *fs_translate(const char *in, char *out, int outsz) {
  return fs_translate_ex(in, out, outsz, 1);
}

// `do_log` exists for stat(): CExoBaseInternal::GetDirectoryList stats every
// candidate name in a directory, so logging each translation would bury the log.
//
// `use_set` 0 gives the card root's own path even where the active mod set
// (modset.h) has the file -- opendir needs both to merge their listings.
static const char *fs_translate_set(const char *in, char *out, int outsz,
                                    int do_log, int use_set) {
  if (!in) { out[0] = 0; return out; }

  // The active mod set stands in for the card root's game-data files it has,
  // whichever spelling of the card root the game used.
  if (use_set && strncmp(in, DATA_PATH "/", sizeof(DATA_PATH)) == 0 &&
      modset_redirect(in + sizeof(DATA_PATH), out, outsz))
    return out;

  // Already a Vita path -- leave it alone.
  if (strncmp(in, "ux0:", 4) == 0 || strncmp(in, "app0:", 5) == 0 ||
      strncmp(in, "ur0:", 4) == 0) {
    snprintf(out, outsz, "%s", in);
    return out;
  }

  for (unsigned i = 0; i < sizeof(android_prefixes) / sizeof(*android_prefixes); i++) {
    size_t plen = strlen(android_prefixes[i]);
    if (strncmp(in, android_prefixes[i], plen) == 0) {
      if (use_set && modset_redirect(in + plen, out, outsz)) return out;
      snprintf(out, outsz, "%s%s", DATA_PATH, in + plen);   // keep tail after prefix
      if (do_log) log_printf("[FS] %s -> %s", in, out);
      return out;
    }
  }

  // Unknown absolute path: fall back to DATA_PATH + the whole path, and flag it
  // so an unexpected location shows up in the log instead of silently failing.
  if (in[0] == '/') {
    snprintf(out, outsz, "%s%s", DATA_PATH, in);
    if (do_log) log_printf("[FS] (unmapped abs) %s -> %s", in, out);
    return out;
  }

  // Relative path -- resolve against the writable root.
  if (use_set && modset_redirect(in, out, outsz)) return out;
  snprintf(out, outsz, "%s/%s", DATA_PATH, in);
  // log97: in-game the game reopens the texture packs per texture load --
  // swpc_tex_gui.erf alone was translated 449 times -- and each log line is an
  // open/write/close on the memory card. Budget it; the interesting translations
  // all happen during bring-up.
  if (do_log && g_rel_log_n < FS_REL_LOG_LIMIT) {
    log_printf("[FS] (rel) %s -> %s", in, out);
    if (++g_rel_log_n == FS_REL_LOG_LIMIT)
      log_printf("[FS] (rel) path trace silenced after %d lines (steady state)",
                 FS_REL_LOG_LIMIT);
  }
  return out;
}

// ---- posix file/dir ops (translate + log + forward to newlib/sceIo) --------
static int fs_access(const char *path, int mode) {
  char t[512];
  fs_translate(path, t, sizeof(t));
  uint64_t t0 = sceKernelGetProcessTimeWide();
  int r = access(t, mode);
  gameprof_io_note(r ? "accMISS" : "access", t, sceKernelGetProcessTimeWide() - t0);
  // Budgeted like the other path traces: a game that probes in a loop must not
  // turn every miss into a card write.
  static unsigned miss_n = 0;
  if (r != 0 && miss_n < FS_REL_LOG_LIMIT) {
    log_printf("[FS] access MISS: %s", t);
    if (++miss_n == FS_REL_LOG_LIMIT)
      log_printf("[FS] access MISS trace silenced after %d lines", FS_REL_LOG_LIMIT);
  }
  return r;
}
// What the game holds as its DIR *: up to two directories read one after the
// other. `over` is the active mod set's copy of the directory (modset.h) and
// is listed first; `base` is the card root's, minus any name `over` also has,
// so the game sees each name once and opens the set's copy of it. Without a
// set, or when the set lacks this directory, only `base` is open. The game
// imports nothing but opendir/readdir/closedir, so it never looks inside.
typedef struct {
  DIR *over;
  DIR *base;
  char over_path[512];
} FsDir;

static DIR *fs_opendir(const char *path) {
  char t[512], root[512];
  fs_translate(path, t, sizeof(t));
  fs_translate_set(path, root, sizeof(root), 0, 0);

  uint64_t t0 = sceKernelGetProcessTimeWide();
  FsDir *fd = calloc(1, sizeof *fd);
  if (!fd) return NULL;
  if (strcmp(t, root) != 0) {
    fd->over = opendir(t);
    snprintf(fd->over_path, sizeof fd->over_path, "%s", t);
  }
  fd->base = opendir(root);
  if (!fd->over && !fd->base) {
    log_printf("[FS] opendir MISS: %s", root);
    free(fd);
    return NULL;
  }
  if (fd->over)
    log_printf("[FS] opendir %s over %s%s", t, root, fd->base ? "" : " (absent)");
  gameprof_io_note("opendir", root, sceKernelGetProcessTimeWide() - t0);
  return (DIR *)fd;
}
// log90 ROOT CAUSE: `stat` was bound straight to newlib's with NO path
// translation, so every call went to a bare relative path and always failed.
//
// That is fatal because Aspyr replaced directory enumeration wholesale.
// `CExoBaseInternal::GetDirectoryList` (+0x4b5e90) contains no opendir/readdir at
// all -- it walks the OBB zip index with `mz_zip_reader_locate_file` and probes
// the writable card with `stat`. Directories that live in the OBB (modules/,
// rims/, lips/) therefore enumerate fine, while `currentgame/` -- which exists
// only on the card -- always came back EMPTY. `CExoKeyTable::AddDirectoryContents`
// then built an empty CURRENTGAME: key table, so the `GetKeyEntry(<module>, .rsv)`
// / `GetKeyEntry(<module>, .rim)` gate in `CExoResMan::AsyncLoad` (+0x4c8ff2 /
// +0x4c904a) missed, `AddKeyTable("CURRENTGAME:END_M01AA", type=4)` was never
// reached, the module's CRes id stayed 0xFFFFFFFF and Demand returned NULL.
//
// Only stat's RETURN VALUE is read (`cmp r4,#0` at +0x4b6788) -- the struct is
// never touched -- so the bionic-vs-newlib `struct stat` layout mismatch is moot,
// and newlib's smaller struct cannot overflow the caller's frame either.
static unsigned g_stat_n = 0;
static int fs_stat(const char *path, struct stat *st) {
  char t[512];
  fs_translate_ex(path, t, sizeof(t), 0);
  uint64_t t0 = sceKernelGetProcessTimeWide();
  int r = stat(t, st);
  gameprof_io_note(r ? "statMISS" : "stat", t, sceKernelGetProcessTimeWide() - t0);
  // GetDirectoryList stats every candidate name, so keep this bounded: a sample
  // of the early calls, then only the paths this bug is about.
  if (g_stat_n < 48 || (path && strstr(path, "currentgame") && g_stat_n < 256))
    log_printf("[FS] stat: %s -> %d  [#%u]", t, r, g_stat_n + 1);
  g_stat_n++;
  return r;
}

// ROOT CAUSE of the module-load stall (log95, confirmed by disassembly).
//
// `readdir` was forwarded straight through, handing the .so a **vitasdk**
// `struct dirent` while it was compiled against **bionic's**. The layouts put
// `d_name` in completely different places:
//
//   bionic ARM32:  u64 d_ino @0, s64 d_off @8, u16 d_reclen @16,
//                  u8 d_type @18, char d_name[256] @19
//   vitasdk:       SceIoStat d_stat @0 (88 bytes), char d_name[256] @88
//
// `CExoBaseInternal::GetDirectoryList` is the only readdir caller in libKOTOR
// (+0x4b6322) and it does exactly this:
//
//   4b6346:  r1 = dirent + 19        ; bionic d_name
//   4b634c:  memcpy(new char[272], r1, 256)
//
// Offset 19 in the vitasdk struct is the **high byte of `st_ctime.month`**,
// which is always 0 because a month is <= 12. So every name read from a real
// directory came back as an empty string -- deterministically, every time.
//
// Why that stalled the load: directories that live in the OBB (rims/, modules/,
// override/) are enumerated from the zip index and were never affected, which is
// why 234 modules/ keys landed and everything looked healthy. `currentgame/`
// exists only on the card, so its one real file -- END_M01AA.rim, a byte-exact
// 55565-byte copy -- yielded a single EMPTY name. `AddDirectoryContents` then
// rejected it at its first filter (`GetResTypeFromFile("")` finds no '.' and
// returns 0xFFFF), so `AddKey` was never called, the CURRENTGAME: key table
// stayed empty, `GetKeyEntry("end_m01aa", .rim)` missed inside
// `CExoResMan::AsyncLoad`, `AddKeyTable("CURRENTGAME:END_M01AA", type=4)` never
// ran, the module's CRes id stayed 0xFFFFFFFF, `Demand` returned NULL and
// `LoadModuleStart` returned 1.
//
// It also explains the MODULES: 234 -> 235 bump: the card's directory entries all
// collapse to one empty string via CExoArrayList::AddUnique.
struct bionic_dirent {
  uint64_t d_ino;
  int64_t  d_off;
  uint16_t d_reclen;
  uint8_t  d_type;
  char     d_name[256];
};
_Static_assert(__builtin_offsetof(struct bionic_dirent, d_name) == 19,
               "bionic d_name must sit at +19 -- the .so hardcodes that offset");

#define BIONIC_DT_DIR 4
#define BIONIC_DT_REG 8

static struct bionic_dirent g_bdirent;   // readdir's return is caller-borrowed
static unsigned g_readdir_n = 0;

static struct dirent *next_entry(FsDir *fd) {
  if (fd->over) {
    struct dirent *e = readdir(fd->over);
    if (e) return e;
    closedir(fd->over);
    fd->over = NULL;
  }
  while (fd->base) {
    struct dirent *e = readdir(fd->base);
    if (!e) return NULL;
    if (!fd->over_path[0]) return e;
    char p[800];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", fd->over_path, e->d_name);
    if (stat(p, &st) != 0) return e;      // the set has no such name
  }
  return NULL;
}

static void *fs_readdir(DIR *d) {
  uint64_t t0 = sceKernelGetProcessTimeWide();
  struct dirent *e = next_entry((FsDir *)d);
  gameprof_io_note("readdir", e ? e->d_name : "(end)", sceKernelGetProcessTimeWide() - t0);
  if (!e) return NULL;
  memset(&g_bdirent, 0, sizeof g_bdirent);
  g_bdirent.d_ino    = ++g_readdir_n;          // some callers skip ino == 0
  g_bdirent.d_reclen = (uint16_t)sizeof g_bdirent;
  g_bdirent.d_type   = SCE_S_ISDIR(e->d_stat.st_mode) ? BIONIC_DT_DIR : BIONIC_DT_REG;
  snprintf(g_bdirent.d_name, sizeof g_bdirent.d_name, "%s", e->d_name);
  if (g_readdir_n <= 64)
    log_printf("[FS] readdir -> \"%s\" type=%u  [#%u]", g_bdirent.d_name,
               g_bdirent.d_type, g_readdir_n);
  return &g_bdirent;
}
static int fs_closedir(DIR *d) {
  FsDir *fd = (FsDir *)d;
  if (!fd) return -1;
  if (fd->over) closedir(fd->over);
  int r = fd->base ? closedir(fd->base) : 0;
  free(fd);
  return r;
}
static int fs_unlink(const char *path) {
  char t[512];
  fs_translate(path, t, sizeof(t));
  return unlink(t);
}
static int fs_mkdir(const char *path, mode_t mode) {
  char t[512];
  fs_translate(path, t, sizeof(t));
  return mkdir(t, mode);
}
static int fs_rmdir(const char *path) {
  char t[512];
  fs_translate(path, t, sizeof(t));
  return rmdir(t);
}
static int fs_rename(const char *a, const char *b) {
  char ta[512], tb[512];
  fs_translate(a, ta, sizeof(ta));
  fs_translate(b, tb, sizeof(tb));
  return rename(ta, tb);
}

// ---- SDL Android extension ------------------------------------------------
// Vita SDL2 has no SDL_AndroidGetExternalStoragePath; the game calls it to find
// its writable root. Hand back ux0:data/kotor so all derived paths land there.
static const char *SDL_AndroidGetExternalStoragePath(void) {
  log_printf("[FS] SDL_AndroidGetExternalStoragePath -> %s", DATA_PATH);
  return DATA_PATH;
}

// ---- Android Asset Manager (NDK) ------------------------------------------
// Minimal AAsset backed by a real file under ux0:data/kotor/assets/.
typedef struct {
  FILE *fp;
  long  size;
  long  pos;
} FakeAsset;

static void *AAssetManager_open(void *mgr, const char *filename, int mode) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", ASSET_PATH, filename ? filename : "");
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    log_printf("[ASSET] open MISS: %s", path);
    return NULL;
  }
  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  FakeAsset *a = calloc(1, sizeof(FakeAsset));
  if (!a) { fclose(fp); return NULL; }
  a->fp = fp; a->size = sz; a->pos = 0;
  log_printf("[ASSET] open: %s (%ld bytes)", filename ? filename : "?", sz);
  return a;
}
static int AAsset_read(void *asset, void *buf, size_t count) {
  FakeAsset *a = asset;
  if (!a) return -1;
  size_t n = fread(buf, 1, count, a->fp);
  a->pos += (long)n;
  return (int)n;
}
static long AAsset_seek(void *asset, long off, int whence) {
  FakeAsset *a = asset;
  if (!a) return -1;
  if (fseek(a->fp, off, whence) != 0) return -1;
  a->pos = ftell(a->fp);
  return a->pos;
}
static long AAsset_getRemainingLength(void *asset) {
  FakeAsset *a = asset;
  return a ? a->size - a->pos : 0;
}
static void AAsset_close(void *asset) {
  FakeAsset *a = asset;
  if (!a) return;
  if (a->fp) fclose(a->fp);
  free(a);
}

// ---- resolver table -------------------------------------------------------
static const so_default_dynlib fs_dynlib[] = {
  { "access",   (uintptr_t)&fs_access },
  { "stat",     (uintptr_t)&fs_stat },
  { "opendir",  (uintptr_t)&fs_opendir },
  { "readdir",  (uintptr_t)&fs_readdir },
  { "closedir", (uintptr_t)&fs_closedir },
  { "unlink",   (uintptr_t)&fs_unlink },
  { "mkdir",    (uintptr_t)&fs_mkdir },
  { "rmdir",    (uintptr_t)&fs_rmdir },
  { "rename",   (uintptr_t)&fs_rename },
  { "SDL_AndroidGetExternalStoragePath", (uintptr_t)&SDL_AndroidGetExternalStoragePath },
  { "AAssetManager_open",         (uintptr_t)&AAssetManager_open },
  { "AAsset_read",                (uintptr_t)&AAsset_read },
  { "AAsset_seek",                (uintptr_t)&AAsset_seek },
  { "AAsset_getRemainingLength",  (uintptr_t)&AAsset_getRemainingLength },
  { "AAsset_close",               (uintptr_t)&AAsset_close },
};
const int fs_dynlib_size = sizeof(fs_dynlib);
const so_default_dynlib *fs_get_dynlib(void) { return fs_dynlib; }
