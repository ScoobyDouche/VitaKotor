/* test_script_fix.c -- host tests for the in-memory .ncs patches. Not built
 * into the VPK.
 *
 * Needs the original scripts, which the repo can't carry: pass
 * k_ptat_trysteal.ncs and k_ptat_sandsteal.ncs (from main.obb
 * modules/tat_m20aa_s.rim), then any number of other .ncs files that must come
 * through untouched.
 *
 * Build and run:
 *   gcc -std=c99 -Wall -Wextra -Werror -I loader \
 *       -o /tmp/test_script_fix tools/test_script_fix.c loader/script_fix.c \
 *   && /tmp/test_script_fix trysteal.ncs sandsteal.ncs other.ncs...
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script_fix.h"

static unsigned char *slurp(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) { printf("FAIL: cannot open %s\n", path); exit(1); }
  fseek(f, 0, SEEK_END);
  *len = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *b = malloc(*len);
  assert(fread(b, 1, *len, f) == *len);
  fclose(f);
  return b;
}

static void broken(const char *path, const char *want, size_t at) {
  size_t len;
  unsigned char *b = slurp(path, &len);
  unsigned char *orig = malloc(len);
  memcpy(orig, b, len);

  const char *got = script_fix_apply(b, len);
  if (!got || strcmp(got, want) != 0) {
    printf("FAIL: %s patched as %s, want %s\n", path, got ? got : "(none)", want);
    exit(1);
  }
  /* Exactly one byte moves: the BP offset's low byte, 0x9c (-612) -> 0xc0 (-576). */
  for (size_t i = 0; i < len; i++) {
    if (i == at + 5) { assert(orig[i] == 0x9c && b[i] == 0xc0); continue; }
    if (b[i] != orig[i]) { printf("FAIL: %s byte %zu changed\n", path, i); exit(1); }
  }
  /* A second load of the same (cached) buffer must not patch again. */
  assert(script_fix_apply(b, len) == NULL);
  /* One byte off anywhere and the CRC refuses it (a modded script). */
  memcpy(b, orig, len);
  b[len / 2] ^= 1;
  assert(script_fix_apply(b, len) == NULL);
  free(orig);
  free(b);
  printf("ok: %s\n", want);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    printf("usage: %s k_ptat_trysteal.ncs k_ptat_sandsteal.ncs [other.ncs...]\n", argv[0]);
    return 2;
  }
  broken(argv[1], "k_ptat_trysteal", 3316);
  broken(argv[2], "k_ptat_sandsteal", 3340);
  assert(script_fix_apply(NULL, 0) == NULL);
  for (int i = 3; i < argc; i++) {
    size_t len;
    unsigned char *b = slurp(argv[i], &len);
    if (script_fix_apply(b, len)) { printf("FAIL: %s was patched\n", argv[i]); return 1; }
    free(b);
  }
  printf("ok: %d other scripts untouched\n", argc - 3);
  printf("all script_fix tests passed\n");
  return 0;
}
