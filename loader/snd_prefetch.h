/* snd_prefetch.h -- decompress an area's sounds before the game asks for them.
 *
 * log-2026-11-15-perf-a: the recurring 80-360 ms frames in normal play are
 * CSWCSoundObject::AIUpdate starting an ambient sound, and all of that time is
 * LZMA decompressing its 150-630 KB file on the game thread (16 such stalls,
 * 1.75 s in total, in four minutes). The cache only helps a repeat.
 *
 * So remember, per module, which archive ranges turned out to be sounds, and
 * the next time that module loads decompress them on an idle core during the
 * loading screen. The list lives on the card and grows as areas are visited:
 * the first visit to an area still pays, every later one does not.
 */
#pragma once

#include <stddef.h>

typedef int (*snd_prefetch_decode_fn)(unsigned char *dest, size_t *destLen,
                                      const unsigned char *src, size_t *srcLen,
                                      const unsigned char *props, size_t propsSize);

/* Load the list and start the worker. `decode` is the real LzmaUncompress. */
void snd_prefetch_init(snd_prefetch_decode_fn decode);

/* An OBB read just landed `len` bytes from archive offset `off` in `buf`. */
void snd_prefetch_note_read(int which, long off, const void *buf, long len);

/* If this exact read is one of the current module's sounds, fill `buf` from
 * RAM and return 1; the card is not touched. */
int snd_prefetch_serve(int which, long off, void *buf, long len);

/* A cache miss whose output was audio: remember where its input came from. */
void snd_prefetch_learn(const unsigned char *src, size_t src_len, size_t dst_len,
                        const unsigned char *props, size_t props_size);

/* The game opened modules/<name>.rim: a load of that area has begun. */
void snd_prefetch_module(const char *path);

/* One line if anything new was learned since the last call. */
void snd_prefetch_report(void);
