/* langsel.c -- see langsel.h. */

#include <vitasdk.h>
#include <vitaGL.h>
#include <string.h>

#include "langsel.h"
#include "config.h"
#include "font.h"
#include "ini.h"
#include "log.h"
#include "obbzip.h"

#if LANGSEL_ENABLE

/* Indexed by INI_LANG_*, so the row the user is standing on IS the id to save.
 *
 * Native spellings, in CP1252: dialogfont16x16b.txi defines all 256 cells with
 * distinct per-glyph widths across the accented range, so the atlas really does
 * carry these letters rather than blanks. Written as escapes because the bytes
 * are CP1252 and this file is not, and split mid-string because a hex escape
 * would otherwise swallow the letter after it. */
static const char *const kNames[] = {
  "ENGLISH", "FRAN\xC7" "AIS", "ITALIANO", "DEUTSCH", "ESPA\xD1" "OL",
};
_Static_assert(sizeof kNames / sizeof kNames[0] == LANGSEL_BUILTIN,
               "kNames must hold exactly the game's own languages");

/* Built-in rows plus however many fan translations fit. */
#define LANGSEL_MAX_ROWS 32

/* Same trick loadscreen.c uses for its bar: a scissored clear needs no shader,
 * no buffer and no texture, so it cannot disturb anything the font path sets
 * up. Coordinates are GL window space, origin bottom-left. */
static void fill(int x, int y, int w, int h, float r, float g, float b) {
  glEnable(GL_SCISSOR_TEST);
  glScissor(x, y, w, h);
  glClearColor(r, g, b, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glDisable(GL_SCISSOR_TEST);
}

static void centred(const char *s, float y, float scale,
                    float r, float g, float b) {
  float w = (float)font_measure(s, -1) * scale;
  font_draw(s, -1, (SCREEN_W - w) * 0.5f, y, scale, r, g, b, 1.0f);
}

#define ROW_Y(i)  (LANGSEL_ROWS_Y + (i) * LANGSEL_ROW_STEP)

/* A folder name can be far longer than FRANCAIS, so a row that would spill out
 * of its highlight is drawn smaller rather than cut. */
static void row(const char *s, int slot, int on) {
  float scale = LANGSEL_ROW_SCALE;
  float w = (float)font_measure(s, -1);
  float room = (float)(LANGSEL_HL_W - 24);
  if (w * scale > room && w > 0) scale = room / w;
  centred(s, (float)ROW_Y(slot) + (LANGSEL_ROW_SCALE - scale) * 8.0f, scale,
          on ? 0.70f : 0.42f, on ? 0.82f : 0.49f, on ? 1.00f : 0.60f);
}

static void draw(const char *const *rows, int count, int sel, int top,
                 const char *footer) {
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
  glClearColor(0.02f, 0.03f, 0.06f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  /* The highlight goes down before any text, because filling is a clear and a
   * clear would wipe glyphs already drawn. Art coordinates run top-down and GL
   * window space runs bottom-up, hence the flip. */
  fill((SCREEN_W - LANGSEL_HL_W) / 2,
       SCREEN_H - (ROW_Y(sel - top) - LANGSEL_HL_PAD) - LANGSEL_HL_H,
       LANGSEL_HL_W, LANGSEL_HL_H, 0.11f, 0.18f, 0.31f);

  /* Top-down ortho, matching the sense font_draw's y argument is written in. */
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, SCREEN_W, SCREEN_H, 0, -1, 1);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  centred("CHOOSE A LANGUAGE", (float)LANGSEL_TITLE_Y, LANGSEL_TITLE_SCALE,
          0.588f, 0.667f, 0.784f);

  for (int i = top; i < count && i < top + LANGSEL_VISIBLE_ROWS; i++)
    row(rows[i], i - top, i == sel);

  /* With fan translations the list can outgrow the screen, and scrolls; these
   * say there is more in that direction. */
  if (top > 0)
    centred("...", (float)LANGSEL_MORE_UP_Y, 1.0f, 0.43f, 0.63f, 0.92f);
  if (top + LANGSEL_VISIBLE_ROWS < count)
    centred("...", (float)ROW_Y(LANGSEL_VISIBLE_ROWS), 1.0f,
            0.43f, 0.63f, 0.92f);

  centred("CHANGE THIS LATER BY HOLDING L WHILE THE GAME STARTS",
          (float)LANGSEL_HINT_Y, 1.0f, 0.33f, 0.40f, 0.50f);
  centred(footer, (float)LANGSEL_FOOTER_Y, 1.0f, 0.43f, 0.63f, 0.92f);

  /* Leave the matrices as we found them rather than handing the game our
   * ortho; it reads none of this, but a stale matrix surfaces two bugs later. */
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);

  vglSwapBuffers(GL_FALSE);
}

/* Which of cross and circle confirms. The console's own setting decides, the
 * same way ime_patch.c configures its dialogs, so the picker agrees with every
 * other menu on the system. A failed read leaves cross confirming: worst case
 * the two are swapped, and since the footer names them by the same variable,
 * the screen still tells the truth about itself. */
static void enter_buttons(unsigned *ok, unsigned *cancel, const char **footer) {
  int enter = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
  SceAppUtilInitParam init;
  SceAppUtilBootParam boot;
  memset(&init, 0, sizeof init);
  memset(&boot, 0, sizeof boot);
  sceAppUtilInit(&init, &boot);          /* harmless if something already did */
  sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter);

  if (enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) {
    *ok = SCE_CTRL_CIRCLE;
    *cancel = SCE_CTRL_CROSS;
    *footer = "UP DOWN  MOVE      CIRCLE  CONFIRM      CROSS  CANCEL";
  } else {
    *ok = SCE_CTRL_CROSS;
    *cancel = SCE_CTRL_CIRCLE;
    *footer = "UP DOWN  MOVE      CROSS  CONFIRM      CIRCLE  CANCEL";
  }
}

/* Which bit the Vita's L shoulder button actually sets.
 *
 * Both, because it depends on the reader: psp2common/ctrl.h says "Vita's L
 * Trigger and R Trigger are mapped to L1 and R1 when using
 * sceCtrlPeekBufferPositiveExt2", so L is SCE_CTRL_L1 (0x400) here and
 * SCE_CTRL_LTRIGGER (0x100) under the plain reader -- and LTRIGGER is an alias
 * for L2, which the Vita has no such button for.
 *
 * Testing LTRIGGER alone under the Ext2 reader is why the first two attempts
 * at this trigger did nothing on hardware: log178 caught the pad reporting
 * 0x00000400 on all 164 samples of a held L while the check looked at 0x100.
 * Accepting both costs nothing, survives a change of reader, and is right for
 * a DS4 on PSTV, where L1 is the same finger. */
#define LANGSEL_L_MASK (SCE_CTRL_L1 | SCE_CTRL_LTRIGGER)

/* ---- the L latch ----------------------------------------------------------
 *
 * Everything the picker knows about the trigger, gathered across the whole
 * blind part of the boot rather than sampled at one unannounced instant.
 * s_bits is diagnostic and worth its two bytes: it separates "the user did not
 * press L" from "the pad is not reaching the loader at all", which look
 * identical from the picker and want completely different fixes. */
static volatile int      s_watching = 0;
static volatile int      s_latched  = 0;   /* L has been down at some point */
static volatile unsigned s_samples  = 0;
static volatile unsigned s_bits     = 0;   /* every button bit ever seen */
static uint64_t          s_watch_t0 = 0;

static int watch_thread(SceSize args, void *argp) {
  (void)args; (void)argp;
  while (s_watching) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof pad);
    if (sceCtrlPeekBufferPositiveExt2(0, &pad, 1) > 0) {
      s_samples++;
      s_bits |= pad.buttons;
      if ((pad.buttons & LANGSEL_L_MASK) && !s_latched) {
        s_latched = 1;
        log_printf("[langsel] L held at +%ums -- the picker will open",
                   (unsigned)((sceKernelGetProcessTimeWide() - s_watch_t0) / 1000));
      }
    }
    sceKernelDelayThread(LANGSEL_WATCH_MS * 1000);
  }
  return sceKernelExitDeleteThread(0);
}

void langsel_watch_begin(void) {
  s_watch_t0 = sceKernelGetProcessTimeWide();
  s_watching = 1;
  SceUID th = sceKernelCreateThread("langsel_watch", watch_thread,
                                    0x10000100, 0x1000, 0, 0, NULL);
  if (th < 0) {
    /* Not fatal: langsel_run() still takes its own reading, which is exactly
     * the old behaviour -- fragile, but not nothing. */
    s_watching = 0;
    log_printf("!!! [langsel] cannot start the L watcher (0x%08x)", (unsigned)th);
    return;
  }
  sceKernelStartThread(th, 0, NULL);
}

int langsel_run(int current, int have_key, const char *const *extra, int nextra,
                int *out) {
  /* The Ext2 reader, like input_patch.c: main() has already selected
   * ANALOG_WIDE, and this is the reader known to work under it on hardware. A
   * read that returns nothing leaves the buttons zero, which the loop below
   * treats as "no input" -- so a pad that never answers times the picker out
   * rather than pinning the boot. */
  SceCtrlData pad;
  memset(&pad, 0, sizeof pad);
  sceCtrlPeekBufferPositiveExt2(0, &pad, 1);

  /* The latch is the real trigger; this reading only covers the case where the
   * watcher could not start. */
  int held = s_latched || (pad.buttons & LANGSEL_L_MASK) != 0;
  s_watching = 0;

  /* Logged on every boot, including the ones that skip. The first version said
   * nothing when it skipped, which made "L did not register" and "this is not
   * even the new build" the same silence. s_bits is the discriminator: if L
   * was pressed it carries 0x100, and if the pad never reached us at all it is
   * 0 however many samples were taken. */
  log_printf("[langsel] trigger: have_key=%d L=%d (latch %d, pad 0x%08x, "
             "mask 0x%08x) -- watcher saw %u samples, buttons 0x%08x",
             have_key, held, s_latched, (unsigned)pad.buttons,
             (unsigned)LANGSEL_L_MASK, s_samples, s_bits);

  if (have_key && !held) return 0;

  /* The atlas is left loaded on purpose: loadscreen's own font_load() is a
   * no-op once it is up, so the picker costs the boot screen nothing. */
  ObbZip *z = obbzip_open(OBB_PATCH_PATH);
  if (z) { font_load(z); obbzip_close(z); }
  if (!font_ready()) {
    log_printf("[langsel] no font -- skipping the picker (language stays %s)",
               ini_language_code(current));
    return 0;
  }

  unsigned btn_ok, btn_cancel;
  const char *footer;
  enter_buttons(&btn_ok, &btn_cancel, &footer);

  const char *rows[LANGSEL_MAX_ROWS];
  int count = 0;
  for (int i = 0; i < LANGSEL_BUILTIN; i++) rows[count++] = kNames[i];
  for (int i = 0; i < nextra && count < LANGSEL_MAX_ROWS; i++)
    rows[count++] = extra[i];

  int sel = (current >= 0 && current < count) ? current : INI_LANG_EN;
  int top = 0;
  if (sel >= LANGSEL_VISIBLE_ROWS) top = sel - LANGSEL_VISIBLE_ROWS + 1;
  unsigned prev = pad.buttons;
  uint64_t idle = sceKernelGetProcessTimeWide();
  int confirmed = 0;

  log_printf("[langsel] open (current row %d, %s, %d fan translation%s)",
             current, have_key ? "L held" : "no Language key yet",
             count - LANGSEL_BUILTIN, count - LANGSEL_BUILTIN == 1 ? "" : "s");

  for (;;) {
    memset(&pad, 0, sizeof pad);
    sceCtrlPeekBufferPositiveExt2(0, &pad, 1);
    unsigned edge = pad.buttons & ~prev;
    if (pad.buttons != prev) idle = sceKernelGetProcessTimeWide();
    prev = pad.buttons;

    if (edge & SCE_CTRL_UP)   sel = (sel + count - 1) % count;
    if (edge & SCE_CTRL_DOWN) sel = (sel + 1) % count;
    if (sel < top) top = sel;
    if (sel >= top + LANGSEL_VISIBLE_ROWS) top = sel - LANGSEL_VISIBLE_ROWS + 1;

    if (edge & btn_ok) { confirmed = 1; break; }
    if (edge & btn_cancel) break;

    /* Nothing touched at all for a minute means the pad is not reaching us,
     * and a picker nobody can dismiss is a console that will not boot. Carry
     * on with what the ini already said. */
    if (sceKernelGetProcessTimeWide() - idle >
        (uint64_t)LANGSEL_IDLE_TIMEOUT_S * 1000000u) {
      log_printf("[langsel] no input for %ds -- continuing",
                 LANGSEL_IDLE_TIMEOUT_S);
      break;
    }

    draw(rows, count, sel, top, footer);

    /* Whether vglSwapBuffers waits for vblank is vitaGL's business, not ours,
     * so the poll rate is pinned here: fast enough that a press never feels
     * dropped, slow enough that a static menu is not spinning the CPU. */
    sceKernelDelayThread(10000);
  }

  if (confirmed) {
    *out = sel;
    if (sel < LANGSEL_BUILTIN)
      log_printf("[langsel] chose %s (id %d)", ini_language_code(sel), sel);
    else
      log_printf("[langsel] chose fan translation row %d (%s)", sel, rows[sel]);
  } else {
    log_printf("[langsel] dismissed without choosing");
  }
  return confirmed;
}

#else   /* !LANGSEL_ENABLE */

void langsel_watch_begin(void) { }

int langsel_run(int current, int have_key, const char *const *extra, int nextra,
                int *out) {
  (void)current; (void)have_key; (void)extra; (void)nextra; (void)out;
  return 0;
}

#endif
