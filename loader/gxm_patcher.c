/* gxm_patcher.c -- see gxm_patcher.h. Linked in with -Wl,--wrap (CMakeLists.txt),
 * so vitaGL's own calls come through here. */

#include <vitasdk.h>
#include <psp2/gxm.h>
#include <string.h>

#include "config.h"
#include "gxm_patcher.h"
#include "log.h"

int __real_sceGxmShaderPatcherCreateVertexProgram(SceGxmShaderPatcher *sp, SceGxmShaderPatcherId id,
    const SceGxmVertexAttribute *attr, unsigned int na, const SceGxmVertexStream *st,
    unsigned int ns, SceGxmVertexProgram **out);
int __real_sceGxmShaderPatcherCreateFragmentProgram(SceGxmShaderPatcher *sp, SceGxmShaderPatcherId id,
    SceGxmOutputRegisterFormat fmt, SceGxmMultisampleMode msaa, const SceGxmBlendInfo *blend,
    const SceGxmProgram *vp, SceGxmFragmentProgram **out);
int __real_sceGxmShaderPatcherForceUnregisterProgram(SceGxmShaderPatcher *sp, SceGxmShaderPatcherId id);
void __real_sceGxmSetVertexProgram(SceGxmContext *ctx, const SceGxmVertexProgram *vp);

/* A variant not bound for this many frames is fair game. The GPU runs at most
 * a few frames behind the CPU, so nothing this old can still be in flight. */
#define GXMP_IDLE_FRAMES 30
#define GXMP_SLOTS 8192              /* open-addressed, power of two */
#define GXMP_DETAIL_MAX 12           /* failure lines, then one per window */

typedef struct {
  SceGxmVertexProgram *vp;           /* NULL = empty, GXMP_TOMB = deleted */
  SceGxmShaderPatcherId id;
  uint32_t last_frame;
  uint32_t creates;
} gxmp_slot;

#define GXMP_TOMB ((SceGxmVertexProgram *)1)

static gxmp_slot g_slots[GXMP_SLOTS];
static unsigned g_live, g_tombs;
static int g_armed;
static uint32_t g_frame;
static SceGxmShaderPatcher *g_patcher;
static uint32_t g_last_gc_frame = ~0u;
static const SceGxmVertexProgram *g_last_set;
static gxmp_slot *g_last_set_slot;

/* window / lifetime counters for the report */
static unsigned w_vp_calls, w_vp_fail, w_fp_calls, w_fp_fail, w_evicted, w_gc;
static unsigned t_vp_fail, t_fp_fail, t_evicted, t_gc, t_recovered, d_fail;
static int g_vp_lasterr, g_fp_lasterr;

static unsigned slot_hash(const SceGxmVertexProgram *vp) {
  return ((uintptr_t)vp >> 4) * 2654435761u & (GXMP_SLOTS - 1);
}

static gxmp_slot *slot_find(const SceGxmVertexProgram *vp) {
  unsigned h = slot_hash(vp);
  for (unsigned n = 0; n < GXMP_SLOTS; n++, h = (h + 1) & (GXMP_SLOTS - 1)) {
    gxmp_slot *s = &g_slots[h];
    if (s->vp == vp) return s;
    if (!s->vp) return NULL;
  }
  return NULL;
}

static gxmp_slot *slot_insert(SceGxmVertexProgram *vp) {
  unsigned h = slot_hash(vp);
  for (unsigned n = 0; n < GXMP_SLOTS; n++, h = (h + 1) & (GXMP_SLOTS - 1)) {
    gxmp_slot *s = &g_slots[h];
    if (!s->vp || s->vp == GXMP_TOMB) {
      if (s->vp == GXMP_TOMB) g_tombs--;
      s->vp = vp;
      g_live++;
      return s;
    }
  }
  return NULL;
}

static void slot_kill(gxmp_slot *s) {
  s->vp = GXMP_TOMB;
  g_live--;
  g_tombs++;
  if (s == g_last_set_slot) { g_last_set = NULL; g_last_set_slot = NULL; }
}

/* Tombstones make misses walk further; rebuild once they pile up. */
static void slot_compact(void) {
  static gxmp_slot tmp[GXMP_SLOTS];
  unsigned n = 0;
  for (unsigned i = 0; i < GXMP_SLOTS; i++)
    if (g_slots[i].vp && g_slots[i].vp != GXMP_TOMB) tmp[n++] = g_slots[i];
  memset(g_slots, 0, sizeof(g_slots));
  g_live = g_tombs = 0;
  g_last_set = NULL;
  g_last_set_slot = NULL;
  for (unsigned i = 0; i < n; i++) {
    gxmp_slot *s = slot_insert(tmp[i].vp);
    s->id = tmp[i].id;
    s->last_frame = tmp[i].last_frame;
    s->creates = tmp[i].creates;
  }
}

/* Give back every variant no draw has bound lately. Only variants vitaGL has
 * asked for more than once are released: the per-draw path re-creates its
 * program on every draw, so anything it uses gets there immediately, while a
 * program cached once and re-bound from a stored pointer (the fixed-function
 * path) never does, and must not be freed out from under that pointer. */
static unsigned evict_idle(void) {
  unsigned freed = 0;
  for (unsigned i = 0; i < GXMP_SLOTS; i++) {
    gxmp_slot *s = &g_slots[i];
    if (!s->vp || s->vp == GXMP_TOMB) continue;
    if (s->creates < 2 || g_frame - s->last_frame < GXMP_IDLE_FRAMES) continue;
    sceGxmShaderPatcherReleaseVertexProgram(g_patcher, s->vp);
    slot_kill(s);
    freed++;
  }
  if (g_tombs > GXMP_SLOTS / 4) slot_compact();
  return freed;
}

/* Record a successful create. The patcher returns the same program for the
 * same request with its refcount bumped; drop that extra reference so every
 * variant sits at exactly one, and one release frees it. */
static void track(SceGxmShaderPatcher *sp, SceGxmShaderPatcherId id, SceGxmVertexProgram *vp) {
  gxmp_slot *s = slot_find(vp);
  if (s) {
    sceGxmShaderPatcherReleaseVertexProgram(sp, vp);
  } else {
    if (g_live + g_tombs >= GXMP_SLOTS * 3 / 4) {
      slot_compact();
      if (g_live >= GXMP_SLOTS * 3 / 4) return;   /* table full: leave it unmanaged */
    }
    s = slot_insert(vp);
    s->id = id;
    s->creates = 0;
  }
  s->creates++;
  s->last_frame = g_frame;
}

int __wrap_sceGxmShaderPatcherCreateVertexProgram(SceGxmShaderPatcher *sp, SceGxmShaderPatcherId id,
    const SceGxmVertexAttribute *attr, unsigned int na, const SceGxmVertexStream *st,
    unsigned int ns, SceGxmVertexProgram **out) {
  int r = __real_sceGxmShaderPatcherCreateVertexProgram(sp, id, attr, na, st, ns, out);
  g_patcher = sp;
  w_vp_calls++;
  /* one sweep a frame: a full pool fails every draw, and a sweep that found
   * nothing idle will not find anything more until frames pass */
  if (r < 0 && g_armed && g_last_gc_frame != g_frame) {
    g_last_gc_frame = g_frame;
    unsigned freed = evict_idle();
    w_gc++; t_gc++;
    w_evicted += freed; t_evicted += freed;
    if (freed) {
      int r2 = __real_sceGxmShaderPatcherCreateVertexProgram(sp, id, attr, na, st, ns, out);
      if (r2 >= 0) t_recovered++;
      if (d_fail < GXMP_DETAIL_MAX) {
        d_fail++;
        log_printf("[gxmp] vertex program create failed 0x%08x (%u attrs, %u streams): "
                   "released %u idle variants, retry %s (usse now %u KB)",
                   (unsigned)r, na, ns, freed, r2 >= 0 ? "OK" : "FAILED",
                   sceGxmShaderPatcherGetVertexUsseMemAllocated(sp) >> 10);
      }
      r = r2;
    }
  }
  if (r < 0) {
    w_vp_fail++; t_vp_fail++; g_vp_lasterr = r;
    if (d_fail < GXMP_DETAIL_MAX) {
      d_fail++;
      log_printf("[gxmp] VERTEX PROGRAM CREATE FAILED 0x%08x: %u attrs, %u streams, stride0=%u "
                 "-- vitaGL draws with its previous program (lifetime fails %u)",
                 (unsigned)r, na, ns, ns ? (unsigned)st[0].stride : 0u, t_vp_fail);
    }
  } else if (g_armed && out && *out) {
    track(sp, id, *out);
  }
  return r;
}

void __wrap_sceGxmSetVertexProgram(SceGxmContext *ctx, const SceGxmVertexProgram *vp) {
  if (g_armed) {
    if (vp == g_last_set && g_last_set_slot) {
      g_last_set_slot->last_frame = g_frame;
    } else {
      gxmp_slot *s = slot_find(vp);
      if (s) s->last_frame = g_frame;
      g_last_set = vp;
      g_last_set_slot = s;
    }
  }
  __real_sceGxmSetVertexProgram(ctx, vp);
}

/* Deleting a shader frees all its variants inside the patcher; forget them so
 * a later eviction does not release a dangling pointer. */
int __wrap_sceGxmShaderPatcherForceUnregisterProgram(SceGxmShaderPatcher *sp, SceGxmShaderPatcherId id) {
  if (g_armed) {
    for (unsigned i = 0; i < GXMP_SLOTS; i++) {
      gxmp_slot *s = &g_slots[i];
      if (s->vp && s->vp != GXMP_TOMB && s->id == id) slot_kill(s);
    }
  }
  return __real_sceGxmShaderPatcherForceUnregisterProgram(sp, id);
}

int __wrap_sceGxmShaderPatcherCreateFragmentProgram(SceGxmShaderPatcher *sp, SceGxmShaderPatcherId id,
    SceGxmOutputRegisterFormat fmt, SceGxmMultisampleMode msaa, const SceGxmBlendInfo *blend,
    const SceGxmProgram *vp, SceGxmFragmentProgram **out) {
  int r = __real_sceGxmShaderPatcherCreateFragmentProgram(sp, id, fmt, msaa, blend, vp, out);
  g_patcher = sp;
  w_fp_calls++;
  if (r < 0) {
    w_fp_fail++; t_fp_fail++; g_fp_lasterr = r;
    if (d_fail < GXMP_DETAIL_MAX) {
      d_fail++;
      log_printf("[gxmp] FRAGMENT PROGRAM CREATE FAILED 0x%08x (lifetime fails %u)",
                 (unsigned)r, t_fp_fail);
    }
  }
  return r;
}

void gxmp_arm(void) {
  g_armed = 1;
}

void gxmp_on_swap(void) {
  g_frame++;
}

void gxmp_window_report(void) {
  if (!g_patcher) return;
  log_printf("[gxmp] vp calls=%u fail=%u (lifetime %u, last 0x%x) variants=%u | gc=%u evicted=%u "
             "(lifetime gc=%u evicted=%u recovered=%u) | fp calls=%u fail=%u (lifetime %u, last 0x%x) | "
             "mem host=%u KB buffer=%u KB vertexUsse=%u/%u KB fragmentUsse=%u/%u KB",
             w_vp_calls, w_vp_fail, t_vp_fail, (unsigned)g_vp_lasterr, g_live,
             w_gc, w_evicted, t_gc, t_evicted, t_recovered,
             w_fp_calls, w_fp_fail, t_fp_fail, (unsigned)g_fp_lasterr,
             sceGxmShaderPatcherGetHostMemAllocated(g_patcher) >> 10,
             sceGxmShaderPatcherGetBufferMemAllocated(g_patcher) >> 10,
             sceGxmShaderPatcherGetVertexUsseMemAllocated(g_patcher) >> 10,
             GXMP_VERTEX_USSE_MEM >> 10,
             sceGxmShaderPatcherGetFragmentUsseMemAllocated(g_patcher) >> 10,
             GXMP_FRAGMENT_USSE_MEM >> 10);
  w_vp_calls = w_vp_fail = w_fp_calls = w_fp_fail = w_gc = w_evicted = 0;
  if (d_fail >= GXMP_DETAIL_MAX) d_fail = GXMP_DETAIL_MAX - 1;
}
