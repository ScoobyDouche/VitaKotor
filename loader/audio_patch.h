/* audio_patch.h -- stubbed audio backend (FMOD + OpenSLES)
 *
 * KOTOR's audio path is FModAudioSystem (in libandroid_port.so) -> FMOD
 * (libfmod.so) -> OpenSLES. During bring-up we resolve the whole surface to
 * no-op stubs so the game links and starts silently. Real audio (so-load
 * libfmod.so + an OpenSLES engine over sceAudio) is a later phase (see RECON.md).
 */

#ifndef __AUDIO_PATCH_H__
#define __AUDIO_PATCH_H__

#include "so_util.h"

// Number of entries appended by audio_get_dynlib().
extern const int audio_dynlib_size;

// Returns the stub table for FMOD/OpenSLES symbols so main can splice it into
// the resolver (kept separate to keep dynlib.c readable).
const so_default_dynlib *audio_get_dynlib(void);

#endif
