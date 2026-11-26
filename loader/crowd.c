/* crowd.c -- see crowd.h */

#include <vitasdk.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "crowd.h"
#include "log.h"
#include "pcsamp.h"
#include "so_util.h"

extern so_module kotor_mod;

int g_crowd_dist = CROWD_DISTANCE_M;
int g_crowd_census = 0;
char g_crowd_classes[96];

typedef uint32_t (*fn1_t)(void *);
typedef uint32_t (*fn2_t)(void *, uint32_t);

static fn1_t g_cam_orig, g_dyn_orig;
static fn2_t g_gob_orig;

static int g_pos_off = -1;           /* Gob's position Vector, bytes from `this` */
static void *volatile g_cam;         /* the camera of the scene being drawn */
static int g_cam_ok;                 /* g_cam derives from Gob */
static const void *g_cam_ti;         /* the camera class last checked */
static SceUID g_render_thid;
static int g_in_dyn, g_gob_depth;
static float g_cam_pos[3];
static float g_cull_sq;

/* ---- reading the module's own code and type information ---------------- */

static int in_module(uintptr_t p) {
  return (p - kotor_mod.text_base < kotor_mod.text_size) ||
         (p - kotor_mod.data_base < kotor_mod.data_size);
}

/* A typeinfo object is { vptr, const char *name, ... }. */
static const char *ti_name(const uint32_t *ti) {
  if (!in_module((uintptr_t)ti) || ((uintptr_t)ti & 3)) return NULL;
  const char *n = (const char *)(uintptr_t)ti[1];
  if (!in_module((uintptr_t)n)) return NULL;
  if (!((n[0] >= '1' && n[0] <= '9') || n[0] == 'N')) return NULL;
  return n;
}

static const uint32_t *ti_of(const void *obj) {
  if (!obj || ((uintptr_t)obj & 3)) return NULL;
  uintptr_t vt = *(const uintptr_t *)obj;
  if (!in_module(vt) || (vt & 3)) return NULL;
  const uint32_t *ti = (const uint32_t *)(uintptr_t)((const uint32_t *)vt)[-1];
  return ti_name(ti) ? ti : NULL;
}

/* Itanium ABI: __si_class_type_info adds one base pointer at [2];
 * __vmi_class_type_info adds flags [2], base count [3], then {base, offset}
 * pairs. A flags word is tiny and a pointer is not, which tells them apart.
 * A class with no base has neither; whatever follows it fails the checks. */
static int derives(const uint32_t *ti, const char *want, int depth) {
  const char *n = ti_name(ti);
  if (!n || depth > 8) return 0;
  if (!strcmp(n, want)) return 1;
  uint32_t w2 = ti[2];
  if (w2 < 0x10) {
    uint32_t cnt = ti[3];
    if (cnt == 0 || cnt > 8) return 0;
    for (uint32_t i = 0; i < cnt; i++)
      if (derives((const uint32_t *)(uintptr_t)ti[4 + 2 * i], want, depth + 1)) return 1;
    return 0;
  }
  return derives((const uint32_t *)(uintptr_t)w2, want, depth + 1);
}

/* Gob::SetPosition(Vector). The .so is softfp, so the Vector travels in core
 * registers, in one of two shapes:
 *   void   form: `this` r0, x y z in r1 r2 r3;
 *   Vector form (it returns the new position): result pointer r0, `this` r1,
 *                x y in r2 r3, z on the stack at [sp] -- the libKOTOR build
 *                on hardware is this one (Nov 26 log: stores at r1+120..128).
 * Follow the base registers derived from `this` through MOV/ADD, note which
 * register holds which component, and record where each one is stored.
 * Returns the offset of x when y and z land right after it, else -1. */
static int scan_pos(const uint16_t *c, int this_reg, const int arg_comp[4]) {
  int base[16], comp[16], pos[3] = { -1, -1, -1 };
  for (int i = 0; i < 16; i++) { base[i] = -1; comp[i] = -1; }
  base[this_reg] = 0;
  for (int r = 0; r < 4; r++) comp[r] = arg_comp[r];
#define STORE(rt, at) do { int k_ = comp[(rt)]; if (k_ >= 0 && pos[k_] < 0) pos[k_] = (at); } while (0)
  for (int i = 0; i < 48; i++) {
    uint16_t h = c[i];
    if (h == 0x4770 || (h & 0xFF00) == 0xBD00) break;           /* bx lr / pop {..pc} */
    if (h >= 0xE800) {
      uint16_t h2 = c[++i];
      if ((h & 0xFFF0) == 0xF8C0) {                              /* STR.W Rt,[Rn,#imm12] */
        int rn = h & 0xF, rt = h2 >> 12;
        if (base[rn] >= 0) STORE(rt, base[rn] + (h2 & 0xFFF));
      } else if (h == 0xF8DD) {                                  /* LDR.W Rt,[SP,#imm12] */
        int rt = h2 >> 12;
        comp[rt] = (h2 & 0xFFF) == 0 && arg_comp[0] == -2 ? 2 : -1;
        base[rt] = -1;
      } else if ((h & 0xFFF0) == 0xE9C0) {                       /* STRD Rt,Rt2,[Rn,#imm8*4] */
        int rn = h & 0xF, rt = h2 >> 12, rt2 = (h2 >> 8) & 0xF, im = (h2 & 0xFF) * 4;
        if (base[rn] >= 0) { STORE(rt, base[rn] + im); STORE(rt2, base[rn] + im + 4); }
      } else if ((h & 0xFFD0) == 0xE880) {                       /* STM.W Rn,{list} */
        int rn = h & 0xF, at = base[rn];
        if (at >= 0)
          for (int r = 0; r < 16; r++)
            if (h2 & (1u << r)) { STORE(r, at); at += 4; }
      } else if ((h & 0xFBE0) == 0xF100 || (h & 0xFBF0) == 0xF200) { /* ADD.W / ADDW */
        int rn = h & 0xF, rd = (h2 >> 8) & 0xF;
        int imm = ((h >> 10) & 1) << 11 | ((h2 >> 12) & 7) << 8 | (h2 & 0xFF);
        comp[rd] = -1;
        if ((h & 0xFBE0) == 0xF100 && (imm >> 8)) { base[rd] = -1; continue; } /* rotated */
        base[rd] = base[rn] >= 0 ? base[rn] + imm : -1;
      } else if ((h & 0xF800) == 0xF000 && (h2 & 0x8000)) {
        break;                                                   /* a branch or call */
      }
      continue;
    }
    if ((h & 0xF800) == 0x6000) {                                /* STR Rt,[Rn,#imm5*4] */
      int rn = (h >> 3) & 7, rt = h & 7;
      if (base[rn] >= 0) STORE(rt, base[rn] + ((h >> 6) & 0x1F) * 4);
    } else if ((h & 0xF800) == 0x9800) {                         /* LDR Rt,[SP,#imm8*4] */
      int rt = (h >> 8) & 7;
      comp[rt] = (h & 0xFF) == 0 && arg_comp[0] == -2 ? 2 : -1;
      base[rt] = -1;
    } else if ((h & 0xF800) == 0xC000) {                         /* STMIA Rn!,{list} */
      int rn = (h >> 8) & 7, at = base[rn];
      if (at >= 0)
        for (int r = 0; r < 8; r++)
          if (h & (1u << r)) { STORE(r, at); at += 4; }
    } else if ((h & 0xFE00) == 0x1C00) {                         /* ADDS Rd,Rn,#imm3 */
      int rn = (h >> 3) & 7, rd = h & 7;
      base[rd] = base[rn] >= 0 ? base[rn] + ((h >> 6) & 7) : -1;
      comp[rd] = -1;
    } else if ((h & 0xF800) == 0x3000) {                         /* ADDS Rdn,#imm8 */
      int rd = (h >> 8) & 7;
      base[rd] = base[rd] >= 0 ? base[rd] + (h & 0xFF) : -1;
      comp[rd] = -1;
    } else if ((h & 0xFF00) == 0x4600) {                         /* MOV Rd,Rm */
      int rd = (h & 7) | ((h >> 4) & 8), rm = (h >> 3) & 0xF;
      base[rd] = base[rm];
      comp[rd] = comp[rm];
    } else if ((h & 0xF000) == 0xD000 || (h & 0xF800) == 0xE000 || (h & 0xF500) == 0xB100) {
      break;                                                     /* a branch */
    }
  }
#undef STORE
  if (pos[0] >= 0 && pos[1] == pos[0] + 4 && pos[2] == pos[0] + 8) return pos[0];
  return -1;
}

static int find_pos_offset(uintptr_t fn) {
  const uint16_t *c = (const uint16_t *)(fn & ~(uintptr_t)1);
  /* comp per argument register r0-r3; -2 in r0's slot marks the Vector form,
   * whose z is the first stack word. */
  static const int void_form[4] = { -1, 0, 1, 2 };
  static const int vec_form[4] = { -2, -1, 0, 1 };
  int off = scan_pos(c, 0, void_form);
  if (off < 0) off = scan_pos(c, 1, vec_form);
  if (off >= 0) return off;
  log_printf("[crowd] Gob::SetPosition not understood; code:");
  for (int i = 0; i < 32; i += 8)
    log_printf("[crowd]   %04x %04x %04x %04x %04x %04x %04x %04x", c[i], c[i + 1], c[i + 2],
               c[i + 3], c[i + 4], c[i + 5], c[i + 6], c[i + 7]);
  return -1;
}

static int read_pos(const void *gob, float out[3]) {
  const float *p = (const float *)((const char *)gob + g_pos_off);
  for (int i = 0; i < 3; i++) {
    if (!isfinite(p[i]) || fabsf(p[i]) > 1e6f) return 0;
    out[i] = p[i];
  }
  return 1;
}

/* ---- census ------------------------------------------------------------- */

#define NCLS 48
typedef struct {
  const uint32_t *ti;
  int cull;                           /* matches CrowdClasses */
  uint32_t drawn, culled;
  uint64_t us;
} cls_t;
static cls_t g_cls[NCLS];
static unsigned g_ncls;
static uint32_t g_hist[6];            /* <10, <20, <30, <50, <80, beyond (m) */
static uint32_t g_frames, g_nopos;
static uint32_t g_tot_culled;

static int class_matches(const char *name) {
  if (!g_crowd_classes[0]) return 1;
  char buf[sizeof g_crowd_classes], *save = NULL;
  strcpy(buf, g_crowd_classes);
  for (char *tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save))
    if (strstr(name, tok)) return 1;
  return 0;
}

static cls_t *class_of(const uint32_t *ti) {
  for (unsigned i = 0; i < g_ncls; i++)
    if (g_cls[i].ti == ti) return &g_cls[i];
  if (g_ncls == NCLS) return NULL;
  cls_t *c = &g_cls[g_ncls++];
  memset(c, 0, sizeof *c);
  c->ti = ti;
  c->cull = class_matches(ti_name(ti));
  return c;
}

static void census_report(void) {
  log_printf("[crowd] %s, %u frames: culled %u (beyond %d m), no position %u; drawn at <10:%u "
             "<20:%u <30:%u <50:%u <80:%u more:%u m; camera at %.1f %.1f %.1f",
             pcsamp_area(), (unsigned)g_frames, (unsigned)g_tot_culled, g_crowd_dist, (unsigned)g_nopos,
             (unsigned)g_hist[0], (unsigned)g_hist[1], (unsigned)g_hist[2], (unsigned)g_hist[3],
             (unsigned)g_hist[4], (unsigned)g_hist[5], (double)g_cam_pos[0],
             (double)g_cam_pos[1], (double)g_cam_pos[2]);
  for (unsigned i = 0; i < g_ncls; i++) {
    cls_t *c = &g_cls[i];
    if (!c->drawn && !c->culled) continue;
    log_printf("[crowd]   %-28.28s %s drawn %5.1f/frame (%5.2f ms/frame), culled %5.1f/frame",
               ti_name(c->ti), c->cull ? "*" : " ", (double)c->drawn / g_frames,
               (double)c->us / 1000.0 / g_frames, (double)c->culled / g_frames);
    c->drawn = c->culled = 0;
    c->us = 0;
  }
  memset(g_hist, 0, sizeof g_hist);
  g_frames = g_nopos = g_tot_culled = 0;
}

/* ---- hooks -------------------------------------------------------------- */

static uint32_t cam_hook(void *self) {
  g_cam = self;
  return g_cam_orig(self);
}

static uint32_t dyn_hook(void *scene) {
  g_render_thid = sceKernelGetThreadId();
  void *cam = g_cam;
  if (cam) {
    const uint32_t *ti = ti_of(cam);
    if (ti != g_cam_ti) {
      g_cam_ti = ti;
      g_cam_ok = ti && derives(ti, "3Gob", 0);
      log_printf("[crowd] camera class %s: %s", ti ? ti_name(ti) : "?",
                 g_cam_ok ? "a Gob, distances on" : "not a Gob, cull off");
    }
  }
  g_in_dyn = g_cam_ok && cam && read_pos(cam, g_cam_pos);
  g_gob_depth = 0;
  uint32_t r = g_dyn_orig(scene);
  g_in_dyn = 0;
  if (g_crowd_census && ++g_frames >= 450) census_report();
  return r;
}

static uint32_t gob_hook(void *gob, uint32_t arg) {
  if (!g_in_dyn || g_gob_depth || sceKernelGetThreadId() != g_render_thid)
    return g_gob_orig(gob, arg);
  float p[3];
  if (!read_pos(gob, p)) {
    g_nopos++;
    return g_gob_orig(gob, arg);
  }
  float dx = p[0] - g_cam_pos[0], dy = p[1] - g_cam_pos[1], dz = p[2] - g_cam_pos[2];
  float d2 = dx * dx + dy * dy + dz * dz;
  cls_t *c = NULL;
  if (g_crowd_census || g_crowd_classes[0]) {
    const uint32_t *ti = ti_of(gob);
    c = ti ? class_of(ti) : NULL;
  }
  if (g_cull_sq > 0.0f && d2 > g_cull_sq && (!c || c->cull) &&
      (c || !g_crowd_classes[0])) {
    g_tot_culled++;
    if (c) c->culled++;
    return 0;
  }
  if (!g_crowd_census) {
    g_gob_depth++;
    uint32_t r = g_gob_orig(gob, arg);
    g_gob_depth--;
    return r;
  }
  float d = sqrtf(d2);
  g_hist[d < 10 ? 0 : d < 20 ? 1 : d < 30 ? 2 : d < 50 ? 3 : d < 80 ? 4 : 5]++;
  uint64_t t0 = sceKernelGetProcessTimeWide();
  g_gob_depth++;
  uint32_t r = g_gob_orig(gob, arg);
  g_gob_depth--;
  if (c) {
    c->drawn++;
    c->us += sceKernelGetProcessTimeWide() - t0;
  }
  return r;
}

static int hook_sym(const char *sym, void *hook, void **orig) {
  uintptr_t a = so_symbol(&kotor_mod, sym);
  if (!a) { log_printf("[crowd] %s missing", sym); return 0; }
  *orig = (void *)build_thumb_trampoline(a, thumb_patch_len(a));
  if (!*orig) { log_printf("[crowd] %s: trampoline failed", sym); return 0; }
  hook_thumb(a, (uintptr_t)hook);
  return 1;
}

void crowd_install(void) {
  if (g_crowd_dist <= 0 && !g_crowd_census) {
    log_printf("[crowd] off (CrowdDistance=0, CrowdCensus=0)");
    return;
  }
  uintptr_t sp = so_symbol(&kotor_mod, "_ZN3Gob11SetPositionE6Vector");
  if (!sp) { log_printf("[crowd] Gob::SetPosition missing: off"); return; }
  g_pos_off = find_pos_offset(sp);
  if (g_pos_off < 0) { log_printf("[crowd] off: no position field"); return; }
  if (!hook_sym("_ZN6Camera11RenderSceneEv", (void *)cam_hook, (void **)&g_cam_orig) ||
      !hook_sym("_ZN5Scene21RenderDynamicGeometryEv", (void *)dyn_hook, (void **)&g_dyn_orig) ||
      !hook_sym("_ZN3Gob6RenderEb", (void *)gob_hook, (void **)&g_gob_orig)) {
    log_printf("[crowd] off: hooks incomplete");
    return;
  }
  g_cull_sq = g_crowd_dist > 0 ? (float)g_crowd_dist * (float)g_crowd_dist : 0.0f;
  log_printf("[crowd] on: position at Gob+%d, cull beyond %d m%s%s, census %s", g_pos_off,
             g_crowd_dist, g_crowd_classes[0] ? " for classes " : "", g_crowd_classes,
             g_crowd_census ? "on" : "off");
}
