/* modset.c -- see modset.h. */

#include <vitasdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modset.h"
#include "config.h"
#include "log.h"
#include "main.h"
#include "so_util.h"
#include "trname.h"

#define MODS_DIR     DATA_PATH "/mods"
#define ACTIVE_FILE  MODS_DIR "/active.txt"
#define MENU_FLAG    MODS_DIR "/menu.flag"
#define NAME_MAX_LEN 64    /* folder names, including the NUL */
#define LABEL_MAX_LEN 32

/* What a set may hold: the card-root names the engine reads game data from.
 * A path whose first component is one of these is served from the active set;
 * anything else (saves, currentgame, swkotor.ini, the OBBs) stays shared. */
static const char *const kContent[] = {
  "override", "modules", "rims", "lips", "texturepacks", "movies",
  "streamwaves", "streamvoice", "streammusic", "streamsounds",
  "dialog.tlk", "tv_dialog.tlk",
};

typedef struct {
  char name[NAME_MAX_LEN];
  char label[LABEL_MAX_LEN];
} Set;

#define NCONTENT (sizeof kContent / sizeof *kContent)

static Set s_set[MODSET_MAX];
static int s_count  = 0;
static int s_active = -1;
static int s_menu   = 0;

/* Which kContent names the active set actually has, so a set that brings no
 * texturepacks/ costs the game's texture-pack opens nothing at all. */
static unsigned char s_has[NCONTENT];

static void note_contents(void) {
  memset(s_has, 0, sizeof s_has);
  if (s_active < 0) return;
  for (unsigned i = 0; i < NCONTENT; i++) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s/%s", MODS_DIR, s_set[s_active].name,
             kContent[i]);
    SceIoStat st;
    memset(&st, 0, sizeof st);
    s_has[i] = sceIoGetstat(path, &st) >= 0;
    if (s_has[i]) log_printf("[mods]   %s has %s", s_set[s_active].name, kContent[i]);
  }
}

static int by_name(const void *a, const void *b) {
  return strcmp(((const Set *)a)->name, ((const Set *)b)->name);
}

/* The saved choice, or "" for vanilla. */
static void read_active(char *out, int outsz) {
  out[0] = '\0';
  SceUID fd = sceIoOpen(ACTIVE_FILE, SCE_O_RDONLY, 0);
  if (fd < 0) return;
  int n = sceIoRead(fd, out, (unsigned)(outsz - 1));
  sceIoClose(fd);
  if (n < 0) n = 0;
  out[n] = '\0';
  out[strcspn(out, "\r\n")] = '\0';
}

int modset_scan(void) {
  s_count = 0;
  s_active = -1;

  SceIoStat st;
  memset(&st, 0, sizeof st);
  s_menu = sceIoGetstat(MENU_FLAG, &st) >= 0;
  if (s_menu) {
    sceIoRemove(MENU_FLAG);
    log_printf("[mods] the Google Play button asked for the mod menu");
  }

  SceUID d = sceIoDopen(MODS_DIR);
  if (d < 0) {
    log_printf("[mods] no %s -- vanilla", MODS_DIR);
    return 0;
  }
  SceIoDirent e;
  memset(&e, 0, sizeof e);
  while (sceIoDread(d, &e) > 0) {
    const char *name = e.d_name;
    if (SCE_S_ISDIR(e.d_stat.st_mode) && name[0] != '.') {
      if (strlen(name) >= NAME_MAX_LEN) {
        log_printf("[mods] skipping %s: folder name longer than %d bytes",
                   name, NAME_MAX_LEN - 1);
      } else if (s_count == MODSET_MAX) {
        log_printf("[mods] skipping %s: only %d mod sets are listed",
                   name, MODSET_MAX);
      } else {
        Set *s = &s_set[s_count++];
        strcpy(s->name, name);
        trname_label(name, s->label, sizeof s->label);
      }
    }
    memset(&e, 0, sizeof e);
  }
  sceIoDclose(d);

  /* Card order is write order; the menu should not reshuffle when a folder is
   * copied again. */
  qsort(s_set, (size_t)s_count, sizeof s_set[0], by_name);
  for (int i = 0; i < s_count; i++)
    log_printf("[mods] found mod set %d: %s", i, s_set[i].name);

  char want[NAME_MAX_LEN];
  read_active(want, sizeof want);
  if (want[0]) {
    for (int i = 0; i < s_count; i++)
      if (trname_eq(s_set[i].name, want)) s_active = i;
    if (s_active < 0)
      log_printf("[mods] %s names \"%s\", which is not a folder in %s -- vanilla",
                 ACTIVE_FILE, want, MODS_DIR);
  }
  log_printf("[mods] active mod set: %s",
             s_active >= 0 ? s_set[s_active].name : "vanilla");
  note_contents();
  return s_count;
}

int modset_count(void) { return s_count; }

const char *modset_name(int i) {
  return (i >= 0 && i < s_count) ? s_set[i].name : NULL;
}

const char *modset_label(int i) {
  return (i >= 0 && i < s_count) ? s_set[i].label : NULL;
}

int modset_active(void) { return s_active; }

void modset_choose(int i) {
  s_active = (i >= 0 && i < s_count) ? i : -1;
  const char *name = s_active >= 0 ? s_set[s_active].name : "";

  sceIoMkdir(MODS_DIR, 0777);
  SceUID fd = sceIoOpen(ACTIVE_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  int n = (int)strlen(name);
  int wrote = (fd >= 0) ? sceIoWrite(fd, name, (unsigned)n) : -1;
  if (fd >= 0) sceIoClose(fd);
  if (wrote != n)
    log_printf("!!! [mods] could not save the choice to %s -- it lasts this run only",
               ACTIVE_FILE);
  log_printf("[mods] chose mod set: %s", s_active >= 0 ? name : "vanilla");
  note_contents();
}

int modset_menu_requested(void) {
  int r = s_menu;
  s_menu = 0;
  return r;
}

const char *modset_redirect(const char *tail, char *out, int outsz) {
  if (s_active < 0 || !tail) return NULL;
  while (tail[0] == '.' && tail[1] == '/') tail += 2;
  while (tail[0] == '/') tail++;

  size_t first = strcspn(tail, "/");
  char head[NAME_MAX_LEN];
  if (first == 0 || first >= sizeof head) return NULL;
  memcpy(head, tail, first);
  head[first] = '\0';

  for (unsigned i = 0; i < NCONTENT; i++) {
    if (!s_has[i] || !trname_eq(head, kContent[i])) continue;
    snprintf(out, (size_t)outsz, "%s/%s/%s", MODS_DIR, s_set[s_active].name, tail);
    /* Strip a trailing slash: FAT stats "dir/" as missing. */
    size_t n = strlen(out);
    if (n > 0 && out[n - 1] == '/') out[n - 1] = '\0';
    SceIoStat st;
    memset(&st, 0, sizeof st);
    return sceIoGetstat(out, &st) >= 0 ? out : NULL;
  }
  return NULL;
}

/* ---- the Google Play button ------------------------------------------------
 *
 * CSWGuiMainMenu::OnGooglePlayButton(CSWGuiControl *). The original takes a
 * mutex, asks Java to sign in to Play Games and waits on a condition the Java
 * side signals -- none of which exists here. Its result is SDL_UnlockMutex's
 * (a tail call) and nothing reads it, so 0 is a faithful return.
 *
 * Only reachable from the main menu, so there is no game in progress to lose
 * by restarting. */
static int OnGooglePlayButton_hook(void *self, void *control) {
  (void)self; (void)control;
  sceIoMkdir(MODS_DIR, 0777);
  SceUID fd = sceIoOpen(MENU_FLAG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0) {
    log_printf("!!! [mods] cannot write %s (0x%08x) -- not restarting",
               MENU_FLAG, (unsigned)fd);
    return 0;
  }
  sceIoClose(fd);

  log_printf("[mods] Google Play button -- restarting into the mod menu");
  log_flush();
  int r = sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);

  /* Still here: the restart was refused. The request stays on the card, so
   * closing and relaunching the game by hand opens the menu all the same. */
  log_printf("!!! [mods] sceAppMgrLoadExec failed (0x%08x) -- the mod menu opens "
             "on the next launch", (unsigned)r);
  return 0;
}

void modset_install_button(void) {
  uintptr_t f = so_symbol(&kotor_mod,
                          "_ZN14CSWGuiMainMenu18OnGooglePlayButtonEP13CSWGuiControl");
  if (!f) {
    log_printf("!!! [mods] OnGooglePlayButton not found -- no mod menu button");
    return;
  }
  hook_thumb(f, (uintptr_t)&OnGooglePlayButton_hook);
  log_printf("[mods] Google Play button now opens the mod menu");
}
