/* tga.c -- see tga.h. */

#include "tga.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#define HDR 18

/* Same reasoning as obbzip's cap: a bad header should yield NULL, not a
 * multi-hundred-megabyte allocation during boot. */
#define MAX_DIM 4096

unsigned char *tga_decode(const void *data, unsigned len, int *w, int *h) {
  const unsigned char *p = data;
  if (!p || len < HDR) return NULL;

  unsigned id_len   = p[0];
  unsigned cmap     = p[1];
  unsigned type     = p[2];
  unsigned width    = p[12] | (p[13] << 8);
  unsigned height   = p[14] | (p[15] << 8);
  unsigned bpp      = p[16];
  unsigned desc     = p[17];

  if (type != 2 || cmap != 0) {
    log_printf("[tga] type %u cmap %u not supported", type, cmap);
    return NULL;
  }
  if (bpp != 24 && bpp != 32) {
    log_printf("[tga] %u bpp not supported", bpp);
    return NULL;
  }
  if (!width || !height || width > MAX_DIM || height > MAX_DIM) {
    log_printf("[tga] implausible size %ux%u", width, height);
    return NULL;
  }

  unsigned stride = width * (bpp / 8);
  unsigned need = HDR + id_len + stride * height;
  if (len < need) {
    log_printf("[tga] truncated: %u bytes, need %u", len, need);
    return NULL;
  }

  unsigned char *out = malloc((size_t)width * height * 4);
  if (!out) return NULL;

  const unsigned char *src = p + HDR + id_len;
  /* Bit 5 set means the first row stored is the top one. TGA's default is
   * bottom-up, which is what every KOTOR asset uses. */
  int top_down = (desc & 0x20) != 0;

  for (unsigned y = 0; y < height; y++) {
    const unsigned char *row = src + (size_t)(top_down ? y : height - 1 - y) * stride;
    unsigned char *dst = out + (size_t)y * width * 4;
    if (bpp == 24) {
      for (unsigned x = 0; x < width; x++) {
        dst[0] = row[2]; dst[1] = row[1]; dst[2] = row[0]; dst[3] = 0xff;
        dst += 4; row += 3;
      }
    } else {
      for (unsigned x = 0; x < width; x++) {
        dst[0] = row[2]; dst[1] = row[1]; dst[2] = row[0]; dst[3] = row[3];
        dst += 4; row += 4;
      }
    }
  }

  *w = (int)width;
  *h = (int)height;
  return out;
}
