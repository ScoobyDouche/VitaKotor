/* trname.h -- the text half of fan-translation support (see translation.h).
 *
 * Folder names on the card are UTF-8; the picker draws with a CP1252 bitmap
 * font. This is the conversion between the two, plus the case-insensitive
 * name match the game's own file lookups use.
 *
 * Deliberately free of any VitaSDK dependency, like ini.c, so it compiles and
 * is tested on the host. See tools/test_trname.c.
 */
#ifndef TRNAME_H
#define TRNAME_H

/* Turn a UTF-8 folder name into an upper-case CP1252 label for the picker,
 * in `out` (always NUL-terminated when outsz > 0).
 *
 * Latin-1 letters keep their accents (the atlas has them); anything the font
 * cannot draw -- Cyrillic, CJK, a broken sequence -- becomes '?', so a label
 * is never empty or garbage. Returns the label's length. */
int trname_label(const char *utf8, char *out, int outsz);

/* ASCII case-insensitive equality: "tv_DIALOG.tlk" matches "tv_dialog.tlk". */
int trname_eq(const char *a, const char *b);

#endif
