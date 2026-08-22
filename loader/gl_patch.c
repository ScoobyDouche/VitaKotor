/* gl_patch.c -- wire the companion's real GLES2 imports to vitaGL.
 *
 * libandroid_port.so exposes android_port_gl* wrappers to the game, but the
 * wrappers themselves import the *real* gl* entry points (141 of them). Those
 * were deliberately left unresolved during skeleton bring-up; the first GL call
 * (OpenGLES20Implementation::init -> glGetIntegerv) therefore jumped through an
 * unresolved PLT slot and prefetch-aborted. This table resolves them to vitaGL.
 *
 * Three groups:
 *   (1) direct     -- integer/pointer signatures map straight to vitaGL.
 *   (2) float shims -- functions taking GLfloat BY VALUE. The Android .so is
 *       softfp (floats in core regs r0-r3); vitaGL here is hardfp (floats in
 *       VFP). A hardfp function whose params are declared uint32_t receives the
 *       softfp bit patterns in core regs unchanged; we reinterpret to float and
 *       call vitaGL normally (hardfp->hardfp). Pointer variants (*fv, Matrix*fv)
 *       need no shim -- pointers pass in core regs in both ABIs.
 *   (3) gap stubs  -- 14 symbols vitaGL does not implement; safe bring-up
 *       defaults (no-ops / zero-fill) so shader/query paths don't wild-branch.
 */

#include <vitasdk.h>
#include <vitaGL.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "so_util.h"
#include "config.h"
#include "gl_patch.h"
#include "glsl_prep.h"
#include "dynlib.h"
#include "log.h"
#include "loadscreen.h"

static inline float u2f(uint32_t u) { union { uint32_t u; float f; } c; c.u = u; return c.f; }

// Per-call GL trace budget lives in config.h (GL_TRACE_LIMIT) so it can be
// re-armed for bring-up without editing this file. Declared early so both
// glViewport_t and the GLLOG block below can gate on it.
static int g_gl_seq = 0;

/* -- (2) softfp->hardfp float-by-value shims -------------------------------- */
static void glClearColor_s(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
  glClearColor(u2f(r), u2f(g), u2f(b), u2f(a));
}
static void glClearDepthf_s(uint32_t d) { glClearDepthf(u2f(d)); }
static void glDepthRangef_s(uint32_t n, uint32_t f) { glDepthRangef(u2f(n), u2f(f)); }
static void glLineWidth_s(uint32_t w) { glLineWidth(u2f(w)); }
static void glPolygonOffset_s(uint32_t factor, uint32_t units) { glPolygonOffset(u2f(factor), u2f(units)); }
static void glTexParameterf_s(uint32_t target, uint32_t pname, uint32_t param) {
  glTexParameterf(target, pname, u2f(param));
}
static void glUniform1f_s(uint32_t loc, uint32_t v0) { glUniform1f(loc, u2f(v0)); }
static void glUniform2f_s(uint32_t loc, uint32_t v0, uint32_t v1) { glUniform2f(loc, u2f(v0), u2f(v1)); }
static void glUniform3f_s(uint32_t loc, uint32_t v0, uint32_t v1, uint32_t v2) {
  glUniform3f(loc, u2f(v0), u2f(v1), u2f(v2));
}
static void glUniform4f_s(uint32_t loc, uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3) {
  glUniform4f(loc, u2f(v0), u2f(v1), u2f(v2), u2f(v3));
}
static void glVertexAttrib1f_s(uint32_t i, uint32_t v0) { glVertexAttrib1f(i, u2f(v0)); }
static void glVertexAttrib2f_s(uint32_t i, uint32_t v0, uint32_t v1) { glVertexAttrib2f(i, u2f(v0), u2f(v1)); }
static void glVertexAttrib3f_s(uint32_t i, uint32_t v0, uint32_t v1, uint32_t v2) {
  glVertexAttrib3f(i, u2f(v0), u2f(v1), u2f(v2));
}
static void glVertexAttrib4f_s(uint32_t i, uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3) {
  glVertexAttrib4f(i, u2f(v0), u2f(v1), u2f(v2), u2f(v3));
}

/* -- (3) gap stubs: not implemented by vitaGL ------------------------------- */
static void glBlendColor_g(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { (void)r;(void)g;(void)b;(void)a; }
static void glCompressedTexSubImage2D_g(GLenum t, GLint l, GLint xo, GLint yo, GLsizei w, GLsizei h,
                                        GLenum fmt, GLsizei sz, const void *d) {
  (void)t;(void)l;(void)xo;(void)yo;(void)w;(void)h;(void)fmt;(void)sz;(void)d;
}
static void glDetachShader_g(GLuint p, GLuint s) { (void)p;(void)s; }
static void glGetRenderbufferParameteriv_g(GLenum t, GLenum p, GLint *params) { (void)t;(void)p; if (params) params[0] = 0; }
static void glGetShaderPrecisionFormat_g(GLenum st, GLenum pt, GLint *range, GLint *precision) {
  (void)st;(void)pt;
  if (range) { range[0] = 127; range[1] = 127; }
  if (precision) *precision = 23;  /* highp float */
}
static void glGetTexParameterfv_g(GLenum t, GLenum p, GLfloat *params) { (void)t;(void)p; if (params) params[0] = 0.0f; }
static void glGetTexParameteriv_g(GLenum t, GLenum p, GLint *params) { (void)t;(void)p; if (params) params[0] = 0; }
static void glGetUniformfv_g(GLuint prog, GLint loc, GLfloat *params) { (void)prog;(void)loc; if (params) params[0] = 0.0f; }
static void glGetUniformiv_g(GLuint prog, GLint loc, GLint *params) { (void)prog;(void)loc; if (params) params[0] = 0; }
static GLboolean glIsBuffer_g(GLuint b) { return b ? GL_TRUE : GL_FALSE; }
static GLboolean glIsShader_g(GLuint s) { return s ? GL_TRUE : GL_FALSE; }
static void glSampleCoverage_g(uint32_t value, GLboolean invert) { (void)value;(void)invert; }
static void glTexParameterfv_g(GLenum target, GLenum pname, const GLfloat *params) {
  if (params) glTexParameteri(target, pname, (GLint)params[0]);
}
static void glValidateProgram_g(GLuint p) { (void)p; }


/* -- GL lifecycle tracing ---------------------------------------------------
 * The game's GL init is otherwise invisible in the log. These wrappers trace
 * the shape of bring-up (capability queries, shader pipeline, first frame) so a
 * silent hang/abort in the GL region is localised. Shader compile/link failures
 * dump the vitaGL/vitashark info log -- the usual suspect for a stall here.
 */
static const GLubyte *glGetString_t(GLenum name) {
  log_printf("[GL] glGetString(0x%x) ...", (unsigned)name);
  const GLubyte *s = glGetString(name);
  log_printf("[GL] glGetString(0x%x) -> \"%s\"", (unsigned)name, s ? (const char *)s : "(null)");
  return s;
}
static GLuint glCreateShader_t(GLenum type) {
  log_printf("[GL] glCreateShader(0x%x) ...", (unsigned)type);   // log BEFORE: first
  GLuint id = glCreateShader(type);                              // call may lazily
  log_printf("[GL] glCreateShader(0x%x) -> %u", (unsigned)type, (unsigned)id);  // init vitashark
  return id;
}
static void glShaderSource_t(GLuint sh, GLsizei count, const GLchar *const *str, const GLint *len) {
  log_printf("[GL] glShaderSource(sh=%u, count=%d) ===begin dump===", (unsigned)sh, count);
  // Dump the define header (str[0]) line-by-line -- its concrete macro values
  // decide which #if branches (and thus which varyings) are live. Also emit every
  // 'varying' source line across all strings so we can count declared varyings.
  char line[256];
  for (int s = 0; s < count && str; s++) {
    const char *p = str[s];
    if (!p) continue;
    int header = (s == 0);
    while (*p) {
      int n = 0;
      while (n < 255 && p[n] && p[n] != '\n') { line[n] = p[n]; n++; }
      line[n] = '\0';
      // Header: log all lines. Other chunks: only 'varying' declaration lines.
      if (header) {
        if (n > 0) log_printf("[GLSRC h] %s", line);
      } else {
        const char *v = line; while (*v == ' ' || *v == '\t') v++;
        if (!strncmp(v, "varying", 7)) log_printf("[GLSRC s%d] %s", s, line);
      }
      p += n;
      if (*p == '\n') p++;
    }
  }
  log_printf("[GL] glShaderSource(sh=%u) ===end dump===", (unsigned)sh);

  // vitaGL finds varying declarations by text-scanning for the keyword, with no
  // preprocessor evaluation at all -- so it reserves a GXM TEXCOORD slot for
  // every declaration in the ubershader, including the ~18 that the #if guards
  // discard and any that merely appear in a comment. That overflows
  // MAX_CG_TEXCOORD_ID (10), force-binds the excess to TEXCOORD9 and faults
  // inside the shader compiler at glLinkProgram. Do the dead-declaration
  // elimination here, on a concatenated copy, before vitaGL ever sees it.
  size_t total = 1;
  for (int s = 0; s < count && str; s++)
    total += (len && len[s] >= 0) ? (size_t)len[s] : (str[s] ? strlen(str[s]) : 0);

  char *joined = (char *)malloc(total);
  if (joined) {
    size_t off = 0;
    for (int s = 0; s < count && str; s++) {
      if (!str[s]) continue;
      size_t l = (len && len[s] >= 0) ? (size_t)len[s] : strlen(str[s]);
      memcpy(joined + off, str[s], l);
      off += l;
    }
    joined[off] = '\0';

    // SKINNING BISECT (log77). Characters explode into spikes, and every input to
    // the skinned shader has now been proven correct in turn:
    //   - .mdl/.mdx bytes match an offline LZMA decode exactly
    //   - attributes are contiguous GL_FLOAT at stride 64, nothing normalised
    //   - glUniform4fv delivers count=51 of clean near-identity bone rows
    //   - the LINKED program reports u_boneMatrices size=51 type=GL_FLOAT_VEC4,
    //     so ShaccCg did not collapse the array
    // Three theories disproved by measurement, so stop theorising about the one
    // remaining suspect (GXM's execution of `u_boneMatrices[indices.x]`, a
    // dynamic uniform-array read driven by a vertex attribute) and test it.
    // Forcing the ubershader's own USE_SKIN switch to 0 takes the `pos = a_position`
    // branch instead, bypassing the indexed read entirely while changing nothing
    // else. The rewrite is length-preserving ('1' -> '0'), so every offset in the
    // buffer -- and the varying pre-pass that runs next -- is unaffected.
    // Spikes gone  => the dynamic indexed read is the culprit, and the fix belongs
    //                 in vitaGL's GLSL->Cg translation.
    // Spikes stay  => skinning is exonerated and the fault is elsewhere entirely
    //                 (geometry/index buffers), which redirects the whole hunt.
    // Cost while enabled: characters render in BIND POSE -- static, but correctly
    // shaped, which is strictly better to look at than the current explosion.
    // This is a diagnostic, not the fix; revert once the cause is known.
    int skin_off = 0;
    if (SKIN_BISECT_DISABLE) {
      char *d = joined;
      while ((d = strstr(d, "#define USE_SKIN 1")) != NULL) {
        d[sizeof("#define USE_SKIN ") - 1] = '0';
        skin_off++;
        d += sizeof("#define USE_SKIN ") - 1;
      }
      if (skin_off)
        log_printf("[GL] SKIN BISECT sh=%u: forced USE_SKIN 1 -> 0 (%d site%s)",
                   (unsigned)sh, skin_off, skin_off == 1 ? "" : "s");
    }

    // Skinning bone-index rounding fix. The bisect above proved the dynamic
    // uniform-array read is what wrecks characters, and everything feeding it is
    // provably correct (attributes, bone data, and the linked program's
    // u_boneMatrices[51] all verified). What is left is the index arithmetic:
    //     ivec4 indices = ivec4(clamp(3.0 * a_matrixIndices, 0.0, 50.0));
    // ivec4() TRUNCATES. Each bone occupies 3 consecutive vec4 rows, so the true
    // values are exact multiples of 3 -- but only if a_matrixIndices survives as
    // an exact integer. If ShaccCg demotes the attribute to fp16, a stored 3.0
    // can arrive as 2.9999, and 3.0*2.9999 = 8.9997 truncates to 8 instead of 9:
    // that vertex then reads rows 8/9/10 rather than 9/10/11 and is transformed
    // by an entirely different bone. Only indices that land just under an integer
    // are affected, which is exactly why the models are mostly right with some
    // vertices flung away, rather than uniformly wrong.
    // Adding 0.5 before truncation turns floor() into round-to-nearest, making
    // the index robust to +/-0.5 of error while changing nothing when the value
    // is already exact. The replacement is written to the same 46 characters as
    // the original so every offset in the buffer -- and the varying pre-pass that
    // runs next -- is untouched. `clamp` deliberately stays in the float domain:
    // GLSL ES 1.00 has no integer overload of clamp().
    int skin_fix = 0;
    if (SKIN_INDEX_ROUND_FIX) {
      static const char kOld[] = "ivec4(clamp(3.0 * a_matrixIndices, 0.0, 50.0))";
      static const char kNew[] = "ivec4(clamp(3.*a_matrixIndices+.5,0.,50.))    ";
      char *d = joined;
      while ((d = strstr(d, kOld)) != NULL) {
        memcpy(d, kNew, sizeof(kNew) - 1);   // same length: offsets preserved
        skin_fix++;
        d += sizeof(kNew) - 1;
      }
      if (skin_fix)
        log_printf("[GL] SKIN FIX sh=%u: bone index round-to-nearest (%d site%s)",
                   (unsigned)sh, skin_fix, skin_fix == 1 ? "" : "s");
    }
    skin_off += skin_fix;   // either rewrite means we must pass OUR buffer on

    int raw = 0, live = 0;
    if (glsl_prep_strip_dead_varyings(joined, &raw, &live)) {
      log_printf("[GL] glShaderSource(sh=%u): varyings %d seen by vitaGL -> %d live",
                 (unsigned)sh, raw, live);
      if (live > 10)
        log_printf("[GL] WARNING sh=%u: %d live varyings still exceeds the 10-slot "
                   "TEXCOORD budget -- link will overflow", (unsigned)sh, live);
      const GLchar *one = joined;
      glShaderSource(sh, 1, &one, NULL);
      free(joined);
      return;
    }
    // If the varying pre-pass declined we would normally hand back the game's
    // original strings -- but that would silently discard a USE_SKIN rewrite, so
    // pass our edited copy in that case.
    if (skin_off) {
      log_printf("[GL] glShaderSource(sh=%u): varying pre-pass declined, but "
                 "passing the SKIN-BISECT copy", (unsigned)sh);
      const GLchar *one = joined;
      glShaderSource(sh, 1, &one, NULL);
      free(joined);
      return;
    }
    log_printf("[GL] glShaderSource(sh=%u): varying pre-pass declined this source, "
               "passing it through unmodified", (unsigned)sh);
    free(joined);
  }

  glShaderSource(sh, count, str, len);
}
static void glCompileShader_t(GLuint sh) {
  log_printf("[GL] glCompileShader(%u) ...", (unsigned)sh);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char info[512]; info[0] = '\0';
    glGetShaderInfoLog(sh, sizeof(info), NULL, info);
    log_printf("[GL] !!! shader %u COMPILE FAILED: %s", (unsigned)sh, info);
  } else {
    log_printf("[GL] shader %u compiled OK", (unsigned)sh);
  }
}
// vitaGL's glReleaseShaderCompiler calls shark_end(), which tears the runtime
// shader compiler down for good. That is a reasonable thing to honour on a
// desktop driver, but vitaGL is in VGL_MODE_POSTPONED here: GLSL is translated
// and compiled lazily inside *every* glLinkProgram. Once shark is ended,
// shark_compile_shader_extended returns NULL on its first instruction -- without
// invoking the compiler, so it emits no diagnostics at all -- vitaGL leaves
// shader->prog NULL, and glLinkProgram's set_default_attrib_binding() hands that
// NULL to sceGxmProgramGetParameterCount, which faults at FAR=0x24 inside
// SceGxm. libandroid_port.so imports this entry point, so the game can pull the
// rug out from under every shader it has not compiled yet. Refuse to do it: we
// keep the compiler alive for the life of the process.
static void glReleaseShaderCompiler_t(void) {
  extern GLboolean is_shark_online;
  log_printf("[GL] glReleaseShaderCompiler() IGNORED (would shark_end() and make "
             "every later shader compile fail silently; shark_online=%d)",
             (int)is_shark_online);
}
static GLuint   g_cur_prog = 0;           /* redundant program-switch shadow */
static unsigned g_prog_skipped_win = 0;

static void glLinkProgram_t(GLuint p) {
  extern GLboolean is_shark_online;
  log_printf("[GL] glLinkProgram(%u) ... shark_online=%d", (unsigned)p,
             (int)is_shark_online);
  glLinkProgram(p);
  g_cur_prog = 0;             /* relinking can change what this id draws with */
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char info[512]; info[0] = '\0';
    glGetProgramInfoLog(p, sizeof(info), NULL, info);
    log_printf("[GL] !!! program %u LINK FAILED: %s", (unsigned)p, info);
  } else {
    log_printf("[GL] program %u linked OK", (unsigned)p);
    // Skinned characters explode into spikes even though the vertex layout and
    // the bone data we hand GL are both provably correct (attributes are
    // contiguous GL_FLOAT at stride 64; glUniform4fv gets count=51 of clean
    // near-identity rows). kotor.vert indexes `uniform vec4 u_boneMatrices[51]`
    // DYNAMICALLY from a vertex attribute, so the remaining suspect is what the
    // COMPILED GXM program made of that array. vitaGL reports it faithfully:
    // glGetActiveUniform's size comes straight from
    // sceGxmProgramParameterGetArraySize. If u_boneMatrices comes back with
    // size=1 (or anything < 51), ShaccCg collapsed the array and every dynamic
    // index reads the same row -- which is exactly what flings vertices onto
    // wrong bones. Dump it once per program, alongside the attributes so the
    // active set can be matched against the a_* locations we already log.
    GLint nu = 0, na = 0;
    glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &nu);
    glGetProgramiv(p, GL_ACTIVE_ATTRIBUTES, &na);
    log_printf("[vtx] prog %u: %d active uniforms, %d active attributes",
               (unsigned)p, (int)nu, (int)na);
    for (GLint i = 0; i < nu && i < 48; i++) {
      char nm[96]; GLint sz = -1; GLenum ty = 0; GLsizei len = 0;
      nm[0] = '\0';
      glGetActiveUniform(p, (GLuint)i, sizeof(nm), &len, &sz, &ty, nm);
      log_printf("[vtx]   uniform[%d] \"%.48s\" size=%d type=0x%x", (int)i, nm,
                 (int)sz, (unsigned)ty);
    }
    for (GLint i = 0; i < na && i < 24; i++) {
      char nm[96]; GLint sz = -1; GLenum ty = 0; GLsizei len = 0;
      nm[0] = '\0';
      glGetActiveAttrib(p, (GLuint)i, sizeof(nm), &len, &sz, &ty, nm);
      log_printf("[vtx]   attrib[%d] \"%.48s\" size=%d type=0x%x", (int)i, nm,
                 (int)sz, (unsigned)ty);
    }
  }
}
static void glViewport_t(GLint x, GLint y, GLsizei w, GLsizei h) {
  if (g_gl_seq < GL_TRACE_LIMIT)  // silence with the rest of the per-call trace
    log_printf("[GL] glViewport(%d, %d, %d, %d)", x, y, (int)w, (int)h);
  glViewport(x, y, w, h);
}
// Draw/clear accounting. The first few of each are logged in full; after that we
// only count, and gl_patch_on_swap() prints a per-frame-window summary. Both the
// "since last summary" and lifetime totals are tracked so a stuck render loop
// (identical draw count every window) is distinguishable from an advancing one.
static int g_clear_n = 0, g_draw_n = 0;
static unsigned g_arrays_win = 0, g_elements_win = 0, g_clears_win = 0;
static unsigned g_arrays_tot = 0, g_elements_tot = 0;
static void glClear_t(GLbitfield mask) {
  if (g_clear_n < 5) log_printf("[GL] glClear(0x%x) #%d", (unsigned)mask, g_clear_n);
  g_clear_n++;
  g_clears_win++;
  glClear(mask);
}
// Text-draw trace: g_gl_text_draw is raised around the game's GUI-string Draw, so
// these lines isolate the glyph draws from the thousands of ordinary scene draws.
// g_cur_tex mirrors the GL_TEXTURE_2D binding -- a glyph quad drawn with texture 0
// (or with a non-atlas texture) is the difference between "text never reaches GL"
// and "text reaches GL untextured".
int g_gl_text_draw = 0;
static unsigned g_cur_tex = 0;
static int g_textdraw_n = 0;
#define TEXTDRAW_LOG_MAX 80

// Draws bucketed by the SIZE of the bound texture. Keying on dimensions rather than
// texture id is deliberate: ids are recycled after glDeleteTextures (tex10 was
// re-uploaded six times under a different format), which made an id-keyed counter
// meaningless. A re-upload simply overwrites the dimension entry.
// 756x106 is the main-menu button art (20 of them, ios_mm_*_en.tga); if that bucket
// stays at 0 while the buttons are on screen, their quads never reach GL.
#define TEXDIM_MAX 512
static unsigned short g_tex_w[TEXDIM_MAX], g_tex_h[TEXDIM_MAX];
#define BUCKET_MAX 14
static unsigned short g_bk_w[BUCKET_MAX], g_bk_h[BUCKET_MAX];
static unsigned g_bk_n_draws[BUCKET_MAX];
static int g_bk_n = 0;

static void tex_note_size(unsigned tex, int w, int h) {
  if (tex < TEXDIM_MAX) { g_tex_w[tex] = (unsigned short)w; g_tex_h[tex] = (unsigned short)h; }
}
static void tex_note_draw(unsigned tex) {
  if (tex >= TEXDIM_MAX) return;
  unsigned short w = g_tex_w[tex], h = g_tex_h[tex];
  if (!w) return;                      // untextured or never uploaded
  for (int k = 0; k < g_bk_n; k++)
    if (g_bk_w[k] == w && g_bk_h[k] == h) { g_bk_n_draws[k]++; return; }
  if (g_bk_n < BUCKET_MAX) { g_bk_w[g_bk_n] = w; g_bk_h[g_bk_n] = h; g_bk_n_draws[g_bk_n++] = 1; }
}

/* Per-draw CPU cost is the remaining stutter lever: log119 hit ~745 draw calls a
 * frame at 13 fps (~103 us/draw), which is far too slow to be GPU fill. The two
 * usual causes are (a) client-side vertex arrays, which vitaGL must copy into the
 * circular pool on EVERY draw, and (b) redundant state changes. Count both so the
 * next tuning step is chosen from data rather than guessed. */
static unsigned g_draw_client_win = 0, g_draw_vbo_win = 0;
static unsigned g_prog_win = 0, g_texbind_win = 0, g_bufdata_win = 0;
/* Attribute-offset high-water mark, for the geometry corruption. The exploding
 * characters were SceGxmVertexAttribute::offset being a uint16_t -- any VBO
 * offset >= 64 KB truncated mod 65536 and the GPU fetched each attribute from a
 * different vertex of the same buffer, drawing a recognisable model with long
 * spikes. That is fixed in this tree's vitaGL and the fix is confirmed present
 * in the linked library, but the photos of the late-session corruption show
 * spikes rather than the diagonal smear the README describes, so watch the
 * boundary instead of assuming it. If corruption arrives in the same window
 * maxAttrOff first passes 64 KB, that is the answer; if offsets never approach
 * it, the whole family is ruled out and the search moves to texture eviction. */
static uintptr_t g_vap_max_off = 0;
static unsigned  g_vap_over64k = 0;
static unsigned g_bind_skipped_win = 0;   /* redundant binds we suppressed */
static unsigned g_nontex2d_binds = 0;     /* cube/3D binds -- the envmap path */
static GLuint   g_cur_arraybuf = 0;      /* last glBindBuffer(GL_ARRAY_BUFFER) */

static inline void draw_note_source(void) {
  if (g_cur_arraybuf) g_draw_vbo_win++; else g_draw_client_win++;
}

static void glDrawArrays_t(GLenum mode, GLint first, GLsizei count) {
  if (loadscreen_active()) loadscreen_end();   /* first real frame: hand over */
  if (g_draw_n < 20) log_printf("[GL] glDrawArrays(mode=0x%x, first=%d, count=%d) #%d",
                               (unsigned)mode, first, (int)count, g_draw_n);
  if (g_gl_text_draw && g_textdraw_n < TEXTDRAW_LOG_MAX)
    log_printf("[textdraw#%d] glDrawArrays(mode=0x%x, count=%d) tex=%u",
               g_textdraw_n++, (unsigned)mode, (int)count, g_cur_tex);
  tex_note_draw(g_cur_tex);
  g_draw_n++; g_arrays_win++; g_arrays_tot++; draw_note_source();
  glDrawArrays(mode, first, count);
}
static void glDrawElements_t(GLenum mode, GLsizei count, GLenum type, const void *idx) {
  if (loadscreen_active()) loadscreen_end();   /* first real frame: hand over */
  if (g_draw_n < 20) log_printf("[GL] glDrawElements(mode=0x%x, count=%d, type=0x%x) #%d",
                               (unsigned)mode, (int)count, (unsigned)type, g_draw_n);
  if (g_gl_text_draw && g_textdraw_n < TEXTDRAW_LOG_MAX)
    log_printf("[textdraw#%d] glDrawElements(mode=0x%x, count=%d) tex=%u",
               g_textdraw_n++, (unsigned)mode, (int)count, g_cur_tex);
  tex_note_draw(g_cur_tex);
  g_draw_n++; g_elements_win++; g_elements_tot++; draw_note_source();
  glDrawElements(mode, count, type, idx);
}
void gl_patch_on_swap(void) {
  static unsigned frame = 0;
  frame++;
  if (frame % 120 == 0) {  // ~ every couple seconds at 60fps
    log_printf("[GL] frame %u: this window drawArrays=%u drawElements=%u clears=%u"
               " | lifetime draws=%u",
               frame, g_arrays_win, g_elements_win, g_clears_win,
               g_arrays_tot + g_elements_tot);
    char ab[256]; int o = 0;
    for (int k = 0; k < g_bk_n && o < (int)sizeof(ab) - 28; k++)
      o += snprintf(ab + o, sizeof(ab) - o, "%ux%u=%u ", g_bk_w[k], g_bk_h[k], g_bk_n_draws[k]);
    if (o) log_printf("[GL]   draws by texture size (lifetime): %s", ab);
    log_printf("[GL]   per-window: clientArrayDraws=%u vboDraws=%u  texBinds=%u "
               "(skipped %u = %u%%) progSwitches=%u (skipped %u = %u%%) "
               "bufferUploads=%u nonTex2DBinds=%u",
               g_draw_client_win, g_draw_vbo_win, g_texbind_win, g_bind_skipped_win,
               g_texbind_win ? (g_bind_skipped_win * 100 / g_texbind_win) : 0,
               g_prog_win, g_prog_skipped_win,
               g_prog_win ? (g_prog_skipped_win * 100 / g_prog_win) : 0,
               g_bufdata_win, g_nontex2d_binds);
    g_bind_skipped_win = g_prog_skipped_win = 0;
    g_arrays_win = g_elements_win = g_clears_win = 0;
    g_draw_client_win = g_draw_vbo_win = 0;
    g_texbind_win = g_prog_win = g_bufdata_win = 0;
  }
}
/* Capability-query getters. The companion's OpenGLES20Implementation::init()
 * fires ~18 of these before any other GL call, and feeds one straight into a
 * malloc size. If vitaGL doesn't handle an enum it leaves *params UNWRITTEN
 * (uninitialised) -> garbage count -> huge malloc / bad loop. The sentinel
 * pre-fill detects exactly that, and prints the value so we see the last query
 * before a hang. */
// log97: in-game, these getters alone emitted 6864 of the log's 19086 lines --
// the game polls GL_TEXTURE_BINDING_2D (0x8069) every few draws. Each line is a
// separate open/write/close on the memory card, so this was a large slice of the
// low frame rate. The *diagnostic* value is all in init (~18 cap queries) and the
// UNWRITTEN case; steady-state values are noise. Budget the normal case, and keep
// UNWRITTEN unconditional since it always signals a real vitaGL gap.
/* GLGET_TRACE_LIMIT lives in config.h so it can be re-armed without editing
 * this file. 0 disables the budgeted case; UNWRITTEN stays unconditional. */
static int g_glget_n = 0;
#define GLGETLOG(...) do { \
    if (g_glget_n < GLGET_TRACE_LIMIT) { \
      log_printf(__VA_ARGS__); \
      if (++g_glget_n == GLGET_TRACE_LIMIT) \
        log_printf("[GL] getter trace silenced after %d calls (steady state)", \
                   GLGET_TRACE_LIMIT); \
    } \
  } while (0)

static void glGetIntegerv_t(GLenum pname, GLint *params) {
  const GLint SENT = 0x0BADF00D;
  if (params) *params = SENT;
  glGetIntegerv(pname, params);
  if (params && *params == SENT)
    log_printf("[GL] glGetIntegerv(0x%x) -> UNWRITTEN (vitaGL ignored enum)", (unsigned)pname);
  else
    GLGETLOG("[GL] glGetIntegerv(0x%x) -> %d", (unsigned)pname, params ? *params : 0);
  // 0xd57 is init()'s LAST cap query; the hang is just past here. Arm heap
  // tracing now so OpenGLESState::init's allocations become visible.
  if (MEM_TRACE_ENABLE && pname == 0xd57 && !g_mem_trace) {
    log_printf("[GL] --- arming heap trace (entering GL engine setup) ---");
    g_mem_trace = 1;
  }
}
static void glGetBooleanv_t(GLenum pname, GLboolean *params) {
  const GLboolean SENT = 0x5A;
  if (params) *params = SENT;
  glGetBooleanv(pname, params);
  if (params && *params == SENT)
    log_printf("[GL] glGetBooleanv(0x%x) -> UNWRITTEN", (unsigned)pname);
  else
    GLGETLOG("[GL] glGetBooleanv(0x%x) -> %d", (unsigned)pname, params ? *params : 0);
}
static void glGetFloatv_t(GLenum pname, GLfloat *params) {
  union { uint32_t u; float f; } sent = { .u = 0x0BADF00D };
  if (params) *params = sent.f;
  glGetFloatv(pname, params);
  union { uint32_t u; float f; } got = { .f = params ? *params : 0.0f };
  if (params && got.u == sent.u)
    log_printf("[GL] glGetFloatv(0x%x) -> UNWRITTEN", (unsigned)pname);
  else
    GLGETLOG("[GL] glGetFloatv(0x%x) -> bits 0x%x", (unsigned)pname, (unsigned)got.u);
}

/* -- entry tracing for post-init setup calls --------------------------------
 * init() completes; the hang is the first GXM-touching GL call after it, which
 * is otherwise unlogged. These wrappers log on ENTRY (before calling through),
 * with a global sequence number, so the LAST line in the log is the call that
 * blocked -- even if it never returns. Restricted to the state/object-setup
 * surface a GL engine touches right after capability query. */
// log.c flushes every line to the SD card with its own open/write/close (~15ms).
// Tracing every GL call (~50/frame) therefore throttled the whole game to ~1 fps
// and starved its frame timers. Trace only the first GL_TRACE_LIMIT calls (see
// declaration above u2f) -- that covers init and the first frames, where every
// historical hang lived -- then auto-silence so steady state runs at native
// speed. Frame summaries, shader, link, and crash logging are unaffected.
#define GLLOG(fmt, ...) do { \
    int _s = g_gl_seq++; \
    if (_s < GL_TRACE_LIMIT) log_printf("[GL#%d] " fmt, _s, ##__VA_ARGS__); \
    else if (_s == GL_TRACE_LIMIT) \
      log_printf("[GL] per-call GL trace silenced after %d calls (steady state; " \
                 "frame/shader/link/crash logs continue)", GL_TRACE_LIMIT); \
    loadscreen_note_gl(); \
  } while (0)
static void glEnable_e(GLenum cap) { GLLOG("glEnable(0x%x)", (unsigned)cap); glEnable(cap); }
static void glDisable_e(GLenum cap) { GLLOG("glDisable(0x%x)", (unsigned)cap); glDisable(cap); }
/* Redundant-bind filter.
 *
 * log120: texBinds=1562368 against vboDraws=1549697 -- the game rebinds a texture
 * before essentially EVERY draw, and vitaGL's glBindTexture does real state work
 * each time. Skipping binds that do not change anything is free and safe, as long
 * as the shadow state is exact:
 *   - tracked PER TEXTURE UNIT (glBindTexture affects the active unit only)
 *   - GL_TEXTURE_2D only; every other target passes straight through
 *   - the whole shadow is dropped on glDeleteTextures, because ids get recycled
 *     and a stale entry would silently draw with the wrong texture
 *   - and binding ANY other target to a unit clears that unit's entry. Desktop
 *     GL keeps one binding per target per unit, so a cube bind would leave the
 *     2D binding intact and skipping the next 2D bind would be correct -- but
 *     GXM has a single texture per sampler slot and vitaGL is not obliged to
 *     model GL's per-target state. The shadow must not assume it does: kotor.vert
 *     samples u_texture2Sampler as a real GL_SAMPLER_CUBE for the environment
 *     map, so exactly the shiny-armour materials bind a cube to a unit that
 *     otherwise carries a 2D texture. Forgetting the entry costs one redundant
 *     bind on the rare cube path and cannot draw the wrong texture.
 * Set GL_FILTER_REDUNDANT_BINDS to 0 in config.h to rule this out. */
#define GL_MAX_TEXUNITS 8
static unsigned g_active_unit = 0;
static GLuint   g_bound2d[GL_MAX_TEXUNITS];

static void glActiveTexture_e(GLenum t) {
  GLLOG("glActiveTexture(0x%x)", (unsigned)t);
  unsigned u = (unsigned)t - 0x84C0u;               /* GL_TEXTURE0 */
  g_active_unit = (u < GL_MAX_TEXUNITS) ? u : 0;
  glActiveTexture(t);
}

static void glDeleteTextures_e(GLsizei n, const GLuint *tex) {
  /* ids are recycled, so any cached binding may now mean a different texture */
  memset(g_bound2d, 0, sizeof g_bound2d);
  glDeleteTextures(n, tex);
}
static void glPixelStorei_e(GLenum p, GLint v) { GLLOG("glPixelStorei(0x%x,%d)", (unsigned)p, v); glPixelStorei(p, v); }
static void glGenTextures_e(GLsizei n, GLuint *t) { GLLOG("glGenTextures(%d)", (int)n); glGenTextures(n, t); }
static void glBindTexture_e(GLenum tg, GLuint t) {
  GLLOG("glBindTexture(0x%x,%u)", (unsigned)tg, (unsigned)t);
  g_texbind_win++;
#if GL_FILTER_REDUNDANT_BINDS
  if (tg == GL_TEXTURE_2D) {
    if (g_bound2d[g_active_unit] == t) { g_bind_skipped_win++; g_cur_tex = t; return; }
    g_bound2d[g_active_unit] = t;
  } else {
    g_bound2d[g_active_unit] = 0;        /* unit may no longer hold that 2D texture */
    g_nontex2d_binds++;
  }
#endif
  if (tg == GL_TEXTURE_2D) g_cur_tex = t;  // mirrored for the text-draw trace
  glBindTexture(tg, t);
}
// --- skinning diagnostics --------------------------------------------------
// Character models render as exploding spikes even though their .mdl/.mdx bytes
// are byte-identical to an offline LZMA decode -- so the DATA is right and the
// way it reaches the GPU is not. kotor.vert's skinned path is:
//     uniform vec4 u_boneMatrices[51];        // 17 bones x 3 vec4 rows
//     attribute vec4 a_matrixWeights, a_matrixIndices;
//     ivec4 indices = ivec4(clamp(3.0 * a_matrixIndices, 0.0, 50.0));
//     pos = weights.x * vec3(dot(u_boneMatrices[indices.x], a_position), ...);
// i.e. DYNAMIC indexing of a uniform array from a vertex attribute. Two things
// can break it independently, and spikes look the same either way:
//   - a_matrixIndices arriving in the wrong format (e.g. UNSIGNED_BYTE flagged
//     normalized -> 0..255 collapses to 0..1 -> 3.0*x picks bone 0-3 for every
//     vertex), so the wrong bone transforms the vertex;
//   - the 51-element uniform array not being uploaded/addressed contiguously by
//     vitaGL, so indices.x+1 / +2 read someone else's rows.
// These get their OWN log budget: the general GLLOG trace is spent during init
// (4000 calls) long before a character is ever drawn, which is why the model
// path has been invisible. Log each DISTINCT attribute configuration once, and
// the first few large glUniform4fv uploads (the bone array).
#define VAP_SLOTS 24
static struct { GLuint idx; GLint size; GLenum type; GLboolean norm; GLsizei stride; uintptr_t off; } g_vap[VAP_SLOTS];
static unsigned g_vap_used = 0;

static void glVertexAttribPointer_e(GLuint index, GLint size, GLenum type, GLboolean normalized,
                                    GLsizei stride, const void *pointer) {
  unsigned i;
  for (i = 0; i < g_vap_used; i++)
    if (g_vap[i].idx == index && g_vap[i].size == size && g_vap[i].type == type &&
        g_vap[i].norm == normalized && g_vap[i].stride == stride &&
        g_vap[i].off == (uintptr_t)pointer)
      break;
  if (i == g_vap_used && g_vap_used < VAP_SLOTS) {
    g_vap[g_vap_used].idx = index; g_vap[g_vap_used].size = size;
    g_vap[g_vap_used].type = type; g_vap[g_vap_used].norm = normalized;
    g_vap[g_vap_used].stride = stride; g_vap[g_vap_used].off = (uintptr_t)pointer;
    g_vap_used++;
    log_printf("[vtx] attrib idx=%u size=%d type=0x%x norm=%d stride=%d off=0x%x",
               (unsigned)index, (int)size, (unsigned)type, (int)normalized,
               (int)stride, (unsigned)(uintptr_t)pointer);
  }
  if ((uintptr_t)pointer > g_vap_max_off) g_vap_max_off = (uintptr_t)pointer;
  if ((uintptr_t)pointer > 0xFFFFu) g_vap_over64k++;
  glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

static unsigned g_u4fv_n = 0;
static void glUniform4fv_e(GLint location, GLsizei count, const GLfloat *v) {
  // The bone array is the only uniform uploaded in bulk; log its first rows so a
  // garbage/identity matrix set is obvious.
  if (count > 8 && g_u4fv_n < 12) {
    log_printf("[vtx] glUniform4fv(loc=%d, count=%d) rows0-2: "
               "[%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]",
               (int)location, (int)count,
               v ? v[0] : 0.f, v ? v[1] : 0.f, v ? v[2] : 0.f, v ? v[3] : 0.f,
               v ? v[4] : 0.f, v ? v[5] : 0.f, v ? v[6] : 0.f, v ? v[7] : 0.f,
               v ? v[8] : 0.f, v ? v[9] : 0.f, v ? v[10] : 0.f, v ? v[11] : 0.f);
    g_u4fv_n++;
  }
  glUniform4fv(location, count, v);
}

// Name->location for the skinning inputs, so the attrib indices above can be tied
// to a_matrixIndices/a_matrixWeights and the uniform loc to u_boneMatrices.
static GLint glGetAttribLocation_e(GLuint prog, const GLchar *name) {
  GLint l = glGetAttribLocation(prog, name);
  if (name && (strstr(name, "matrix") || strstr(name, "position") || strstr(name, "normal")))
    log_printf("[vtx] attribLoc prog=%u \"%s\" -> %d", (unsigned)prog, name, (int)l);
  return l;
}
static GLint glGetUniformLocation_e(GLuint prog, const GLchar *name) {
  GLint l = glGetUniformLocation(prog, name);
  if (name && strstr(name, "bone"))
    log_printf("[vtx] uniformLoc prog=%u \"%s\" -> %d", (unsigned)prog, name, (int)l);
  return l;
}

static GLsizei gl_rt_align_w(GLsizei w) { return (w > 0 && (w & 7)) ? ((w + 7) & ~7) : w; }

static void glTexImage2D_e(GLenum tg, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *px) {
  GLLOG("glTexImage2D(0x%x, l=%d, %dx%d, fmt=0x%x)", (unsigned)tg, l, (int)w, (int)h, (unsigned)f);
  // The environment map is a real cube: kotor.vert declares u_texture2Sampler as
  // GL_SAMPLER_CUBE and the shiny-armour material is the USE_CUBEMAP variant.
  // Nothing in any log so far shows a single cube face being uploaded, so before
  // theorising about why armour renders flat white, record whether the faces
  // arrive at all -- and how many of the six.
  if (tg >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && tg <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) {
    static unsigned nc = 0, faces = 0;
    faces |= 1u << (tg - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
    if (nc < 48)
      log_printf("[GL] cube face %u: l=%d %dx%d ifmt=0x%x fmt=0x%x data=%s "
                 "(faces seen 0x%02x)",
                 (unsigned)(tg - GL_TEXTURE_CUBE_MAP_POSITIVE_X), l, (int)w, (int)h,
                 (unsigned)ifmt, (unsigned)f, px ? "yes" : "NULL", faces);
    nc++;
    glTexImage2D(tg, l, ifmt, w, h, b, f, ty, px);
    return;                        // none of the 2D heuristics below apply
  }
  if (tg != GL_TEXTURE_2D) { glTexImage2D(tg, l, ifmt, w, h, b, f, ty, px); return; }
  // Square power-of-two base levels are the font atlases; naming the texture id
  // here is what lets the text-draw trace say whether a glyph quad used one.
  if (l == 0) {
    tex_note_size(g_cur_tex, (int)w, (int)h);
    if (w == h && (w == 256 || w == 512 || w == 1024))
      log_printf("[GL] atlas candidate: tex=%u %dx%d fmt=0x%x", g_cur_tex, (int)w, (int)h, (unsigned)f);
  }
  // NPOT WIDTH SHEAR TEST (log73). Backgrounds render progressively skewed while
  // every character/GUI surface is clean, and the split falls exactly on width
  // alignment -- tallying every upload in the log:
  //     w%8==0 : 8,16,32,64,128,256,512,1024 (12 sizes)  -- all render correctly
  //     w%8==7 : 847x480, 423x240  -- precisely the sheared backgrounds
  //     w%8==4 : 756x106, 4x4
  // vitaGL lays texture data out at VGL_ALIGN(w,8) stride (gpu_alloc_texture in
  // gpu_utils.c copies row-by-row into aligned_w*bpp) but then hands GXM the
  // UNALIGNED w via vglInitLinearTexture. If GXM does not re-align internally,
  // each row is read one-to-seven pixels early and the error accumulates down the
  // image -- which is exactly the diagonal banding on screen.
  // Rather than patch vitaGL blind, prove it here: for an odd width, upload a
  // padded copy whose width IS 8-aligned, replicating the last column into the
  // pad so edge sampling stays sane. If the shear disappears, the alignment
  // theory is right and the real fix belongs in vitaGL (strided texture init).
  // Cost is one staging buffer per NPOT upload; these are a handful of
  // backgrounds, not a per-frame path.
  int bpp = (f == GL_RGBA) ? 4 : (f == GL_RGB) ? 3 : 0;
  if (px && bpp && l == 0 && w > 0 && h > 0 && (w & 7)) {
    int aw = (w + 7) & ~7;
    unsigned char *pad = (unsigned char *)malloc((size_t)aw * h * bpp);
    if (pad) {
      const unsigned char *src = (const unsigned char *)px;
      for (int y = 0; y < h; y++) {
        unsigned char *drow = pad + (size_t)y * aw * bpp;
        const unsigned char *srow = src + (size_t)y * w * bpp;
        memcpy(drow, srow, (size_t)w * bpp);
        for (int x = w; x < aw; x++)              // replicate last column
          memcpy(drow + (size_t)x * bpp, srow + (size_t)(w - 1) * bpp, bpp);
      }
      log_printf("[GL] NPOT pad: %dx%d -> %dx%d (bpp=%d)", (int)w, (int)h, aw, (int)h, bpp);
      glTexImage2D(tg, l, ifmt, aw, h, b, f, ty, pad);
      free(pad);
      return;
    }
  }
  // Storage-only allocation (data == NULL) is the FBO colour attachment; pad its
  // width to match the renderbuffer above so the two stay dimension-consistent.
  if (!px && l == 0) {
    GLsizei aw = gl_rt_align_w(w);
    if (aw != w)
      log_printf("[GL] RT pad: color tex %dx%d -> %dx%d", (int)w, (int)h, (int)aw, (int)h);
    glTexImage2D(tg, l, ifmt, aw, h, b, f, ty, px);
    return;
  }
  glTexImage2D(tg, l, ifmt, w, h, b, f, ty, px);
}
static void glTexParameteri_e(GLenum tg, GLenum p, GLint v) { GLLOG("glTexParameteri(0x%x,0x%x,%d)", (unsigned)tg, (unsigned)p, v); glTexParameteri(tg, p, v); }
static void glGenFramebuffers_e(GLsizei n, GLuint *f) { GLLOG("glGenFramebuffers(%d)", (int)n); glGenFramebuffers(n, f); }
static void glBindFramebuffer_e(GLenum tg, GLuint f) { GLLOG("glBindFramebuffer(0x%x,%u)", (unsigned)tg, (unsigned)f); glBindFramebuffer(tg, f); }
static void glGenRenderbuffers_e(GLsizei n, GLuint *r) { GLLOG("glGenRenderbuffers(%d)", (int)n); glGenRenderbuffers(n, r); }
static void glBindRenderbuffer_e(GLenum tg, GLuint r) { GLLOG("glBindRenderbuffer(0x%x,%u)", (unsigned)tg, (unsigned)r); glBindRenderbuffer(tg, r); }
// The scrambled chargen/menu backgrounds are RENDER TARGETS, not uploads: every
// `glTexImage2D(847x480, data=NULL)` is immediately followed by a matching
// glRenderbufferStorage for the depth attachment, i.e. the game renders its 3D
// scene off-screen and composites the result. Those sizes are 847x480 and
// 423x240 -- both width%8 == 7 -- while every surface that draws CORRECTLY is
// power-of-two. vitaGL allocates texture memory at VGL_ALIGN(w,8) stride, so a
// 847-wide target is backed by an 848-wide surface; if the render/sample paths
// disagree about which width is authoritative, every row slips by up to 7 pixels
// and the error accumulates down the image -- exactly the diagonal shear on screen.
// Round render-target dimensions up to a multiple of 8 so allocation, rendering
// and sampling all agree. Colour texture and depth renderbuffer must be padded
// TOGETHER or the FBO becomes dimension-incomplete. The game keeps its own
// viewport (847 wide) inside the slightly larger surface, so the only cost is
// ~0.1% of UV scale -- invisible next to the shear it replaces.

static void glRenderbufferStorage_e(GLenum tg, GLenum ifmt, GLsizei w, GLsizei h) {
  GLsizei aw = gl_rt_align_w(w);
  GLLOG("glRenderbufferStorage(0x%x,0x%x,%dx%d)", (unsigned)tg, (unsigned)ifmt, (int)w, (int)h);
  if (aw != w)
    log_printf("[GL] RT pad: renderbuffer %dx%d -> %dx%d", (int)w, (int)h, (int)aw, (int)h);
  glRenderbufferStorage(tg, ifmt, aw, h);
}
static void glFramebufferRenderbuffer_e(GLenum tg, GLenum at, GLenum rt, GLuint r) { GLLOG("glFramebufferRenderbuffer(at=0x%x)", (unsigned)at); glFramebufferRenderbuffer(tg, at, rt, r); }
static void glFramebufferTexture2D_e(GLenum tg, GLenum at, GLenum tt, GLuint t, GLint l) { GLLOG("glFramebufferTexture2D(at=0x%x,tex=%u)", (unsigned)at, (unsigned)t); glFramebufferTexture2D(tg, at, tt, t, l); }
static GLenum glCheckFramebufferStatus_e(GLenum tg) { GLLOG("glCheckFramebufferStatus(0x%x) ...", (unsigned)tg); GLenum s = glCheckFramebufferStatus(tg); log_printf("[GL]  -> status 0x%x", (unsigned)s); return s; }
static void glGenBuffers_e(GLsizei n, GLuint *b) { GLLOG("glGenBuffers(%d)", (int)n); glGenBuffers(n, b); }
static void glBindBuffer_e(GLenum tg, GLuint b) {
  GLLOG("glBindBuffer(0x%x,%u)", (unsigned)tg, (unsigned)b);
  if (tg == GL_ARRAY_BUFFER) g_cur_arraybuf = b;
  glBindBuffer(tg, b);
}
static void glBufferData_e(GLenum tg, GLsizeiptr sz, const void *d, GLenum u) { GLLOG("glBufferData(0x%x, %d bytes)", (unsigned)tg, (int)sz); g_bufdata_win++; glBufferData(tg, sz, d, u); }
static GLuint glCreateProgram_e(void) { GLLOG("glCreateProgram()"); GLuint p = glCreateProgram(); log_printf("[GL]  -> program %u", (unsigned)p); return p; }
/* Redundant program-switch filter.
 *
 * The Undercity issues ~480 draws and ~43 glUseProgram calls a frame at 5-8 fps,
 * against 140 draws at 30-40 fps in the streets above, and there are only ten
 * programs in the whole session -- so most of those switches re-select the
 * program that is already current.
 *
 * That is not free here the way it is on desktop GL. vitaGL's glUseProgram does
 * no GXM work at all; it sets cur_program and then marks EVERY uniform dirty
 * (dirty_vert_unifs = 0xFFFF, dirty_frag_unifs = 0xFFFFFFFF, custom_shaders.c
 * ~2462). The next draw therefore re-uploads the program's entire uniform set,
 * u_boneMatrices[51] and u_lightData[15] included, for a call that changed
 * nothing.
 *
 * Skipping it is safe, and checked against vitaGL rather than assumed:
 *   - uniforms are per-program state and persist across a re-select, so not
 *     re-marking them dirty cannot lose a value;
 *   - glUniform* flags its own slot via flag_dirty_vert_unif, so writes are
 *     still tracked while the filter is active;
 *   - gxm.c ~796 re-dirties everything at each frame end regardless ("just to be
 *     safe"), so the per-frame invalidation never depended on this call.
 * The shadow is dropped whenever a program is linked or deleted, since either
 * can change what an id means.
 * Set GL_FILTER_REDUNDANT_PROGS to 0 in config.h to rule this out. */
static void glUseProgram_e(GLuint p) {
  GLLOG("glUseProgram(%u)", (unsigned)p);
  g_prog_win++;
#if GL_FILTER_REDUNDANT_PROGS
  if (p && p == g_cur_prog) { g_prog_skipped_win++; return; }
  g_cur_prog = p;
#endif
  glUseProgram(p);
}
static void glDeleteProgram_e(GLuint p) { g_cur_prog = 0; glDeleteProgram(p); }
static void glScissor_e(GLint x, GLint y, GLsizei w, GLsizei h) { GLLOG("glScissor(%d,%d,%d,%d)", x, y, (int)w, (int)h); glScissor(x, y, w, h); }
static void glClearStencil_e(GLint s) { GLLOG("glClearStencil(%d)", s); glClearStencil(s); }
static GLenum glGetError_e(void) { GLenum e = glGetError(); GLLOG("glGetError() -> 0x%x", (unsigned)e); return e; }
static void glHint_e(GLenum t, GLenum m) { GLLOG("glHint(0x%x,0x%x)", (unsigned)t, (unsigned)m); glHint(t, m); }
static void glFrontFace_e(GLenum m) { GLLOG("glFrontFace(0x%x)", (unsigned)m); glFrontFace(m); }
static void glCullFace_e(GLenum m) { GLLOG("glCullFace(0x%x)", (unsigned)m); glCullFace(m); }
static void glDepthFunc_e(GLenum f) { GLLOG("glDepthFunc(0x%x)", (unsigned)f); glDepthFunc(f); }
static void glColorMask_e(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { GLLOG("glColorMask(%d%d%d%d)", r, g, b, a); glColorMask(r, g, b, a); }
static void glBlendFunc_e(GLenum s, GLenum d) { GLLOG("glBlendFunc(0x%x,0x%x)", (unsigned)s, (unsigned)d); glBlendFunc(s, d); }
static void glDepthMask_e(GLboolean f) { GLLOG("glDepthMask(%d)", f); glDepthMask(f); }

static const so_default_dynlib gl_dynlib[] = {
  /* (2) float-by-value shims */
  { "glClearColor",                      (uintptr_t)&glClearColor_s },
  { "glClearDepthf",                     (uintptr_t)&glClearDepthf_s },
  { "glDepthRangef",                     (uintptr_t)&glDepthRangef_s },
  { "glLineWidth",                       (uintptr_t)&glLineWidth_s },
  { "glPolygonOffset",                   (uintptr_t)&glPolygonOffset_s },
  { "glTexParameterf",                   (uintptr_t)&glTexParameterf_s },
  { "glUniform1f",                       (uintptr_t)&glUniform1f_s },
  { "glUniform2f",                       (uintptr_t)&glUniform2f_s },
  { "glUniform3f",                       (uintptr_t)&glUniform3f_s },
  { "glUniform4f",                       (uintptr_t)&glUniform4f_s },
  { "glVertexAttrib1f",                  (uintptr_t)&glVertexAttrib1f_s },
  { "glVertexAttrib2f",                  (uintptr_t)&glVertexAttrib2f_s },
  { "glVertexAttrib3f",                  (uintptr_t)&glVertexAttrib3f_s },
  { "glVertexAttrib4f",                  (uintptr_t)&glVertexAttrib4f_s },
  /* (3) gap stubs (not in vitaGL) */
  { "glBlendColor",                      (uintptr_t)&glBlendColor_g },
  { "glCompressedTexSubImage2D",         (uintptr_t)&glCompressedTexSubImage2D_g },
  { "glDetachShader",                    (uintptr_t)&glDetachShader_g },
  { "glGetRenderbufferParameteriv",      (uintptr_t)&glGetRenderbufferParameteriv_g },
  { "glGetShaderPrecisionFormat",        (uintptr_t)&glGetShaderPrecisionFormat_g },
  { "glGetTexParameterfv",               (uintptr_t)&glGetTexParameterfv_g },
  { "glGetTexParameteriv",               (uintptr_t)&glGetTexParameteriv_g },
  { "glGetUniformfv",                    (uintptr_t)&glGetUniformfv_g },
  { "glGetUniformiv",                    (uintptr_t)&glGetUniformiv_g },
  { "glIsBuffer",                        (uintptr_t)&glIsBuffer_g },
  { "glIsShader",                        (uintptr_t)&glIsShader_g },
  { "glSampleCoverage",                  (uintptr_t)&glSampleCoverage_g },
  { "glTexParameterfv",                  (uintptr_t)&glTexParameterfv_g },
  { "glValidateProgram",                 (uintptr_t)&glValidateProgram_g },
  /* (1t) traced lifecycle wrappers */
  { "glGetString",                       (uintptr_t)&glGetString_t },
  { "glCreateShader",                    (uintptr_t)&glCreateShader_t },
  { "glShaderSource",                    (uintptr_t)&glShaderSource_t },
  { "glCompileShader",                   (uintptr_t)&glCompileShader_t },
  { "glLinkProgram",                     (uintptr_t)&glLinkProgram_t },
  { "glViewport",                        (uintptr_t)&glViewport_t },
  { "glClear",                           (uintptr_t)&glClear_t },
  { "glDrawArrays",                      (uintptr_t)&glDrawArrays_t },
  { "glDrawElements",                    (uintptr_t)&glDrawElements_t },
  { "glGetIntegerv",                     (uintptr_t)&glGetIntegerv_t },
  { "glGetBooleanv",                     (uintptr_t)&glGetBooleanv_t },
  { "glGetFloatv",                       (uintptr_t)&glGetFloatv_t },
  /* (1e) entry-traced setup calls */
  { "glEnable",                          (uintptr_t)&glEnable_e },
  { "glDisable",                         (uintptr_t)&glDisable_e },
  { "glActiveTexture",                   (uintptr_t)&glActiveTexture_e },
  { "glPixelStorei",                     (uintptr_t)&glPixelStorei_e },
  { "glGenTextures",                     (uintptr_t)&glGenTextures_e },
  { "glBindTexture",                     (uintptr_t)&glBindTexture_e },
  { "glTexImage2D",                      (uintptr_t)&glTexImage2D_e },
  { "glTexParameteri",                   (uintptr_t)&glTexParameteri_e },
  { "glGenFramebuffers",                 (uintptr_t)&glGenFramebuffers_e },
  { "glBindFramebuffer",                 (uintptr_t)&glBindFramebuffer_e },
  { "glGenRenderbuffers",                (uintptr_t)&glGenRenderbuffers_e },
  { "glBindRenderbuffer",                (uintptr_t)&glBindRenderbuffer_e },
  { "glRenderbufferStorage",             (uintptr_t)&glRenderbufferStorage_e },
  { "glFramebufferRenderbuffer",         (uintptr_t)&glFramebufferRenderbuffer_e },
  { "glFramebufferTexture2D",            (uintptr_t)&glFramebufferTexture2D_e },
  { "glCheckFramebufferStatus",          (uintptr_t)&glCheckFramebufferStatus_e },
  { "glGenBuffers",                      (uintptr_t)&glGenBuffers_e },
  { "glBindBuffer",                      (uintptr_t)&glBindBuffer_e },
  { "glBufferData",                      (uintptr_t)&glBufferData_e },
  { "glCreateProgram",                   (uintptr_t)&glCreateProgram_e },
  { "glUseProgram",                      (uintptr_t)&glUseProgram_e },
  { "glScissor",                         (uintptr_t)&glScissor_e },
  { "glClearStencil",                    (uintptr_t)&glClearStencil_e },
  { "glGetError",                        (uintptr_t)&glGetError_e },
  { "glHint",                            (uintptr_t)&glHint_e },
  { "glFrontFace",                       (uintptr_t)&glFrontFace_e },
  { "glCullFace",                        (uintptr_t)&glCullFace_e },
  { "glDepthFunc",                       (uintptr_t)&glDepthFunc_e },
  { "glColorMask",                       (uintptr_t)&glColorMask_e },
  { "glBlendFunc",                       (uintptr_t)&glBlendFunc_e },
  { "glDepthMask",                       (uintptr_t)&glDepthMask_e },
  /* (1) direct maps to vitaGL */
  { "glAttachShader",                    (uintptr_t)&glAttachShader },
  { "glBindAttribLocation",              (uintptr_t)&glBindAttribLocation },
  { "glBlendEquation",                   (uintptr_t)&glBlendEquation },
  { "glBlendEquationSeparate",           (uintptr_t)&glBlendEquationSeparate },
  { "glBlendFuncSeparate",               (uintptr_t)&glBlendFuncSeparate },
  { "glBufferSubData",                   (uintptr_t)&glBufferSubData },
  // GLES OES buffer-mapping ext: identical signatures to vitaGL's core maps.
  { "glMapBufferOES",                    (uintptr_t)&glMapBuffer },
  { "glUnmapBufferOES",                  (uintptr_t)&glUnmapBuffer },
  { "glCompressedTexImage2D",            (uintptr_t)&glCompressedTexImage2D },
  { "glCopyTexImage2D",                  (uintptr_t)&glCopyTexImage2D },
  { "glCopyTexSubImage2D",               (uintptr_t)&glCopyTexSubImage2D },
  { "glDeleteBuffers",                   (uintptr_t)&glDeleteBuffers },
  { "glDeleteFramebuffers",              (uintptr_t)&glDeleteFramebuffers },
  { "glDeleteProgram",                   (uintptr_t)&glDeleteProgram_e },
  { "glDeleteRenderbuffers",             (uintptr_t)&glDeleteRenderbuffers },
  { "glDeleteShader",                    (uintptr_t)&glDeleteShader },
  { "glDeleteTextures",                  (uintptr_t)&glDeleteTextures_e },
  { "glDisableVertexAttribArray",        (uintptr_t)&glDisableVertexAttribArray },
  { "glEnableVertexAttribArray",         (uintptr_t)&glEnableVertexAttribArray },
  { "glFinish",                          (uintptr_t)&glFinish },
  { "glFlush",                           (uintptr_t)&glFlush },
  { "glGenerateMipmap",                  (uintptr_t)&glGenerateMipmap },
  { "glGetActiveAttrib",                 (uintptr_t)&glGetActiveAttrib },
  { "glGetActiveUniform",                (uintptr_t)&glGetActiveUniform },
  { "glGetAttachedShaders",              (uintptr_t)&glGetAttachedShaders },
  { "glGetAttribLocation",               (uintptr_t)&glGetAttribLocation_e },
  { "glGetBufferParameteriv",            (uintptr_t)&glGetBufferParameteriv },
  { "glGetFramebufferAttachmentParameteriv",   (uintptr_t)&glGetFramebufferAttachmentParameteriv },
  { "glGetProgramInfoLog",               (uintptr_t)&glGetProgramInfoLog },
  { "glGetProgramiv",                    (uintptr_t)&glGetProgramiv },
  { "glGetShaderInfoLog",                (uintptr_t)&glGetShaderInfoLog },
  { "glGetShaderiv",                     (uintptr_t)&glGetShaderiv },
  { "glGetShaderSource",                 (uintptr_t)&glGetShaderSource },
  { "glGetUniformLocation",              (uintptr_t)&glGetUniformLocation_e },
  { "glGetVertexAttribfv",               (uintptr_t)&glGetVertexAttribfv },
  { "glGetVertexAttribiv",               (uintptr_t)&glGetVertexAttribiv },
  { "glGetVertexAttribPointerv",         (uintptr_t)&glGetVertexAttribPointerv },
  { "glIsEnabled",                       (uintptr_t)&glIsEnabled },
  { "glIsFramebuffer",                   (uintptr_t)&glIsFramebuffer },
  { "glIsProgram",                       (uintptr_t)&glIsProgram },
  { "glIsRenderbuffer",                  (uintptr_t)&glIsRenderbuffer },
  { "glIsTexture",                       (uintptr_t)&glIsTexture },
  { "glReadPixels",                      (uintptr_t)&glReadPixels },
  { "glReleaseShaderCompiler",           (uintptr_t)&glReleaseShaderCompiler_t },
  { "glShaderBinary",                    (uintptr_t)&glShaderBinary },
  { "glStencilFunc",                     (uintptr_t)&glStencilFunc },
  { "glStencilFuncSeparate",             (uintptr_t)&glStencilFuncSeparate },
  { "glStencilMask",                     (uintptr_t)&glStencilMask },
  { "glStencilMaskSeparate",             (uintptr_t)&glStencilMaskSeparate },
  { "glStencilOp",                       (uintptr_t)&glStencilOp },
  { "glStencilOpSeparate",               (uintptr_t)&glStencilOpSeparate },
  { "glTexParameteriv",                  (uintptr_t)&glTexParameteriv },
  { "glTexSubImage2D",                   (uintptr_t)&glTexSubImage2D },
  { "glUniform1fv",                      (uintptr_t)&glUniform1fv },
  { "glUniform1i",                       (uintptr_t)&glUniform1i },
  { "glUniform1iv",                      (uintptr_t)&glUniform1iv },
  { "glUniform2fv",                      (uintptr_t)&glUniform2fv },
  { "glUniform2i",                       (uintptr_t)&glUniform2i },
  { "glUniform2iv",                      (uintptr_t)&glUniform2iv },
  { "glUniform3fv",                      (uintptr_t)&glUniform3fv },
  { "glUniform3i",                       (uintptr_t)&glUniform3i },
  { "glUniform3iv",                      (uintptr_t)&glUniform3iv },
  { "glUniform4fv",                      (uintptr_t)&glUniform4fv_e },
  { "glUniform4i",                       (uintptr_t)&glUniform4i },
  { "glUniform4iv",                      (uintptr_t)&glUniform4iv },
  { "glUniformMatrix2fv",                (uintptr_t)&glUniformMatrix2fv },
  { "glUniformMatrix3fv",                (uintptr_t)&glUniformMatrix3fv },
  { "glUniformMatrix4fv",                (uintptr_t)&glUniformMatrix4fv },
  { "glVertexAttrib1fv",                 (uintptr_t)&glVertexAttrib1fv },
  { "glVertexAttrib2fv",                 (uintptr_t)&glVertexAttrib2fv },
  { "glVertexAttrib3fv",                 (uintptr_t)&glVertexAttrib3fv },
  { "glVertexAttrib4fv",                 (uintptr_t)&glVertexAttrib4fv },
  { "glVertexAttribPointer",             (uintptr_t)&glVertexAttribPointer_e },
};

const int gl_dynlib_size = sizeof(gl_dynlib);
const so_default_dynlib *gl_get_dynlib(void) { return gl_dynlib; }
