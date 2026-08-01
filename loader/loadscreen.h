/* loadscreen.h -- progress bar covering the whole startup.
 *
 * The game draws nothing at all until its first draw call, which log124 put at
 * 69.3s -- after even the "Main Menu" analytics string at 56.1s. So the console
 * shows a black screen for over a minute and the loader is the only thing that
 * can say otherwise.
 *
 * An earlier version tracked only the archive mount. Once the mount got fast
 * (95s -> 2s via the replay cache) that bar filled at 10s and then sat at 100%
 * for another 59 seconds, which is worse than showing nothing: it claims to be
 * finished when it is not. This version spans begin -> first game draw.
 *
 * Progress is elapsed time against how long the LAST boot took, persisted to
 * ux0:data/kotor/startup.tim, kept separately for a warm and a cold archive
 * cache because those differ by about a minute. It is an estimate and is
 * clamped below 100% until the handoff actually happens -- a bar that stalls
 * near the end is honest; one that sits full is not.
 *
 * Drawn from whichever thread called loadscreen_begin(), never a helper thread:
 * issuing GL from a second thread caused an earlier hang in this port. */

#ifndef __LOADSCREEN_H__
#define __LOADSCREEN_H__

/* Take over the screen. `warm` says whether the archive replay cache was
 * available, which selects the duration estimate. */
void loadscreen_begin(int warm);

/* Draw a frame if due. Cheap, throttled, safe to call from any hot path; it
 * no-ops off-thread, before begin, and after end. */
void loadscreen_tick(void);

/* Hand the screen to the game. Called from the first draw call. Records how
 * long this boot actually took, so the next one estimates better. */
void loadscreen_end(void);

/* 1 between begin and end. Lets the GL layer avoid work while we own the screen. */
int loadscreen_active(void);

#endif
