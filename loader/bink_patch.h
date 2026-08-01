/* bink_patch.h -- stub Bink video
 *
 * The Bink decode/draw entry points (MacPlayBinkGL, MacCreateBinkShaders) are
 * implemented *inside* libandroid_port.so. During bring-up we hook them in the
 * loaded companion module so cutscenes are skipped rather than driving an
 * unported codec path. Real playback is a later phase.
 *
 * MacDecompress is NOT one of them despite the shared `Mac` prefix -- it is the
 * OBB's LZMA resource decompressor and must be left alone. See bink_patch.c.
 */

#ifndef __BINK_PATCH_H__
#define __BINK_PATCH_H__

#include "so_util.h"

// Hook the companion module's Bink exports to no-op stubs.
void bink_patch(so_module *port_mod);

#endif
