/* geo_probe.h -- find out why world geometry spikes late in a long session.
 *
 * The spikes build up over minutes, persist until relaunch, and leave nothing
 * in the log. A vertex that lands far away is one of three things, and each
 * wants a different fix:
 *   (a) the buffer's bytes changed after upload with no GL call writing them
 *       (a memory stomp),
 *   (b) the bytes were already bad when the game uploaded them,
 *   (c) the bytes are fine but the GPU reads the wrong ones (an index past the
 *       end of the buffer, or attributes spread more than 64 KB apart, which
 *       the vitaGL offset patch cannot express).
 * This samples a few draws a frame and hashes every buffer at upload, so one
 * session says which. Gated on GEOM_PROBE in config.h.
 */
#pragma once

#include <vitaGL.h>
#include <stdint.h>

void geo_note_bind(GLenum target, GLuint buf);
void geo_note_attrib(GLuint index, GLint size, GLenum type, GLsizei stride,
                     const void *pointer, GLuint buf);
void geo_note_enable(GLuint index, int on);
void geo_note_bufdata(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
void geo_note_subdata(GLenum target);
void geo_note_map(GLenum target);
void geo_note_unmap(GLenum target);
void geo_note_delete(GLsizei n, const GLuint *bufs);
void geo_note_link(GLuint prog);
void geo_check_elements(GLuint prog, GLsizei count, GLenum type, const void *idx);
void geo_check_arrays(GLuint prog, GLint first, GLsizei count);
void geo_on_swap(void);
void geo_window_report(void);
