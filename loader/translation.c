/* translation.c -- see translation.h. */

#include <vitasdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "translation.h"
#include "config.h"
#include "log.h"
#include "trname.h"

#define TRANSLATION_DIR  DATA_PATH "/translations"
#define NAME_MAX_LEN     64    /* folder and file names, including the NUL */
#define LABEL_MAX_LEN    32
#define FILES_MAX        128   /* files remembered in the active folder */

typedef struct {
  char name[NAME_MAX_LEN];
  char label[LABEL_MAX_LEN];
} Folder;

static Folder s_folder[TRANSLATION_MAX];
static int    s_count  = 0;
static int    s_active = -1;

/* The active folder's listing, taken once when it is selected, so serving a
 * file costs a string compare rather than a card access per game open. */
static char s_file[FILES_MAX][NAME_MAX_LEN];
static int  s_nfile = 0;

/* Call `fn` for every plain file in `dir`. Returns how many it saw, -1 if the
 * directory cannot be opened. */
static int each_file(const char *dir, void (*fn)(const char *, void *), void *ctx) {
  SceUID d = sceIoDopen(dir);
  if (d < 0) return -1;
  int n = 0;
  SceIoDirent e;
  memset(&e, 0, sizeof e);
  while (sceIoDread(d, &e) > 0) {
    if (!SCE_S_ISDIR(e.d_stat.st_mode)) { fn(e.d_name, ctx); n++; }
    memset(&e, 0, sizeof e);
  }
  sceIoDclose(d);
  return n;
}

static void note_table(const char *file, void *ctx) {
  if (trname_eq(file, "tv_dialog.tlk") || trname_eq(file, "dialog.tlk"))
    *(int *)ctx = 1;
}

static int by_name(const void *a, const void *b) {
  return strcmp(((const Folder *)a)->name, ((const Folder *)b)->name);
}

int translation_scan(void) {
  s_count = 0;
  s_active = -1;
  s_nfile = 0;

  SceUID d = sceIoDopen(TRANSLATION_DIR);
  if (d < 0) {
    log_printf("[tr] no %s -- no fan translations", TRANSLATION_DIR);
    return 0;
  }
  SceIoDirent e;
  memset(&e, 0, sizeof e);
  while (sceIoDread(d, &e) > 0) {
    const char *name = e.d_name;
    int isdir = SCE_S_ISDIR(e.d_stat.st_mode);
    if (isdir && name[0] != '.' && strlen(name) >= NAME_MAX_LEN) {
      log_printf("[tr] skipping %s: folder name longer than %d bytes",
                 name, NAME_MAX_LEN - 1);
    } else if (isdir && name[0] != '.') {
      char path[sizeof TRANSLATION_DIR + NAME_MAX_LEN];
      snprintf(path, sizeof path, "%s/%s", TRANSLATION_DIR, name);
      int has_table = 0;
      each_file(path, note_table, &has_table);
      if (!has_table) {
        log_printf("[tr] skipping %s: no tv_dialog.tlk or dialog.tlk in it", name);
      } else if (s_count == TRANSLATION_MAX) {
        log_printf("[tr] skipping %s: only %d translations are listed",
                   name, TRANSLATION_MAX);
      } else {
        Folder *f = &s_folder[s_count++];
        strcpy(f->name, name);
        trname_label(name, f->label, sizeof f->label);
      }
    }
    memset(&e, 0, sizeof e);
  }
  sceIoDclose(d);

  /* The card returns entries in whatever order they were written; the picker
   * should not reshuffle because someone copied a folder again. */
  qsort(s_folder, (size_t)s_count, sizeof s_folder[0], by_name);
  for (int i = 0; i < s_count; i++)
    log_printf("[tr] found translation %d: %s", i, s_folder[i].name);
  return s_count;
}

int translation_count(void) { return s_count; }

const char *translation_name(int i) {
  return (i >= 0 && i < s_count) ? s_folder[i].name : NULL;
}

const char *translation_label(int i) {
  return (i >= 0 && i < s_count) ? s_folder[i].label : NULL;
}

int translation_find(const char *name) {
  if (!name || !name[0]) return -1;
  for (int i = 0; i < s_count; i++)
    if (trname_eq(s_folder[i].name, name)) return i;
  return -1;
}

static void remember_file(const char *file, void *ctx) {
  (void)ctx;
  if (strlen(file) >= NAME_MAX_LEN) {
    log_printf("[tr]   ignoring %s: name longer than %d bytes", file, NAME_MAX_LEN - 1);
    return;
  }
  if (s_nfile == FILES_MAX) {
    log_printf("[tr]   ignoring %s: only %d files per translation", file, FILES_MAX);
    return;
  }
  strcpy(s_file[s_nfile++], file);
  log_printf("[tr]   overrides %s", file);
}

/* The game's own tables all hold this many entries (dialog.tlk in main.obb
 * and tv_dialog.tlk in patch.obb alike; only 65 controller prompts differ).
 * A translation with a different count was made from some other edition, and
 * its lines will not land where the game looks for them -- which is worth
 * saying in the log before anyone reports "wrong text". */
#define GAME_TLK_ENTRIES 49265

static void check_table(void) {
  char path[sizeof TRANSLATION_DIR + 2 * NAME_MAX_LEN];
  if (!translation_path("tv_dialog.tlk", path, sizeof path)) return;
  unsigned char head[20];
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  int n = (fd >= 0) ? sceIoRead(fd, head, sizeof head) : -1;
  if (fd >= 0) sceIoClose(fd);
  if (n != (int)sizeof head || memcmp(head, "TLK V3.0", 8) != 0) {
    log_printf("!!! [tr] %s is not a TLK V3.0 table -- the game will not read it",
               path);
    return;
  }
  unsigned count = (unsigned)head[12] | ((unsigned)head[13] << 8) |
                   ((unsigned)head[14] << 16) | ((unsigned)head[15] << 24);
  log_printf("%s[tr] %s: %u entries (the game's own tables have %u)%s",
             count == GAME_TLK_ENTRIES ? "" : "!!! ", path, count,
             GAME_TLK_ENTRIES,
             count == GAME_TLK_ENTRIES ? "" : " -- lines may be missing or shifted");
}

void translation_select(int i) {
  s_nfile = 0;
  if (i < 0 || i >= s_count) {
    s_active = -1;
    return;
  }
  s_active = i;
  char path[256];
  snprintf(path, sizeof path, "%s/%s", TRANSLATION_DIR, s_folder[i].name);
  log_printf("[tr] active translation: %s", s_folder[i].name);
  each_file(path, remember_file, NULL);
  check_table();
}

int translation_active(void) { return s_active; }

static const char *find_file(const char *base) {
  for (int i = 0; i < s_nfile; i++)
    if (trname_eq(s_file[i], base)) return s_file[i];
  return NULL;
}

const char *translation_path(const char *base, char *out, int outsz) {
  if (s_active < 0 || !base) return NULL;
  const char *file = find_file(base);
  if (!file && trname_eq(base, "tv_dialog.tlk")) file = find_file("dialog.tlk");
  if (!file) return NULL;
  snprintf(out, (size_t)outsz, "%s/%s/%s", TRANSLATION_DIR,
           s_folder[s_active].name, file);
  return out;
}
