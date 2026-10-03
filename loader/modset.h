/* modset.h -- switchable mod sets, chosen from the main menu's Google Play
 * button.
 *
 * The Android build's Google Play button signs in to Play Games, which the
 * Vita has no use for. On the Vita it opens the mod menu instead: pressing it
 * restarts the game into a list of the folders under
 *
 *   ux0:data/kotor/mods/<Name>/
 *
 * plus VANILLA. Each folder is laid out like the card root -- override/,
 * modules/, lips/, streamwaves/, tv_dialog.tlk and so on -- and the active
 * set is laid over the card root: a file the set has wins, anything it does
 * not have comes from the card root (and then the OBB) as before, and the
 * game's listing of each of those directories is the two merged. So a set
 * holds only what its mods change, and the card's own files -- the 234
 * extracted .rim files in modules/ -- stay visible under every set. VANILLA is the
 * card root alone, as before mod sets existed.
 *
 * Saves, swkotor.ini and everything else not in kContent (modset.c) stay
 * shared between sets.
 *
 * The menu runs at boot, not in the menu itself: the game reads its resource
 * directories once, at startup, so a set chosen mid-session would not take
 * effect until the next one anyway, and the boot window is the only time the
 * loader owns the screen (see langsel.h).
 *
 * The choice is kept in ux0:data/kotor/mods/active.txt rather than in
 * swkotor.ini, which the engine rewrites whenever options are saved.
 */
#ifndef __MODSET_H__
#define __MODSET_H__

/* Most folders the menu will list; the rest are logged and skipped. */
#define MODSET_MAX 16

/* Look for mod-set folders and restore the saved choice. Call once, before the
 * game opens any file. Returns how many sets were found. */
int modset_scan(void);

int modset_count(void);

/* The folder name, and the CP1252 upper-case label the menu draws. NULL for an
 * out-of-range index. */
const char *modset_name(int i);
const char *modset_label(int i);

/* The active index, or -1 for vanilla. */
int modset_active(void);

/* Make set `i` active (-1 for vanilla) and save the choice to the card. */
void modset_choose(int i);

/* Whether the last run asked for the menu (the Google Play button). Reading it
 * clears the request, so the menu opens once per press. */
int modset_menu_requested(void);

/* Replace the Google Play button with "restart into the mod menu". */
void modset_install_button(void);

/* Map a card path the game asked for onto the active set. `tail` is the part
 * after ux0:data/kotor/, with or without a leading "./". Returns `out` when
 * the path is under a content name and the active set has that file or
 * directory, NULL when the card root's copy should be used. */
const char *modset_redirect(const char *tail, char *out, int outsz);

#endif
