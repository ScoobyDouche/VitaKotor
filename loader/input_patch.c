/* input_patch.c -- synthesize SDL touch events from the Vita front panel.
 *
 * The game is touch-driven: its menu/input loops poll SDL_PollEvent and act on
 * SDL_FINGERDOWN/MOTION/UP (0x700/0x701/0x702) -- confirmed by disassembly of
 * libandroid_port (MacPlayBinkGL's skip loop compares event.type to 0x700).
 * vitasdk SDL2, however, delivers the pad as a joystick but produces NO finger
 * (or mouse) events from the front panel in our configuration -- we log every
 * event returned to the game and saw zero. So the menu never gets a tap.
 *
 * Rather than debug SDL's internal touch backend, we read the front panel with
 * sceTouchPeek each frame and push proper SDL_TouchFingerEvents into SDL's queue
 * with SDL_PushEvent. Whoever polls (game or port layer) then sees real finger
 * events. We track one finger (report 0) and emit DOWN on first contact, MOTION
 * while it persists, UP on release.
 *
 * Coordinates: the front panel reports x in [0,1919], y in [0,1087]; SDL finger
 * events are normalized [0,1]. We divide by the panel maximum from
 * sceTouchGetPanelInfo (falling back to the standard 1919x1087). If the menu
 * still ignores taps, the per-event log line prints raw + normalized so we can
 * see whether the engine wants a different coordinate space.
 */

#include <vitasdk.h>
#include <SDL2/SDL.h>
#include <string.h>

#include "ime_patch.h"
#include "input_patch.h"
#include "log.h"

static int   s_ready = 0;
static float s_min_x = 0.0f;      // front-panel ACTIVE-AREA bounds, in the same
static float s_min_y = 0.0f;      // units as report.x/y (from sceTouchGetPanelInfo).
static float s_max_x = 1919.0f;   // Normalising by max alone silently assumes the
static float s_max_y = 1087.0f;   // area starts at 0 -> constant positional offset.
static int   s_finger_down = 0;   // previous-frame contact state
static Uint32 s_window_id = 0;    // focused window, for the event's windowID
static float s_last_x = 0.0f;     // last contact position, so FINGERUP can carry
static float s_last_y = 0.0f;     // it (SDL semantics; UI that acts on release
                                  // otherwise sees every tap at the top-left).
static int   s_finger_id = -1;    // panel report id of the finger being followed
static int   s_wait_clear = 0;    // followed finger lifted while others stayed:
                                  // ignore the panel until it is empty

void input_touch_init(void) {
  int r = sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
  /* We only ever read the FRONT panel, but the back panel is where your fingers
   * rest while holding the console -- and Vita SDL2's own touch backend turns
   * ANY sampling port into finger events, so the rear pad was firing taps into
   * the game. Nothing wants it; stop sampling it. */
  sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_STOP);

  SceTouchPanelInfo info;
  memset(&info, 0, sizeof(info));
  if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &info) >= 0) {
    // Active-area bounds, in the same units as report.x/y. Both ends matter:
    // using max alone assumes the area starts at 0 and shifts every coordinate.
    if (info.maxAaX > info.minAaX) { s_min_x = (float)info.minAaX; s_max_x = (float)info.maxAaX; }
    if (info.maxAaY > info.minAaY) { s_min_y = (float)info.minAaY; s_max_y = (float)info.maxAaY; }
    log_printf("[touch] panel aa=(%d,%d)-(%d,%d) disp=(%d,%d)-(%d,%d)",
               (int)info.minAaX, (int)info.minAaY, (int)info.maxAaX, (int)info.maxAaY,
               (int)info.minDispX, (int)info.minDispY, (int)info.maxDispX, (int)info.maxDispY);
  }

  // The finger event carries the window it happened over; grab the game's.
  SDL_Window *w = SDL_GetWindowFromID(1);           // our only window is id 1
  if (!w) w = SDL_GL_GetCurrentWindow();
  if (w) s_window_id = SDL_GetWindowID(w);

  s_ready = 1;
  log_printf("[touch] front-panel sampling %s (aa x %.0f..%.0f, y %.0f..%.0f, windowID=%u)",
             r >= 0 ? "started" : "FAILED", s_min_x, s_max_x, s_min_y, s_max_y,
             (unsigned)s_window_id);
}

/* Raw-hardware witnesses, reported by input_probe_census on the watchdog's
 * clock. log166 could not tell whether input died in the pad/panel, in SDL, or
 * in the game, because the only evidence was our own injection log (alive to the
 * last line) and the game's reaction (absent). These count what the HARDWARE is
 * doing in the same windows sdl_input_census covers the SDL queue, so the two
 * lines together localise the break to one of the three layers.
 *
 * SDL_PushEvent's return was previously discarded. It returns 0 when the queue
 * is full, which would drop every finger event while the game kept draining
 * buttons -- exactly the observed symptom -- and we had no way to see it. */
static unsigned s_frames = 0;        /* pump calls this window */
static unsigned s_rstick_frames = 0; /* ... with the right stick off centre */
static unsigned s_rstick_max = 0;    /* ... peak deflection, 0..127 */
static unsigned s_touch_frames = 0;  /* ... with a finger on the panel */
static unsigned s_rear_frames = 0;   /* ... with a finger on the REAR panel:
                                      * nonzero means someone (reVita) turned
                                      * rear sampling back on behind us */
static unsigned s_pushed = 0;        /* finger events handed to SDL */
static unsigned s_push_fail = 0;     /* ... that SDL refused (queue full) */

static void input_stick_census(void);

void input_probe_census(void) {
  log_printf("[input] raw: %u frames, rstick off-centre %u (peak %u/127), "
             "touching %u, rear %u; pushed %u finger events, %u REFUSED by SDL",
             s_frames, s_rstick_frames, s_rstick_max, s_touch_frames,
             s_rear_frames, s_pushed, s_push_fail);
  s_frames = s_rstick_frames = s_rstick_max = s_touch_frames = 0;
  s_rear_frames = 0;
  s_pushed = s_push_fail = 0;
  input_stick_census();
}

/* Read the right stick straight from the pad. main() selects ANALOG_WIDE via
 * sceCtrlSetSamplingModeExt, so the Ext2 reader is the one that sees analog. */
static void sample_rstick(void) {
  SceCtrlData pad;
  memset(&pad, 0, sizeof(pad));
  if (sceCtrlPeekBufferPositiveExt2(0, &pad, 1) <= 0) return;
  int dx = (int)pad.rx - 128, dy = (int)pad.ry - 128;
  if (dx < 0) dx = -dx;
  if (dy < 0) dy = -dy;
  unsigned d = (unsigned)(dx > dy ? dx : dy);
  /* 32/127 is well outside stick slop and well inside a deliberate push. */
  if (d > 32) {
    s_rstick_frames++;
    if (d > s_rstick_max) s_rstick_max = d;
  }
}

/* log215: jerky sticks with smooth touch steering. SDL's Vita backend reads the
 * pad with sceCtrlPeekBufferPositive2 -- NOT the Ext2 reader above, and the two
 * have separate sampling modes -- so read the stick both ways and count frames
 * where a stick snaps between pushed (>48) and centred (<16) from one frame to
 * the next. A thumb never does that at 30 fps; a reader that alternates does. */
static struct { unsigned pushed, snaps; int was; } s_stk[2][2]; /* [reader][stick] */

static unsigned stick_dev(unsigned char x, unsigned char y) {
  int dx = (int)x - 128, dy = (int)y - 128;
  if (dx < 0) dx = -dx;
  if (dy < 0) dy = -dy;
  return (unsigned)(dx > dy ? dx : dy);
}

static void stick_track(int r, int k, unsigned d) {
  int now = d > 48 ? 1 : (d < 16 ? -1 : 0);
  if (now == 1) s_stk[r][k].pushed++;
  if (now && s_stk[r][k].was && now != s_stk[r][k].was) s_stk[r][k].snaps++;
  if (now) s_stk[r][k].was = now;
}

static void sample_sticks(void) {
  SceCtrlData pad;
  for (int r = 0; r < 2; r++) {
    memset(&pad, 0, sizeof(pad));
    int got = r ? sceCtrlPeekBufferPositive2(0, &pad, 1)
                : sceCtrlPeekBufferPositiveExt2(0, &pad, 1);
    if (got <= 0) continue;
    stick_track(r, 0, stick_dev(pad.lx, pad.ly));
    stick_track(r, 1, stick_dev(pad.rx, pad.ry));
  }
}

static void input_stick_census(void) {
  log_printf("[input] sticks: Ext2 L pushed %u snaps %u, R pushed %u snaps %u | "
             "Pos2(SDL) L pushed %u snaps %u, R pushed %u snaps %u",
             s_stk[0][0].pushed, s_stk[0][0].snaps, s_stk[0][1].pushed, s_stk[0][1].snaps,
             s_stk[1][0].pushed, s_stk[1][0].snaps, s_stk[1][1].pushed, s_stk[1][1].snaps);
  for (int r = 0; r < 2; r++)
    for (int k = 0; k < 2; k++)
      s_stk[r][k].pushed = s_stk[r][k].snaps = 0;
}

static void push_finger(Uint32 type, float nx, float ny) {
  SDL_Event e;
  memset(&e, 0, sizeof(e));
  e.tfinger.type = type;
  e.tfinger.timestamp = SDL_GetTicks();
  e.tfinger.touchId = 0;
  e.tfinger.fingerId = 0;
  e.tfinger.x = nx;
  e.tfinger.y = ny;
  e.tfinger.dx = 0.0f;
  e.tfinger.dy = 0.0f;
  e.tfinger.pressure = (type == SDL_FINGERUP) ? 0.0f : 1.0f;
  e.tfinger.windowID = s_window_id;
  s_pushed++;
  if (SDL_PushEvent(&e) <= 0) s_push_fail++;
}

void input_touch_pump(void) {
  /* SDL's video init runs AFTER input_touch_init and re-enables both ports, so
   * assert this again once we are definitely past it. Cheap: one syscall, once. */
  static int back_off_again = 0;
  if (!back_off_again && ++back_off_again)
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_STOP);

  if (!s_ready)
    return;

  /* Sampled before the IME early-out: the stick keeps meaning something to the
   * game whether or not the keyboard owns the panel. */
  s_frames++;
  sample_rstick();
  sample_sticks();
  SceTouchData rear;
  if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &rear, 1) > 0 && rear.reportNum > 0)
    s_rear_frames++;

  /* While the on-screen keyboard is up, the panel belongs to it: the taps are
   * aimed at its keys, and forwarding them would also press whatever GUI
   * control sits underneath. Release any finger the game still thinks is down
   * -- a DOWN with no matching UP leaves controls stuck in their pressed
   * state -- then stay quiet until the dialog closes. */
  if (ime_dialog_active()) {
    if (s_finger_down) {
      push_finger(SDL_FINGERUP, s_last_x, s_last_y);
      s_finger_down = 0;
    }
    return;
  }

  SceTouchData td;
  int n = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
  int touching = (n >= 0 && td.reportNum > 0);
  if (touching) s_touch_frames++;

  /* Follow ONE finger by its report id. report[0] is just whichever contact
   * the panel lists first: with a thumb resting on the screen and a tap
   * elsewhere, lifting the thumb used to turn report[0] into the other finger,
   * and the game saw the one finger it knows about jump across the screen in
   * a single MOTION -- a drag, or a tap somewhere it was never aimed. When the
   * followed finger lifts, release it where it was, and leave any finger still
   * resting there alone until the panel is clear: picking it up would press
   * whatever sits under a thumb that never meant to tap. */
  int ri = 0;
  if (!touching) {
    s_wait_clear = 0;
  } else if (s_finger_down) {
    ri = -1;
    for (int k = 0; k < td.reportNum && k < SCE_TOUCH_MAX_REPORT; k++)
      if (td.report[k].id == s_finger_id) { ri = k; break; }
    if (ri < 0) { touching = 0; s_wait_clear = 1; }
  } else if (s_wait_clear) {
    touching = 0;
  }

  // log144 spent its whole budget in the first five minutes of a 31-minute
  // session, and log143 -- where the chargen name screen would not advance --
  // recorded no touch at all, leaving "the panel reported nothing" and "the
  // user never touched it" indistinguishable. At 0.27 ms a line, 400 is still
  // noise next to the GL trace, and taps are the one input we can prove.
  static int log_budget = 400;

  if (touching) {
    float nx = ((float)td.report[ri].x - s_min_x) / (s_max_x - s_min_x);
    float ny = ((float)td.report[ri].y - s_min_y) / (s_max_y - s_min_y);
    if (nx < 0.0f) nx = 0.0f; else if (nx > 1.0f) nx = 1.0f;
    if (ny < 0.0f) ny = 0.0f; else if (ny > 1.0f) ny = 1.0f;

    Uint32 type = s_finger_down ? SDL_FINGERMOTION : SDL_FINGERDOWN;
    push_finger(type, nx, ny);
    if (!s_finger_down && log_budget > 0) {
      log_budget--;
      log_printf("[touch] DOWN raw=(%d,%d) norm=(%.3f,%.3f) -> SDL_FINGERDOWN",
                 (int)td.report[ri].x, (int)td.report[ri].y, nx, ny);
    }
    s_finger_id = td.report[ri].id;
    s_last_x = nx; s_last_y = ny;
    s_finger_down = 1;
  } else if (s_finger_down) {
    // Release at the LAST contact point, not (0,0): a UI that commits on release
    // would otherwise register every tap in the top-left corner.
    push_finger(SDL_FINGERUP, s_last_x, s_last_y);
    s_finger_down = 0;
    if (log_budget > 0) { log_budget--; log_printf("[touch] UP -> SDL_FINGERUP"); }
  }
}
