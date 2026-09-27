/* translation.h -- fan translations dropped onto the memory card.
 *
 * The game only knows five languages (INI_LANG_*), and each one is a text
 * table inside patch.obb. A fan translation is a replacement for one of those
 * tables, so it needs no language id of its own: the game is told "English"
 * and the English table it then asks for -- tv_dialog.tlk -- is served from
 * the translation's folder instead of the OBB.
 *
 * Each translation is one folder:
 *
 *   ux0:data/kotor/translations/<Name>/tv_dialog.tlk
 *
 * The folder name is what the language picker shows. A folder counts only if
 * it holds tv_dialog.tlk, or dialog.tlk (the PC table, which is what most fan
 * translations are made from) -- nothing else is needed to appear in the list.
 *
 * Every other file in the active folder overrides the game file of the same
 * name, for as long as the game opens it as a plain file (SDL_RWFromFile).
 * That is how a translation brings its own fonts, if the game reads them
 * through that door.
 */
#ifndef __TRANSLATION_H__
#define __TRANSLATION_H__

/* Most folders the picker will list; the rest are logged and skipped. */
#define TRANSLATION_MAX 16

/* Look for translation folders on the card. Call once, before anything asks
 * about them; returns how many were found. */
int translation_scan(void);

int translation_count(void);

/* The folder name (UTF-8, as on the card), and the CP1252 upper-case label the
 * picker draws. NULL for an out-of-range index. */
const char *translation_name(int i);
const char *translation_label(int i);

/* Index of the folder called `name` (case-insensitive), or -1. */
int translation_find(const char *name);

/* Make folder `i` the active translation, or -1 for none. */
void translation_select(int i);

/* The active index, or -1. */
int translation_active(void);

/* Where the active translation keeps `base` (a file name without directory,
 * matched case-insensitively), written into `out`; NULL when there is no
 * active translation or it has no such file. A request for tv_dialog.tlk
 * falls back to the folder's dialog.tlk. */
const char *translation_path(const char *base, char *out, int outsz);

#endif
