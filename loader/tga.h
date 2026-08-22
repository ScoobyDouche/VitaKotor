/* tga.h -- the small corner of the TGA format KOTOR's GUI art actually uses.
 *
 * Not a general decoder. The loading screens (load_*.tga) are type 2 --
 * uncompressed truecolour -- at 24 bits, and that is all this handles. The
 * logo and font atlas are type 10 (RLE) and 32-bit; support for those belongs
 * here too, but only once something draws them.
 *
 * Output is always RGBA8888 in top-down row order, whatever the file's origin
 * bit says, so callers never have to think about which way up a texture is. */

#ifndef __TGA_H__
#define __TGA_H__

/* Decode `len` bytes of TGA into a malloc'd RGBA8888 buffer the caller owns,
 * writing the dimensions to *w and *h. NULL if the data is not a TGA this
 * understands; the reason is logged. */
unsigned char *tga_decode(const void *data, unsigned len, int *w, int *h);

#endif
