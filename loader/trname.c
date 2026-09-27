/* trname.c -- see trname.h. */
#include "trname.h"

/* One code point out of `s`, advancing it. A malformed or truncated sequence
 * consumes one byte and yields -1, so the caller can never loop forever. */
static long next_cp(const unsigned char **s) {
  const unsigned char *p = *s;
  unsigned c = p[0];
  int n;
  long cp;
  if (c < 0x80) { *s = p + 1; return (long)c; }
  if      ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
  else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
  else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
  else { *s = p + 1; return -1; }
  for (int i = 1; i <= n; i++) {
    if ((p[i] & 0xC0) != 0x80) { *s = p + 1; return -1; }
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  *s = p + 1 + n;
  return cp;
}

/* Latin-1 is CP1252 from 0xA0 up, so those map straight across. Upper-casing:
 * a-z, and a0-fe less the two that have no capital in the set (division sign,
 * and sharp s, which stays as it is). */
static int to_label_byte(long cp) {
  if (cp >= 'a' && cp <= 'z') return (int)(cp - 0x20);
  if (cp >= 0x20 && cp < 0x7F) return (int)cp;
  if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) return (int)(cp - 0x20);
  if (cp >= 0xA0 && cp <= 0xFF) return (int)cp;
  return '?';
}

int trname_label(const char *utf8, char *out, int outsz) {
  if (outsz <= 0) return 0;
  int n = 0;
  const unsigned char *s = (const unsigned char *)utf8;
  while (s && *s && n + 1 < outsz)
    out[n++] = (char)to_label_byte(next_cp(&s));
  out[n] = '\0';
  return n;
}

int trname_eq(const char *a, const char *b) {
  for (;; a++, b++) {
    int x = (unsigned char)*a, y = (unsigned char)*b;
    if (x >= 'A' && x <= 'Z') x += 0x20;
    if (y >= 'A' && y <= 'Z') y += 0x20;
    if (x != y) return 0;
    if (!x) return 1;
  }
}
