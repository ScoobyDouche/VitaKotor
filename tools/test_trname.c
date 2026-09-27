/* test_trname.c -- host tests for the translation-folder name helpers. Not
 * built into the VPK.
 *
 * Build and run:
 *   gcc -std=c99 -Wall -Wextra -Werror -I loader \
 *       -o /tmp/test_trname tools/test_trname.c loader/trname.c \
 *   && /tmp/test_trname
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "trname.h"

static void label(const char *in, const char *want) {
  char out[64];
  trname_label(in, out, sizeof out);
  if (strcmp(out, want) != 0) {
    printf("FAIL: label(\"%s\") = \"%s\", want \"%s\"\n", in, out, want);
    assert(0);
  }
}

int main(void) {
  /* Plain ASCII upper-cases and keeps punctuation. */
  label("Polski", "POLSKI");
  label("pt-BR v1.2", "PT-BR V1.2");

  /* Latin-1 keeps its accents, upper-cased into CP1252. */
  label("Portugu\xC3\xAAs", "PORTUGU\xCA" "S");       /* ê -> Ê */
  label("Espa\xC3\xB1ol", "ESPA\xD1" "OL");           /* ñ -> Ñ */
  label("Stra\xC3\x9F" "e", "STRA\xDF" "E");          /* ß has no capital */
  label("\xC3\xB7", "\xF7");                          /* ÷ is not a letter */

  /* Anything the atlas cannot draw is one '?' per character, not per byte. */
  label("\xD0\xA0\xD1\x83\xD1\x81", "???");           /* Рус */
  label("\xE4\xB8\xAD", "?");                         /* 中 */
  label("\xF0\x9F\x98\x80" "x", "?X");                /* emoji */

  /* Broken UTF-8 never runs off the end or yields an empty label. */
  label("a\xC3", "A?");
  label("\xC3" "b", "?B");
  label("\xFF", "?");

  /* Truncation always leaves a terminated string. */
  char small[4];
  assert(trname_label("abcdef", small, sizeof small) == 3);
  assert(strcmp(small, "ABC") == 0);
  assert(trname_label("abc", small, 0) == 0);

  assert(trname_eq("tv_DIALOG.tlk", "tv_dialog.tlk"));
  assert(trname_eq("", ""));
  assert(!trname_eq("dialog.tlk", "tv_dialog.tlk"));
  assert(!trname_eq("dialog.tl", "dialog.tlk"));

  printf("test_trname: all passed\n");
  return 0;
}
