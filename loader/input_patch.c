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
  SDL_PushEvent(&e);
}

void input_touch_pump(void) {
  /* SDL's video init runs AFTER input_touch_init and re-enables both ports, so
   * assert this again once we are definitely past it. Cheap: one syscall, once. */
  static int back_off_again = 0;
  if (!back_off_again && ++back_off_again)
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_STOP);

  if (!s_ready)
    return;

  SceTouchData td;
  int n = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
  int touching = (n >= 0 && td.reportNum > 0);

  // log144 spent its whole budget in the first five minutes of a 31-minute
  // session, and log143 -- where the chargen name screen would not advance --
  // recorded no touch at all, leaving "the panel reported nothing" and "the
  // user never touched it" indistinguishable. At 0.27 ms a line, 400 is still
  // noise next to the GL trace, and taps are the one input we can prove.
  static int log_budget = 400;

  if (touching) {
    float nx = ((float)td.report[0].x - s_min_x) / (s_max_x - s_min_x);
    float ny = ((float)td.report[0].y - s_min_y) / (s_max_y - s_min_y);
    if (nx < 0.0f) nx = 0.0f; else if (nx > 1.0f) nx = 1.0f;
    if (ny < 0.0f) ny = 0.0f; else if (ny > 1.0f) ny = 1.0f;

    Uint32 type = s_finger_down ? SDL_FINGERMOTION : SDL_FINGERDOWN;
    push_finger(type, nx, ny);
    if (!s_finger_down && log_budget > 0) {
      log_budget--;
      log_printf("[touch] DOWN raw=(%d,%d) norm=(%.3f,%.3f) -> SDL_FINGERDOWN",
                 (int)td.report[0].x, (int)td.report[0].y, nx, ny);
    }
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
