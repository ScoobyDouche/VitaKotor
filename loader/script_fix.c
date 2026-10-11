/* script_fix.c -- see script_fix.h. */

#include <stdint.h>
#include <string.h>

#include "script_fix.h"

typedef struct {
  const char *name;
  size_t len;
  uint32_t crc;       /* CRC-32 of the original file */
  size_t at;          /* offset of the bytes to replace */
  unsigned char from[8];
  unsigned char to[8];
} ScriptFix;

/* CPTOPBP (0x27 0x01), BP offset big-endian, size 4: -612 -> -576. */
#define FLAG0_TO_FLAG9 \
  {0x27, 0x01, 0xff, 0xff, 0xfd, 0x9c, 0x00, 0x04}, \
  {0x27, 0x01, 0xff, 0xff, 0xfd, 0xc0, 0x00, 0x04}

static const ScriptFix g_fixes[] = {
  {"k_ptat_trysteal",  4079, 0x0db2d77d, 3316, FLAG0_TO_FLAG9},
  {"k_ptat_sandsteal", 3633, 0x7a39a87e, 3340, FLAG0_TO_FLAG9},
};

static uint32_t crc32(const unsigned char *p, size_t n) {
  uint32_t c = 0xffffffffu;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & -(c & 1));
  }
  return ~c;
}

const char *script_fix_apply(unsigned char *buf, size_t len) {
  if (!buf) return NULL;
  for (size_t i = 0; i < sizeof g_fixes / sizeof g_fixes[0]; i++) {
    const ScriptFix *f = &g_fixes[i];
    if (len != f->len || memcmp(buf + f->at, f->from, sizeof f->from) != 0) continue;
    if (crc32(buf, len) != f->crc) continue;
    memcpy(buf + f->at, f->to, sizeof f->to);
    return f->name;
  }
  return NULL;
}

#ifdef __vita__
#include <vitasdk.h>
#include "log.h"
#include "main.h"
#include "so_util.h"

/* CResNCS layout, from OnResourceServiced (+0x3e4640): it copies the raw
 * buffer [+16] and its size [+24] into the script's own pointer [+48] and
 * size [+44], and sets loaded [+40]. Slot 7 of _ZTV7CResNCS is that method. */
#define NCS_SIZE  11
#define NCS_DATA  12
#define VT_SERVICED 7

static int (*Serviced_orig)(void *self);

static int Serviced_hook(void *self) {
  int r = Serviced_orig(self);
  uint32_t *o = (uint32_t *)self;
  static int seen;
  if (!seen++ && o[NCS_DATA])  // the fixes assume the whole file, header included
    log_printf("[script] first script: %u bytes, starts \"%.8s\"", (unsigned)o[NCS_SIZE], (const char *)o[NCS_DATA]);
  const char *name = script_fix_apply((unsigned char *)o[NCS_DATA], o[NCS_SIZE]);
  if (name) log_printf("[script] patched %s: chest theft warning now uses area flag 9, not Griff's flag 0", name);
  return r;
}

void script_fix_install(void) {
  uintptr_t vt = so_symbol(&kotor_mod, "_ZTV7CResNCS");
  uintptr_t fn = so_symbol(&kotor_mod, "_ZN7CResNCS18OnResourceServicedEv");
  if (!vt || !fn) {
    log_printf("[script] CResNCS symbols missing -- script fixes NOT installed");
    return;
  }
  uintptr_t *slot = (uintptr_t *)vt + VT_SERVICED;
  if ((*slot & ~1u) != (fn & ~1u)) {
    log_printf("[script] CResNCS vtable slot %d is 0x%08x, expected 0x%08x -- script fixes NOT installed",
               VT_SERVICED, (unsigned)*slot, (unsigned)fn);
    return;
  }
  Serviced_orig = (int (*)(void *))*slot;
  *slot = (uintptr_t)&Serviced_hook;
  log_printf("[script] CResNCS::OnResourceServiced hooked via vtable (Griff spawn fix armed)");
}
#endif
