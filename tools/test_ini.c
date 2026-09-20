/* test_ini.c -- host tests for the ini reader. Not built into the VPK.
 *
 * Build and run:
 *   gcc -std=c99 -Wall -Wextra -Werror -I loader \
 *       -o /tmp/test_ini tools/test_ini.c loader/ini.c \
 *   && /tmp/test_ini
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ini.h"

/* The shape swkotor.ini actually has on the card: CRLF, a header comment, and
 * several sections the engine writes back whenever it saves options. */
static const char *kRealish =
  "; Star Wars Knights of the Old Republic\r\n"
  "\r\n"
  "[Display Options]\r\n"
  "FullScreen=1\r\n"
  "Width=800\r\n"
  "\r\n"
  "[Game Options]\r\n"
  "Difficulty Level=1\r\n"
  "Language=fr\r\n"
  "Mouse Sensitivity=5\r\n"
  "\r\n"
  "[Sound Options]\r\n"
  "Disable Sound=0\r\n";

static void expect(const char *text, const char *section, const char *key,
                   int want_found, const char *want_value) {
  char got[64];
  int found = ini_get(text, section, key, got, sizeof got);
  if (found != want_found || strcmp(got, want_value) != 0) {
    printf("FAIL [%s] %s: found=%d \"%s\", wanted found=%d \"%s\"\n",
           section, key, found, got, want_found, want_value);
    assert(0);
  }
}

static void test_finds_key_in_its_section(void) {
  expect(kRealish, "Game Options", "Language", 1, "fr");
  expect(kRealish, "Sound Options", "Disable Sound", 1, "0");
  expect(kRealish, "Display Options", "Width", 1, "800");
}

static void test_missing_key_and_section(void) {
  expect(kRealish, "Game Options", "Nonsense", 0, "");
  expect(kRealish, "No Such Section", "Language", 0, "");
  expect("", "Game Options", "Language", 0, "");
}

/* The key exists, but under a different section -- the commonest way a hand
 * edit goes wrong, and it must read as absent rather than as a match. */
static void test_key_outside_its_section_is_not_found(void) {
  expect("[Sound Options]\nLanguage=fr\n", "Game Options", "Language", 0, "");
}

/* The engine's own reader is case-insensitive, and the file on the card is
 * "swKotor.ini" on some installs, so users capitalise unpredictably. */
static void test_case_insensitive(void) {
  expect(kRealish, "game options", "language", 1, "fr");
  expect(kRealish, "GAME OPTIONS", "LANGUAGE", 1, "fr");
}

static void test_whitespace_is_stripped(void) {
  expect("[Game Options]\n  Language   =   de   \n", "Game Options",
         "Language", 1, "de");
  expect("  [Game Options]  \nLanguage=de\n", "Game Options", "Language", 1,
         "de");
}

static void test_comments_ignored(void) {
  expect("[Game Options]\n; Language=fr\nLanguage=it\n", "Game Options",
         "Language", 1, "it");
  expect("[Game Options]\n# Language=fr\nLanguage=it\n", "Game Options",
         "Language", 1, "it");
  expect("[Game Options]\nLanguage=it ; was fr\n", "Game Options", "Language",
         1, "it");
}

/* A key is only a key if the whole name matches: "Languages" must not answer a
 * request for "Language". */
static void test_prefix_is_not_a_match(void) {
  expect("[Game Options]\nLanguages=fr\n", "Game Options", "Language", 0, "");
  expect("[Game Options Extra]\nLanguage=fr\n", "Game Options", "Language", 0,
         "");
}

static void test_first_match_wins(void) {
  expect("[Game Options]\nLanguage=fr\nLanguage=de\n", "Game Options",
         "Language", 1, "fr");
}

static void test_empty_value(void) {
  expect("[Game Options]\nLanguage=\n", "Game Options", "Language", 1, "");
}

static void test_last_line_without_newline(void) {
  expect("[Game Options]\nLanguage=es", "Game Options", "Language", 1, "es");
}

/* A section runs until the next header, including across blank lines, and a
 * later repeat of the same header is still that section. */
static void test_section_repeated(void) {
  expect("[Game Options]\nFoo=1\n[Other]\nBar=2\n[Game Options]\nLanguage=de\n",
         "Game Options", "Language", 1, "de");
}

static void test_truncates_rather_than_overflows(void) {
  char got[4];
  int found = ini_get("[S]\nK=abcdefgh\n", "S", "K", got, sizeof got);
  assert(found == 1);
  assert(strcmp(got, "abc") == 0);
}

/* outsz 0 must not write anything at all -- there is nowhere to put the NUL. */
static void test_zero_sized_buffer(void) {
  char got[2] = { 'x', 'y' };
  (void)ini_get("[S]\nK=v\n", "S", "K", got, 0);
  assert(got[0] == 'x' && got[1] == 'y');
}

static void test_language_ids(void) {
  assert(ini_language_id("en") == INI_LANG_EN);
  assert(ini_language_id("fr") == INI_LANG_FR);
  assert(ini_language_id("it") == INI_LANG_IT);
  assert(ini_language_id("de") == INI_LANG_DE);
  assert(ini_language_id("es") == INI_LANG_ES);
}

static void test_language_ids_case_insensitive(void) {
  assert(ini_language_id("FR") == INI_LANG_FR);
  assert(ini_language_id("De") == INI_LANG_DE);
}

/* Everything unrecognised is English, because the engine's own out-of-range
 * fallback is English -- a typo must not cost the user their menu art. */
static void test_unknown_language_is_english(void) {
  assert(ini_language_id("") == INI_LANG_EN);
  assert(ini_language_id("xx") == INI_LANG_EN);
  assert(ini_language_id("french") == INI_LANG_EN);
  assert(ini_language_id("f") == INI_LANG_EN);
  assert(ini_language_id(NULL) == INI_LANG_EN);
}

int main(void) {
  test_finds_key_in_its_section();
  test_missing_key_and_section();
  test_key_outside_its_section_is_not_found();
  test_case_insensitive();
  test_whitespace_is_stripped();
  test_comments_ignored();
  test_prefix_is_not_a_match();
  test_first_match_wins();
  test_empty_value();
  test_last_line_without_newline();
  test_section_repeated();
  test_truncates_rather_than_overflows();
  test_zero_sized_buffer();
  test_language_ids();
  test_language_ids_case_insensitive();
  test_unknown_language_is_english();
  printf("test_ini: all tests passed\n");
  return 0;
}
