/* geo_probe.c -- see geo_probe.h for what this is looking for. */

#include <vitasdk.h>
#include <string.h>

#include "config.h"
#include "geo_probe.h"
#include "log.h"

/* vitaGL's buffer object. A buffer name IS a pointer to one of these (see
 * glGenBuffers in vitaGL's buffers.c); only the leading fields are read, and
 * they are the same with or without HAVE_SCRATCH_MEMORY. */
typedef struct { void *ptr; int32_t size; } vgl_vbo_head;

#define GEO_ATTRIBS     16
#define GEO_BUF_SLOTS   16384        /* open-addressed, power of two */
#define GEO_SAMPLE_EVERY 37          /* check one draw in N ... */
#define GEO_CHECKS_PER_FRAME 4       /* ... but never more than this a frame */
#define GEO_VALUE_SAMPLES 64         /* vertices read per checked draw */
#define GEO_BAD_ABS     1.0e5f       /* no KOTOR vertex attribute is this big */
#define GEO_SWEEP_CREDIT_PER_FRAME (64 * 1024) /* background stomp sweep, ~2 MB/s */
#define GEO_SWEEP_CREDIT_MAX (4 * 1024 * 1024)
#define GEO_REHASH_MAX_SIZE (64 * 1024)         /* bigger: no rehash on map/subdata */
#define GEO_DETAIL_MAX  12           /* detail lines per category, then 1 per window */

typedef struct {
  GLuint   name;         /* 0 = empty slot, ~0u = tombstone */
  uint32_t hash;
  void    *hashed_ptr;   /* vitaGL's ptr when hash was taken */
  int32_t  size;
  GLenum   usage;
  uint32_t upload_s;     /* session seconds at last glBufferData */
  uint8_t  valid;        /* hash matches what GL was told to store */
  uint8_t  reported;     /* stomp already logged */
} geo_buf;

static geo_buf  g_bufs[GEO_BUF_SLOTS];
static unsigned g_bufs_live = 0;

static struct {
  GLint size; GLenum type; GLsizei stride; uintptr_t off; GLuint buf; uint8_t on;
} g_attr[GEO_ATTRIBS];

static GLuint   g_arraybuf = 0, g_elembuf = 0;

/* Attribute locations each program actually reads. An array left enabled by an
 * earlier draw is harmless to GL when the current shader ignores it, so judging
 * it would report layouts nothing draws with. */
#define GEO_PROGS 64
static struct { GLuint prog; uint16_t mask; } g_prog_mask[GEO_PROGS];

static uint16_t prog_mask(GLuint prog) {
  if (!prog) return 0;
  for (int i = 0; i < GEO_PROGS; i++)
    if (g_prog_mask[i].prog == prog) return g_prog_mask[i].mask;
  uint16_t mask = 0;
  GLint n = 0;
  glGetProgramiv(prog, GL_ACTIVE_ATTRIBUTES, &n);
  for (GLint a = 0; a < n; a++) {
    char name[64]; GLsizei len = 0; GLint sz; GLenum ty;
    glGetActiveAttrib(prog, (GLuint)a, sizeof(name), &len, &sz, &ty, name);
    GLint loc = glGetAttribLocation(prog, name);
    if (loc >= 0 && loc < GEO_ATTRIBS) mask |= (uint16_t)(1u << loc);
  }
  for (int i = 0; i < GEO_PROGS; i++)
    if (g_prog_mask[i].prog == 0) { g_prog_mask[i].prog = prog; g_prog_mask[i].mask = mask; break; }
  return mask;
}

void geo_note_link(GLuint prog) {
  for (int i = 0; i < GEO_PROGS; i++)
    if (g_prog_mask[i].prog == prog) g_prog_mask[i].prog = 0;
}
static unsigned g_draw_seq = 0, g_checks_frame = 0, g_sweep_pos = 0;

/* per-window counters */
static unsigned w_checked, w_oob, w_spread, w_mixed, w_badval, w_maps, w_subdata;
static unsigned w_rehashed, w_stomped, w_ptrmoved;
/* lifetime counters and detail budgets */
static unsigned t_oob, t_spread, t_mixed, t_badval, t_stomped;
static unsigned d_oob, d_spread, d_badval, d_stomp;

static uint32_t session_s(void) {
  return (uint32_t)(sceKernelGetProcessTimeWide() / 1000000u);
}

static uint32_t hash_bytes(const void *p, int32_t n) {
  const uint32_t *w = (const uint32_t *)p;
  uint32_t h = 2166136261u;
  int32_t words = n >> 2;
  for (int32_t i = 0; i < words; i++) h = (h ^ w[i]) * 16777619u;
  const uint8_t *b = (const uint8_t *)p + (words << 2);
  for (int32_t i = 0; i < (n & 3); i++) h = (h ^ b[i]) * 16777619u;
  return h;
}

static geo_buf *buf_find(GLuint name, int create) {
  if (!name) return NULL;
  unsigned i = (name >> 4) * 2654435761u & (GEO_BUF_SLOTS - 1);
  geo_buf *tomb = NULL;
  for (unsigned n = 0; n < GEO_BUF_SLOTS; n++, i = (i + 1) & (GEO_BUF_SLOTS - 1)) {
    geo_buf *b = &g_bufs[i];
    if (b->name == name) return b;
    if (b->name == ~0u) { if (!tomb) tomb = b; continue; }
    if (b->name == 0) {
      if (!create) return NULL;
      b = tomb ? tomb : b;
      memset(b, 0, sizeof(*b));
      b->name = name;
      g_bufs_live++;
      return b;
    }
  }
  return create ? tomb : NULL;
}

static GLuint bound(GLenum target) {
  return target == GL_ELEMENT_ARRAY_BUFFER ? g_elembuf :
         target == GL_ARRAY_BUFFER ? g_arraybuf : 0;
}

static void rehash_now(geo_buf *b) {
  const vgl_vbo_head *v = (const vgl_vbo_head *)b->name;
  b->valid = 0;
  if (!v->ptr || v->size <= 0 || v->size > GEO_REHASH_MAX_SIZE) return;
  b->hash = hash_bytes(v->ptr, v->size);
  b->hashed_ptr = v->ptr;
  b->size = v->size;
  b->valid = 1;
  b->reported = 0;
}

void geo_note_bind(GLenum target, GLuint buf) {
  if (target == GL_ARRAY_BUFFER) g_arraybuf = buf;
  else if (target == GL_ELEMENT_ARRAY_BUFFER) g_elembuf = buf;
}

void geo_note_attrib(GLuint index, GLint size, GLenum type, GLsizei stride,
                     const void *pointer, GLuint buf) {
  if (index >= GEO_ATTRIBS) return;
  g_attr[index].size = size;
  g_attr[index].type = type;
  g_attr[index].stride = stride;
  g_attr[index].off = (uintptr_t)pointer;
  g_attr[index].buf = buf;
}

void geo_note_enable(GLuint index, int on) {
  if (index < GEO_ATTRIBS) g_attr[index].on = (uint8_t)on;
}

/* Called AFTER vitaGL's glBufferData, so the buffer's ptr is the new one. The
 * hash is taken from the game's source bytes, which is what GL was asked to
 * store; reading them back from GPU memory would be slower and prove less. */
void geo_note_bufdata(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
  geo_buf *b = buf_find(bound(target), 1);
  if (!b) return;
  const vgl_vbo_head *v = (const vgl_vbo_head *)b->name;
  b->size = (int32_t)size;
  b->usage = usage;
  b->upload_s = session_s();
  b->reported = 0;
  b->valid = 0;
  if (data && size > 0 && v->ptr) {
    b->hash = hash_bytes(data, (int32_t)size);
    b->hashed_ptr = v->ptr;
    b->valid = 1;
  }
}

/* Sanctioned CPU writes: take a fresh hash of the result, or give up on
 * buffers too big to rehash on every write. Called after the real call. */
void geo_note_subdata(GLenum target) {
  w_subdata++;
  geo_buf *b = buf_find(bound(target), 0);
  if (b) rehash_now(b);
}

void geo_note_map(GLenum target) {
  w_maps++;
  geo_buf *b = buf_find(bound(target), 0);
  if (b) b->valid = 0;
}

void geo_note_unmap(GLenum target) {
  geo_buf *b = buf_find(bound(target), 0);
  if (b) rehash_now(b);
}

void geo_note_delete(GLsizei n, const GLuint *bufs) {
  for (GLsizei i = 0; i < n; i++) {
    geo_buf *b = buf_find(bufs[i], 0);
    if (b) { b->name = ~0u; g_bufs_live--; }
    if (bufs[i] == g_arraybuf) g_arraybuf = 0;
    if (bufs[i] == g_elembuf) g_elembuf = 0;
  }
}

/* Called only after the live hash has been compared, so the verdict names what
 * the bytes did since GL last knew them. */
static const char *verdict(geo_buf *b) {
  if (!b) return "client memory (no buffer)";
  if (!b->valid) return "unknown: buffer written via map/subdata or too big to hash";
  const vgl_vbo_head *v = (const vgl_vbo_head *)b->name;
  if (v->ptr != b->hashed_ptr) return "unknown: vitaGL moved the buffer";
  return hash_bytes(v->ptr, v->size) == b->hash
           ? "SAME bytes as uploaded -> data was bad on arrival (b)"
           : "CHANGED since upload with no GL write -> memory stomp (a)";
}

static int float_bad(const float *f, int n) {
  for (int i = 0; i < n; i++) {
    uint32_t u; memcpy(&u, &f[i], 4);
    if ((u & 0x7f800000u) == 0x7f800000u) return 1;         /* NaN / Inf */
    if (f[i] > GEO_BAD_ABS || f[i] < -GEO_BAD_ABS) return 1;
  }
  return 0;
}

/* Shared check for one draw over vertices [lo, hi], sampling values at the
 * vertex numbers in `samp`. */
static void check_draw(uint16_t mask, uint32_t lo, uint32_t hi, const uint32_t *samp,
                       int nsamp, const char *kind, GLsizei count) {
  uintptr_t omin = ~(uintptr_t)0, omax = 0;
  GLuint first_buf = 0;
  int have = 0, mixed = 0;
  w_checked++;

  for (int a = 0; a < GEO_ATTRIBS; a++) {
    if (!g_attr[a].on || !(mask & (1u << a))) continue;
    GLuint buf = g_attr[a].buf;
    if (!have) { first_buf = buf; have = 1; }
    else if (buf != first_buf) mixed = 1;
    /* only buffers we saw uploaded: a name we don't track may already be freed */
    if (!buf || !buf_find(buf, 0)) continue;

    uintptr_t off = g_attr[a].off;
    if (off < omin) omin = off;
    if (off > omax) omax = off;

    const vgl_vbo_head *v = (const vgl_vbo_head *)buf;
    GLsizei stride = g_attr[a].stride ? g_attr[a].stride : 4 * g_attr[a].size;
    unsigned esz = g_attr[a].type == GL_FLOAT ? 4u : g_attr[a].type == GL_SHORT ||
                   g_attr[a].type == GL_UNSIGNED_SHORT ? 2u : 1u;
    uint64_t end = (uint64_t)off + (uint64_t)hi * stride + esz * g_attr[a].size;
    if (v->ptr && end > (uint64_t)(uint32_t)v->size) {
      w_oob++; t_oob++;
      if (d_oob < GEO_DETAIL_MAX) {
        d_oob++;
        log_printf("[geo] OOB (c): %s count=%d reads vertex %u of attr %d "
                   "(off=0x%x stride=%d) -> byte %u, but buffer 0x%x holds %d",
                   kind, (int)count, (unsigned)hi, a, (unsigned)off, (int)stride,
                   (unsigned)end, (unsigned)buf, (int)v->size);
      }
    }

    if (g_attr[a].type != GL_FLOAT || !v->ptr) continue;
    for (int s = 0; s < nsamp; s++) {
      uint64_t at = (uint64_t)off + (uint64_t)samp[s] * stride;
      if (at + 4u * g_attr[a].size > (uint64_t)(uint32_t)v->size) continue;
      if (!float_bad((const float *)((const uint8_t *)v->ptr + at), g_attr[a].size)) continue;
      w_badval++; t_badval++;
      geo_buf *b = buf_find(buf, 0);
      if (d_badval < GEO_DETAIL_MAX) {
        const float *f = (const float *)((const uint8_t *)v->ptr + at);
        d_badval++;
        log_printf("[geo] BAD VALUE: %s attr %d vertex %u = (%g, %g, %g) in buffer 0x%x "
                   "size=%d usage=0x%x uploaded t=%us -- %s",
                   kind, a, (unsigned)samp[s], (double)f[0],
                   (double)(g_attr[a].size > 1 ? f[1] : 0.0f),
                   (double)(g_attr[a].size > 2 ? f[2] : 0.0f),
                   (unsigned)buf, (int)v->size, b ? (unsigned)b->usage : 0u,
                   b ? (unsigned)b->upload_s : 0u, verdict(b));
      }
      break;   /* one bad vertex per attribute per draw is enough */
    }
  }

  if (mixed) { w_mixed++; t_mixed++; }
  if (omax != 0 && omax > omin && omax - omin > 0xFFFFu) {
    w_spread++; t_spread++;
    if (d_spread < GEO_DETAIL_MAX) {
      d_spread++;
      log_printf("[geo] SPREAD (c): %s attribute offsets 0x%x..0x%x are more than "
                 "64 KB apart; vitaGL's 16-bit attribute offset truncates this",
                 kind, (unsigned)omin, (unsigned)omax);
    }
  }
}

static int want_check(void) {
  if (g_checks_frame >= GEO_CHECKS_PER_FRAME) return 0;
  if (++g_draw_seq % GEO_SAMPLE_EVERY) return 0;
  g_checks_frame++;
  return 1;
}

void geo_check_elements(GLuint prog, GLsizei count, GLenum type, const void *idx) {
  if (count <= 0 || !want_check()) return;
  uint16_t mask = prog_mask(prog);
  if (!mask) return;
  if (g_elembuf && !buf_find(g_elembuf, 0)) return;
  const uint8_t *base;
  if (g_elembuf) {
    const vgl_vbo_head *e = (const vgl_vbo_head *)g_elembuf;
    unsigned isz = type == GL_UNSIGNED_INT ? 4u : type == GL_UNSIGNED_SHORT ? 2u : 1u;
    if (!e->ptr || (uint64_t)(uintptr_t)idx + (uint64_t)count * isz > (uint64_t)(uint32_t)e->size) {
      w_oob++; t_oob++;
      if (d_oob < GEO_DETAIL_MAX) {
        d_oob++;
        log_printf("[geo] OOB (c): %d indices at +0x%x overrun index buffer 0x%x of %d bytes",
                   (int)count, (unsigned)(uintptr_t)idx, (unsigned)g_elembuf,
                   e->ptr ? (int)e->size : -1);
      }
      return;
    }
    base = (const uint8_t *)e->ptr + (uintptr_t)idx;
  } else {
    base = (const uint8_t *)idx;
  }

  uint32_t lo = ~0u, hi = 0, samp[GEO_VALUE_SAMPLES];
  int nsamp = 0, step = count > GEO_VALUE_SAMPLES ? count / GEO_VALUE_SAMPLES : 1;
  for (GLsizei i = 0; i < count; i++) {
    uint32_t v = type == GL_UNSIGNED_INT ? ((const uint32_t *)base)[i] :
                 type == GL_UNSIGNED_SHORT ? ((const uint16_t *)base)[i] : base[i];
    if (v < lo) lo = v;
    if (v > hi) hi = v;
    if (i % step == 0 && nsamp < GEO_VALUE_SAMPLES) samp[nsamp++] = v;
  }
  check_draw(mask, lo, hi, samp, nsamp, "drawElements", count);
}

void geo_check_arrays(GLuint prog, GLint first, GLsizei count) {
  if (count <= 0 || first < 0 || !want_check()) return;
  uint16_t mask = prog_mask(prog);
  if (!mask) return;
  uint32_t samp[GEO_VALUE_SAMPLES];
  int nsamp = 0, step = count > GEO_VALUE_SAMPLES ? count / GEO_VALUE_SAMPLES : 1;
  for (GLsizei i = 0; i < count && nsamp < GEO_VALUE_SAMPLES; i += step)
    samp[nsamp++] = (uint32_t)(first + i);
  check_draw(mask, (uint32_t)first, (uint32_t)(first + count - 1), samp, nsamp, "drawArrays", count);
}

/* Background sweep: rehash a few KB of tracked buffers each frame, round robin,
 * so a stomp shows up even on a buffer no sampled draw happens to touch. */
void geo_on_swap(void) {
  static uint32_t credit = 0;
  g_checks_frame = 0;
  credit += GEO_SWEEP_CREDIT_PER_FRAME;
  if (credit > GEO_SWEEP_CREDIT_MAX) credit = GEO_SWEEP_CREDIT_MAX;
  for (unsigned n = 0; n < 64; n++) {
    geo_buf *b = &g_bufs[g_sweep_pos];
    if (b->name != 0 && b->name != ~0u && b->valid && !b->reported) {
      const vgl_vbo_head *v = (const vgl_vbo_head *)b->name;
      /* A buffer bigger than the credit waits here until enough accrues, so a
       * 2 MB model buffer costs one read a second, not one a frame. */
      if (v->ptr && v->size == b->size && v->ptr == b->hashed_ptr &&
          (uint32_t)v->size <= GEO_SWEEP_CREDIT_MAX && (uint32_t)v->size > credit)
        break;
      if (!v->ptr || v->size != b->size || (uint32_t)v->size > GEO_SWEEP_CREDIT_MAX) {
        /* resized behind our back; the next glBufferData re-tracks it */
      } else if (v->ptr != b->hashed_ptr) {
        w_ptrmoved++; b->valid = 0;
      } else {
        credit -= (uint32_t)v->size;
        w_rehashed++;
        if (hash_bytes(v->ptr, v->size) != b->hash) {
          b->reported = 1;
          w_stomped++; t_stomped++;
          if (d_stomp < GEO_DETAIL_MAX) {
            d_stomp++;
            log_printf("[geo] STOMP (a): buffer 0x%x ptr=%p size=%d usage=0x%x uploaded t=%us "
                       "changed with no GL write", (unsigned)b->name, v->ptr, (int)v->size,
                       (unsigned)b->usage, (unsigned)b->upload_s);
          }
        }
      }
    }
    g_sweep_pos = (g_sweep_pos + 1) & (GEO_BUF_SLOTS - 1);
  }
}

void geo_window_report(void) {
  log_printf("[geo] checked %u draws: oob=%u spread=%u mixedBufs=%u badValue=%u | "
             "buffers tracked=%u rehashed=%u stomped=%u ptrMoved=%u maps=%u subData=%u | "
             "lifetime oob=%u spread=%u badValue=%u stomped=%u",
             w_checked, w_oob, w_spread, w_mixed, w_badval,
             g_bufs_live, w_rehashed, w_stomped, w_ptrmoved, w_maps, w_subdata,
             t_oob, t_spread, t_badval, t_stomped);
  w_checked = w_oob = w_spread = w_mixed = w_badval = w_maps = w_subdata = 0;
  w_rehashed = w_stomped = w_ptrmoved = 0;
  /* after the detail budget is spent, allow one more of each per window */
  if (d_oob >= GEO_DETAIL_MAX) d_oob = GEO_DETAIL_MAX - 1;
  if (d_spread >= GEO_DETAIL_MAX) d_spread = GEO_DETAIL_MAX - 1;
  if (d_badval >= GEO_DETAIL_MAX) d_badval = GEO_DETAIL_MAX - 1;
  if (d_stomp >= GEO_DETAIL_MAX) d_stomp = GEO_DETAIL_MAX - 1;
}
