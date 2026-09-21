/* langsel.h -- the boot-time language picker.
 *
 * The game decides its language once, at startup: ASLPlat_GetCurrentLanguage()
 * chooses both the main-menu art suffix and the dialogue table, and neither is
 * re-read afterwards. So there is no such thing as switching language from
 * inside the running game, and the game's own menus -- which are its data, not
 * ours -- have no row we could add one to. The only place a Vita user can be
 * asked is before the game starts, on a screen the loader draws itself.
 *
 * That screen is this file. It runs between vitaGL coming up and
 * loadscreen_begin(), which is the one window where the loader owns GL
 * outright and nothing is waiting on it: before it the font atlas cannot be
 * uploaded, and after it the time a user spends reading a menu would land in
 * the boot-duration estimate the progress bar persists and skew the next
 * boot's guess.
 *
 * It asks only when there is a reason to: the first run, when swkotor.ini has
 * no Language key at all, and afterwards only when L is held during startup.
 *
 * "During startup" has to mean the whole of it. The first version read the pad
 * once, at the instant the picker was reached -- several seconds in, with the
 * screen black and nothing to say when that instant was. Holding L through it
 * worked only by luck and tapping L never did. So the trigger is now latched by
 * langsel_watch_begin() from the moment the pad can be read at all: press or
 * hold L any time between launching the game and the picker appearing, and it
 * opens.
 * Everything else about it is deliberately inert -- no font, no picker; no
 * input for LANGSEL_IDLE_TIMEOUT_S, no picker -- because a menu that cannot be
 * read or dismissed on a first boot is a console that does not start.
 */
#ifndef __LANGSEL_H__
#define __LANGSEL_H__

/* Start watching for L, as early as the pad is sampleable -- main() calls this
 * right after it selects the sampling mode. Cheap: one thread that peeks the
 * pad every LANGSEL_WATCH_MS and latches whether L has been down at any point.
 * langsel_run() consumes the latch and stops the watcher. */
void langsel_watch_begin(void);

/* Offer the picker. `current` is the language id the ini resolved to and
 * `have_key` says whether it came from an actual Language key or from the
 * English default.
 *
 * Returns 1 when the user confirmed a choice, with the chosen id in *out;
 * 0 when the picker did not run, was cancelled, or timed out, leaving *out
 * untouched. A cancel on a first boot therefore saves nothing and the picker
 * asks again next time, which is the honest reading of "I did not choose". */
int langsel_run(int current, int have_key, int *out);

#endif
