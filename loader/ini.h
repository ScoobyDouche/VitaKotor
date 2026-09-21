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
/* Write `text` back out with `key` set to `value` inside `[section]`, into
 * `out` (always NUL-terminated when outsz > 0).
 *
 * Returns the length the result needs, excluding the NUL, the way snprintf
 * does: a return >= outsz means the text was truncated and MUST NOT be written
 * to the card. Pass outsz 0 to size the buffer without writing anything.
 *
 * The file is the user's, so everything outside the one line that changes
 * comes back byte for byte -- comments, section order, spacing, and the key's
 * own spelling and indentation. Only a stale inline comment on the rewritten
 * line goes, since it describes the value being replaced.
 *
 * An existing key is replaced in place; an existing section without the key
 * gains it directly under the header; a missing section is appended. Line
 * endings follow whatever the file already uses, defaulting to CRLF -- what
 * the engine writes -- when there is nothing to copy. */
size_t ini_set(const char *text, const char *section, const char *key,
               const char *value, char *out, size_t outsz);

int ini_language_id(const char *code);

/* The inverse: the code to write back for an id, for anything that has picked
 * a language and now has to say so in the file. Never NULL -- an id outside
 * 1..4 is English, matching ini_language_id's own fallback. */
const char *ini_language_code(int id);

#endif
