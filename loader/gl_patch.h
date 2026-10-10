/* gl_patch.h -- GLES2 -> vitaGL resolution table (see gl_patch.c) */

#ifndef __GL_PATCH_H__
#define __GL_PATCH_H__

#include "so_util.h"

const so_default_dynlib *gl_get_dynlib(void);
extern const int gl_dynlib_size;

/* Called once per presented frame from the SDL swap hook. Emits a periodic
 * summary (draws/clears since the last summary) so we can tell a live, advancing
 * render loop from one that is stuck repeating an identical frame. */
void gl_patch_on_swap(uint64_t swap_begin_us, uint64_t swap_end_us);
/* When the last frame was presented; the hitch sampler measures from it. */
extern volatile uint64_t g_last_swap_end_us;

/* Scope for KOTOR's nested AurGUI viewport stack. */
extern int g_gl_gui_viewport_scope;

#endif
