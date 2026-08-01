/* gl_patch.h -- GLES2 -> vitaGL resolution table (see gl_patch.c) */

#ifndef __GL_PATCH_H__
#define __GL_PATCH_H__

#include "so_util.h"

const so_default_dynlib *gl_get_dynlib(void);
extern const int gl_dynlib_size;

/* Called once per presented frame from the SDL swap hook. Emits a periodic
 * summary (draws/clears since the last summary) so we can tell a live, advancing
 * render loop from one that is stuck repeating an identical frame. */
void gl_patch_on_swap(void);

/* Set around CAurGUIStringInternal::Draw (see main.c) so the GL layer logs the
 * draw calls the text path actually issues, with the texture bound at the time.
 * Text has metrics and an uploaded atlas yet renders nothing, so the open question
 * is whether glyph quads reach GL at all -- and if so, against which texture.
 * Scoped this way the trace stays tiny instead of drowning the log. */
extern int g_gl_text_draw;

#endif
