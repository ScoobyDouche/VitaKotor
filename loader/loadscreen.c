/* loadscreen.c -- see loadscreen.h. */

#include "loadscreen.h"
#include "config.h"
#include "log.h"

#include <vitasdk.h>
#include <vitaGL.h>

#define TIM_PATH  DATA_PATH "/startup.tim"
#define TIM_MAGIC 0x314D4954u    /* "TIM1" */

typedef struct { unsigned magic; unsigned warm; unsigned us_warm, us_cold; } Tim;

static SceUID   g_owner  = -1;   /* only this thread may draw */
static int      g_active = 0;
static int      g_warm   = 0;
static int      g_busy   = 0;    /* reentrancy: draw() runs GL, GL ticks us */
static uint64_t g_t0     = 0;
static uint64_t g_last   = 0;
static uint64_t g_expect = 0;    /* us this boot is predicted to take */

/* Filled rectangles via scissor+clear: no shaders, no buffers, no textures --
 * nothing to allocate before the game has set up its own GL state, and nothing
 * to tear down when we hand over. */
static void fill(int x, int y, int w, int h, float r, float g, float b) {
  glEnable(GL_SCISSOR_TEST);
  glScissor(x, y, w, h);
  glClearColor(r, g, b, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
}

#define BAR_W 600
#define BAR_H 12
#define BAR_X ((SCREEN_W - BAR_W) / 2)
#define BAR_Y ((SCREEN_H - BAR_H) / 2)

static void draw(float frac) {
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;

  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.05f, 0.06f, 0.09f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  fill(BAR_X - 2, BAR_Y - 2, BAR_W + 4, BAR_H + 4, 0.16f, 0.17f, 0.20f);
  fill(BAR_X, BAR_Y, BAR_W, BAR_H, 0.09f, 0.10f, 0.13f);
  int w = (int)(BAR_W * frac);
  if (w > 0) fill(BAR_X, BAR_Y, w, BAR_H, 0.79f, 0.64f, 0.15f);

  glDisable(GL_SCISSOR_TEST);
  vglSwapBuffers(GL_FALSE);
}

/* ---- persisted duration estimate ------------------------------------------ */

static void tim_read(Tim *t) {
  t->magic = TIM_MAGIC; t->warm = 0;
  t->us_warm = LOADSCREEN_DEFAULT_WARM_S * 1000000u;
  t->us_cold = LOADSCREEN_DEFAULT_COLD_S * 1000000u;
  SceUID fd = sceIoOpen(TIM_PATH, SCE_O_RDONLY, 0);
  if (fd < 0) return;
  Tim d;
  if (sceIoRead(fd, &d, sizeof d) == (int)sizeof d && d.magic == TIM_MAGIC) {
    if (d.us_warm > 1000000u && d.us_warm < 600000000u) t->us_warm = d.us_warm;
    if (d.us_cold > 1000000u && d.us_cold < 600000000u) t->us_cold = d.us_cold;
  }
  sceIoClose(fd);
}

static void tim_write(unsigned us) {
  Tim t;
  tim_read(&t);
  if (g_warm) t.us_warm = us; else t.us_cold = us;
  t.magic = TIM_MAGIC;
  SceUID fd = sceIoOpen(TIM_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0) return;
  sceIoWrite(fd, &t, sizeof t);
  sceIoClose(fd);
}

/* ---- public ---------------------------------------------------------------- */

void loadscreen_begin(int warm) {
#if LOADSCREEN_ENABLE
  Tim t; tim_read(&t);
  g_warm   = warm ? 1 : 0;
  g_expect = warm ? t.us_warm : t.us_cold;
  g_owner  = sceKernelGetThreadId();
  g_t0     = sceKernelGetProcessTimeWide();
  g_last   = 0;
  g_active = 1;
  log_printf("[loadscreen] on (thread 0x%x, %s cache, expecting %us)",
             (unsigned)g_owner, warm ? "warm" : "cold", (unsigned)(g_expect / 1000000));
  g_busy = 1; draw(0.0f); g_busy = 0;
#else
  (void)warm;
#endif
}

int loadscreen_active(void) { return g_active; }

void loadscreen_tick(void) {
#if LOADSCREEN_ENABLE
  if (!g_active || g_busy) return;
  if (sceKernelGetThreadId() != g_owner) return;    /* never GL off-thread */

  uint64_t now = sceKernelGetProcessTimeWide();
  if (now - g_last < LOADSCREEN_REDRAW_MS * 1000) return;
  g_last = now;

  /* Clamp below full: the estimate is from the previous boot and this one may
   * be slower. A bar that creeps to 99% and waits is honest; one that reads
   * 100% while the game is still loading is exactly the bug being fixed. */
  float f = g_expect ? (float)(now - g_t0) / (float)g_expect : 0.0f;
  if (f > 0.99f) f = 0.99f;

  g_busy = 1; draw(f); g_busy = 0;
#endif
}

void loadscreen_end(void) {
#if LOADSCREEN_ENABLE
  if (!g_active) return;
  g_active = 0;
  if (sceKernelGetThreadId() != g_owner) return;

  unsigned took = (unsigned)(sceKernelGetProcessTimeWide() - g_t0);
  g_busy = 1;
  draw(1.0f);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  g_busy = 0;

  tim_write(took);
  log_printf("[loadscreen] off after %u.%us (%s cache) -- estimate saved for next boot",
             took / 1000000, (took / 100000) % 10, g_warm ? "warm" : "cold");
#endif
}
