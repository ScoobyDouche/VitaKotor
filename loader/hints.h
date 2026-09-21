/* hints.h -- the game's own loading-screen hints, for the boot screen.
 *
 * These are the lines the real loading screen shows under the progress bar.
 * Getting at them means walking the resource system by hand, because the boot
 * screen draws long before the game has mounted anything:
 *
 *   chitin.key      names every resource and says which archive holds it
 *   data/2da.bzf    a BIF whose entries are LZMA blobs; holds loadscreenhints
 *   loadscreenhints a binary 2DA of StrRefs, two columns: gameplay and story
 *   dialog.tlk      maps a StrRef to the actual text
 *
 * All four live in main.obb as STORED zip entries, so obbzip reads them
 * directly. The decompressor is passed in rather than looked up here: it is the
 * game's own libLzmaLib, already loaded by the time the boot screen runs, so
 * nothing new is linked in, and keeping the lookup at the call site leaves this
 * file pure parsing that can be exercised off the Vita.
 *
 * Two details that cost time to work out, both encoded here: a .bzf entry's
 * size field is the UNCOMPRESSED size, with the packed bytes running to the
 * next entry's offset; and its LZMA stream is five bytes of properties with the
 * usual eight-byte length field omitted.
 *
 * Which dialog.tlk is read is what makes the boot screen speak the language the
 * user picked: the archive carries all five (dialog.tlk, dialogfr.tlk,
 * dialogit.tlk, dialogde.tlk, dialoges.tlk) and the engine's own table picks
 * between them by the same id ini.h defines. The word "LOADING" comes from the
 * same file for the same reason -- it is StrRef 42493 in every one of them, so
 * the boot screen never has to carry a translation this port invented.
 *
 * Every failure is silent and total: no hints, and the boot screen simply shows
 * its own lines instead. */

#ifndef __HINTS_H__
#define __HINTS_H__

#include <stddef.h>

/* libLzmaLib's entry point: a raw stream, explicit property bytes, and an
 * output size the caller already knows. */
typedef int (*LzmaUncompressFn)(unsigned char *dest, size_t *destLen,
                                const unsigned char *src, size_t *srcLen,
                                const unsigned char *props, size_t propsSize);

/* Read the hints out of main.obb, decompressing with `lzma`, in the language
 * `lang` (an INI_LANG_* id). Returns how many were loaded, 0 on any failure.
 * Call once, early; costs a few hundred milliseconds of card I/O.
 *
 * A language whose table is missing falls back to English rather than leaving
 * the screen blank. */
int hints_load(LzmaUncompressFn lzma, int lang);

/* The game's own word for "Loading", in whatever language hints_load() read,
 * or NULL if it could not be resolved. Valid until hints_free(). */
const char *hints_loading(void);

/* How many hints are available, and one of them. hints_get() returns NULL for
 * an out-of-range index. */
int hints_count(void);
const char *hints_get(int i);

/* Drop the text. Every pointer from hints_get() dangles afterwards. */
void hints_free(void);

#endif
