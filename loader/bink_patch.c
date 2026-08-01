/* bink_patch.c -- stub Bink video (see bink_patch.h) */

#include <vitasdk.h>
#include <stdint.h>
#include "bink_patch.h"
#include "so_util.h"
#include "log.h"

// MacPlayBinkGL(const char *path, bool a, bool &finished, int c):
// report the movie as finished immediately so callers advance past it.
static int MacPlayBinkGL_stub(const char *path, int a, int *finished, int c) {
  log_printf("[BINK] skip movie: %s", path ? path : "(null)");
  if (finished)
    *finished = 1;
  return 0;
}

static int bink_stub(void) {
  return 0;
}

void bink_patch(so_module *port_mod) {
  if (!port_mod)
    return;
  // Redirect the companion's Bink exports to our stubs.
  hook_addr(so_symbol(port_mod, "_Z13MacPlayBinkGLPKcbRbi"),  (uintptr_t)&MacPlayBinkGL_stub);
  hook_addr(so_symbol(port_mod, "_Z20MacCreateBinkShadersv"), (uintptr_t)&bink_stub);

  // DO NOT stub MacDecompress. The `Mac` prefix makes it look like a sibling of
  // MacPlayBinkGL/MacCreateBinkShaders, and bring-up stubbed it on that basis --
  // it is actually the OBB's *resource* decompressor, and stubbing it silently
  // broke every LZMA-compressed asset in the game:
  //   MacDecompress(dest, destLen, src, srcLen)  @ libandroid_port+0x57244
  //     -> LzmaUncompress(dest, &destLen, src+5, &srcLen-5, props=src, 5)
  //     -> returns 1 on success, 0 on failure
  // `data/*.bzf` in the OBB are LZMA-compressed BIFs (5-byte props + stream per
  // entry), so every .bzf resource routes
  // through here. bink_stub returns 0 (= failure) and never touches `dest`, so
  // the resource manager handed the game a correctly-sized, correctly-tagged
  // block still holding newlib free-list bytes (`10 40 40 81 ...`). That is why
  // chargen's models were NULL: IODispatcher::ReadSync rejects the .mdl because
  // byte 0 of that garbage is not the required 0x00. Only assets served from
  // uncompressed RIM/module containers (gui3D_room, mainmenu) survived, which is
 // why the main menu looked fine.
}
