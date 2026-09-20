/* ini.h -- read one key out of an ini file's text.
 *
 * The game keeps its settings in ux0:data/kotor/swkotor.ini, which is also the
 * only file a user can edit without rebuilding the VPK. That makes it the right
 * home for the handful of things the port has to decide before the game starts
 * -- currently the language, which on Android comes from the device locale over
 * JNI and so has no source at all on the Vita (see jni_set_language).
 *
 * The engine ignores keys it does not recognise, so adding our own to a section
 * it already owns is safe and survives the game rewriting the file.
 *
 * Deliberately free of any VitaSDK dependency: the caller supplies the file's
 * text, so this file compiles and is tested on the host. See tools/test_ini.c.
 */
#ifndef INI_H
#define INI_H

#include <stddef.h>

/* Copy the value of `key` inside `[section]` into `out` (always NUL-terminated
 * when outsz > 0). Section and key match case-insensitively, as the engine's
 * own reader does; surrounding whitespace is stripped from both the key and the
 * value, and a ';' or '#' starts a comment.
 *
 * Returns 1 if the key was found, 0 otherwise (out is set to "" on 0). A value
 * longer than outsz-1 is truncated and still reported as found. */
int ini_get(const char *text, const char *section, const char *key,
            char *out, size_t outsz);

/* Map a two-letter language code to the id the game expects.
 *
 * libandroid_port's ASLPlat_GetCurrentLanguage() returns this int, and the
 * engine indexes two parallel tables with it -- the main-menu art suffix
 * (_fr/_it/_de/_es, SetIosButtonImage) and the dialogue table (HD0:DIALOGFR and
 * friends). Both treat anything outside 1..4 as English, which is why an
 * unknown code maps to 0 rather than failing: a typo gets you English, not a
 * missing-resource crash. */
#define INI_LANG_EN 0
#define INI_LANG_FR 1
#define INI_LANG_IT 2
#define INI_LANG_DE 3
#define INI_LANG_ES 4
int ini_language_id(const char *code);

#endif
