/* ini.c -- see ini.h. */
#include "ini.h"

#include <string.h>

static int lower(int c) {
  return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int is_space(int c) {
  return c == ' ' || c == '\t';
}

/* Compare a span of the file [b,e) against a NUL-terminated name, ignoring
 * case. The span is not NUL-terminated -- we never copy the file's text just to
 * compare it. */
static int span_eq(const char *b, const char *e, const char *name) {
  while (b < e && *name) {
    if (lower((unsigned char)*b) != lower((unsigned char)*name)) return 0;
    b++;
    name++;
  }
  return b == e && *name == '\0';
}

static const char *skip_space(const char *p, const char *e) {
  while (p < e && is_space((unsigned char)*p)) p++;
  return p;
}

static const char *trim_back(const char *b, const char *e) {
  while (e > b && is_space((unsigned char)e[-1])) e--;
  return e;
}

int ini_get(const char *text, const char *section, const char *key,
            char *out, size_t outsz) {
  if (outsz > 0) out[0] = '\0';
  if (!text || !section || !key) return 0;

  int in_section = 0;
  const char *p = text;

  while (*p) {
    /* Split off one line; both CR and LF end it, so CRLF and LF files read
     * the same and a lone CR (old Mac saves) does not swallow the file. */
    const char *eol = p;
    while (*eol && *eol != '\n' && *eol != '\r') eol++;

    const char *b = skip_space(p, eol);
    const char *e = trim_back(b, eol);

    if (b < e && *b != ';' && *b != '#') {
      if (*b == '[') {
        /* A header ends the previous section whether or not it is ours, so a
         * key below [Sound Options] is never read as a [Game Options] key. */
        const char *close = b + 1;
        while (close < e && *close != ']') close++;
        in_section = span_eq(skip_space(b + 1, close),
                             trim_back(b + 1, close), section);
      } else if (in_section) {
        const char *eq = b;
        while (eq < e && *eq != '=') eq++;
        if (eq < e && span_eq(b, trim_back(b, eq), key)) {
          const char *v = skip_space(eq + 1, e);
          /* An inline comment is not part of the value: someone leaving
           * "fr ; was de" behind should still get "fr". */
          const char *ve = v;
          while (ve < e && *ve != ';' && *ve != '#') ve++;
          ve = trim_back(v, ve);
          if (outsz > 0) {
            size_t n = (size_t)(ve - v);
            if (n > outsz - 1) n = outsz - 1;
            memcpy(out, v, n);
            out[n] = '\0';
          }
          return 1;
        }
      }
    }

    p = eol;
    while (*p == '\n' || *p == '\r') p++;
  }
  return 0;
}

int ini_language_id(const char *code) {
  static const struct { const char *code; int id; } kCodes[] = {
    { "en", INI_LANG_EN }, { "fr", INI_LANG_FR }, { "it", INI_LANG_IT },
    { "de", INI_LANG_DE }, { "es", INI_LANG_ES },
  };
  if (!code) return INI_LANG_EN;
  for (size_t i = 0; i < sizeof kCodes / sizeof kCodes[0]; i++)
    if (span_eq(code, code + strlen(code), kCodes[i].code)) return kCodes[i].id;
  return INI_LANG_EN;
}
