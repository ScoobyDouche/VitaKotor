/* gameprof.c -- see gameprof.h */

#include <vitasdk.h>
#include <stdint.h>
#include <stdio.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "log.h"
#include "gameprof.h"

#if GAME_PROF

/* ---- the engine's own stopwatches ------------------------------------------
 * Each is a float in milliseconds, stored (not accumulated) once per frame:
 *   loop    GameUpdate, whole body                    (g_GameLoopTime)
 *   client  CClientExoApp::MainLoop -- update + render (g_ClientUpdateTime)
 *   server  CServerExoApp::MainLoop -- rules, server AI (g_ServerUpdateTime)
 *   cAI     CClientAIMaster::UpdateState              (g_AIUpdateTime)
 *   obj       its per-object AIUpdate loop            (g_AIObjectTime)
 *   mod       its CSWCModule::Update                  (g_AIModuleTime2)
 *   gui/snd the client GUI and sound updates
 * Whatever of `client` is not cAI/gui/snd is, near enough, the render pass. */
typedef struct {
  const char *sym, *tag;
  volatile float *p;
  double sum;
  float max;
} eng_t;

static eng_t g_eng[] = {
  { "g_GameLoopTime",     "loop"   },
  { "g_ClientUpdateTime", "client" },
  { "g_ServerUpdateTime", "server" },
  { "g_AIUpdateTime",     "cAI"    },
  { "g_AIObjectTime",     "obj"    },
  { "g_AIModuleTime2",    "mod"    },
  { "g_GuiUpdateTime",    "gui"    },
  { "g_SndUpdateTime",    "snd"    },
};
#define ENG_N (sizeof g_eng / sizeof g_eng[0])
static unsigned g_eng_n;

/* ---- inclusive timers on the functions one level down ----------------------
 * Every target is Thumb with a PC-free prologue over the patched bytes
 * (checked against the disassembly), so the plain trampoline is safe. A float
 * arrives in a core register because the .so is softfp, and nothing returns
 * more than a word. The pass-through carries r0-r3 plus two stack words:
 * UpdateClientGameObjectsForPlayer takes its u64 on the stack, and for every
 * other target the extra words are just the caller's stack, read and ignored.
 * AAPCS keeps sp 8-aligned at each call, so the re-pushed pair lands where
 * the callee looks for it. Recursion is timed once, at the outermost call. */
typedef uint64_t (*fn6_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef struct {
  const char *sym, *tag;
  fn6_t orig;
  unsigned depth, calls;
  uint64_t us;
} hook_t;

static hook_t g_hook[] = {
  /* client side, inside CClientAIMaster::UpdateState */
  { "_ZN12CSWCCreature8AIUpdateEv",               "cCreature"  },
  { "_ZN13CSWCPlaceable8AIUpdateEv",              "cPlaceable" },
  { "_ZN8CSWCDoor8AIUpdateEv",                    "cDoor"      },
  { "_ZN16CSWCVisualEffect8AIUpdateEv",           "cVfx"       },
  { "_ZN15CSWCSoundObject8AIUpdateEv",            "cSoundObj"  },
  { "_ZN11CSWCTrigger8AIUpdateEv",                "cTrigger"   },
  { "_ZN22CSWCAreaOfEffectObject8AIUpdateEv",     "cAoE"       },
  { "_ZN8CSWCArea6UpdateEf",                      "cArea"      },
  { "_ZN16CSWCAmbientSound6UpdateEv",             "cAmbient"   },
  /* the model/animation layer, wherever it is called from */
  { "_ZN3Gob7AnimateEf",                          "gobAnim"    },
  { "_ZN5Scene7AnimateEf",                        "sceneAnim"  },
  { "_ZN11PartEmitter16AnimateParticlesEf",       "particles"  },
  /* server side */
  { "_ZN15CServerAIMaster11UpdateStateEv",        "sAI"        },
  { "_ZN8CSWSArea8AIUpdateEv",                    "sArea"      },
  { "_ZN12CSWSCreature8AIUpdateEv",               "sCreature"  },
  { "_ZN8CSWSArea12PlotGridPathEP20CPathfindInformationy",      "gridPath"  },
  { "_ZN8CSWSArea17PlotPathPointPathEP20CPathfindInformationy", "pointPath" },
  /* log198-200: CSWCModule::Update, the "mod" stopwatch and nearly all of
   * cAI in the cities, is a short wrapper that tail-calls the module
   * camera's vtable slot 3 -- Camera::RenderScene, the whole 3D pass. */
  { "_ZN6Camera11RenderSceneEv",                  "render"     },
  /* the server work outside sAI, which is 20-40 ms of an Upper City frame */
  { "_ZN21CServerExoAppInternal32UpdateClientGameObjectsForPlayerEP10CSWSPlayeriy", "sync" },
  { "_ZN9CNetLayer21ProcessReceivedFramesEi",     "netRecv"    },
  { "_ZN21CServerExoAppInternal13UpdateMapDataEv", "mapData"   },
  { "_ZN13CSWPartyTable13UpdateMembersEi",        "party"      },
  /* and the client applying what the server sent */
  { "_ZN11CSWCMessage27HandleServerToPlayerMessageEPhm", "cMsg"  },
};
#define HOOK_N (sizeof g_hook / sizeof g_hook[0])

uint64_t g_prof_draw_us;

static inline uint64_t timed(hook_t *h, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                             uint32_t e, uint32_t f) {
  if (h->depth++) {
    uint64_t r = h->orig(a, b, c, d, e, f);
    h->depth--;
    return r;
  }
  uint64_t t0 = sceKernelGetProcessTimeWide();
  uint64_t r = h->orig(a, b, c, d, e, f);
  h->us += sceKernelGetProcessTimeWide() - t0;
  h->calls++;
  h->depth--;
  return r;
}

#define P(i) static uint64_t probe_##i(uint32_t a, uint32_t b, uint32_t c, uint32_t d, \
                                     uint32_t e, uint32_t f) \
  { return timed(&g_hook[i], a, b, c, d, e, f); }
P(0) P(1) P(2) P(3) P(4) P(5) P(6) P(7) P(8) P(9) P(10) P(11) P(12) P(13) P(14) P(15) P(16)
P(17) P(18) P(19) P(20) P(21) P(22)
#undef P
static fn6_t const g_probe[] = {
  probe_0, probe_1, probe_2, probe_3, probe_4, probe_5, probe_6, probe_7, probe_8,
  probe_9, probe_10, probe_11, probe_12, probe_13, probe_14, probe_15, probe_16,
  probe_17, probe_18, probe_19, probe_20, probe_21, probe_22,
};
_Static_assert(sizeof g_probe / sizeof g_probe[0] == HOOK_N, "one probe per hook");

void gameprof_install(void) {
  unsigned eng = 0, hooked = 0;
  for (unsigned i = 0; i < ENG_N; i++) {
    g_eng[i].p = (volatile float *)so_symbol(&kotor_mod, g_eng[i].sym);
    if (g_eng[i].p) eng++;
    else log_printf("[prof] %s missing", g_eng[i].sym);
  }
  for (unsigned i = 0; i < HOOK_N; i++) {
    uintptr_t a = so_symbol(&kotor_mod, g_hook[i].sym);
    if (!a) { log_printf("[prof] %s missing", g_hook[i].tag); continue; }
    g_hook[i].orig = (fn6_t)build_thumb_trampoline(a, thumb_patch_len(a));
    if (!g_hook[i].orig) { log_printf("[prof] %s trampoline FAILED", g_hook[i].tag); continue; }
    hook_thumb(a, (uintptr_t)g_probe[i]);
    hooked++;
  }
  log_printf("[prof] armed: %u/%u engine stopwatches, %u/%u timed functions",
             eng, (unsigned)ENG_N, hooked, (unsigned)HOOK_N);
}

void gameprof_after_update(void) {
  for (unsigned i = 0; i < ENG_N; i++) {
    if (!g_eng[i].p) continue;
    float v = *g_eng[i].p;
    g_eng[i].sum += v;
    if (v > g_eng[i].max) g_eng[i].max = v;
  }
  g_eng_n++;
}

void gameprof_window_report(void) {
  unsigned n = g_eng_n;
  if (n) {
    char b[640];
    int o = snprintf(b, sizeof b, "[prof] %u updates, engine ms avg/max:", n);
    for (unsigned i = 0; i < ENG_N && o < (int)sizeof b - 32; i++)
      o += snprintf(b + o, sizeof b - o, " %s=%.1f/%.0f", g_eng[i].tag,
                    g_eng[i].sum / n, (double)g_eng[i].max);
    log_printf("%s", b);

    /* ms per update, then calls per update; functions that did not run are left out */
    o = snprintf(b, sizeof b, "[prof]   timed ms(calls) per update:");
    for (unsigned i = 0; i < HOOK_N && o < (int)sizeof b - 40; i++) {
      if (!g_hook[i].calls) continue;
      o += snprintf(b + o, sizeof b - o, " %s=%.1f(%.0f)", g_hook[i].tag,
                    (double)g_hook[i].us / 1000.0 / n, (double)g_hook[i].calls / n);
    }
    /* time inside vitaGL's glDraw*, wherever it was called
     * from: render minus this is the engine's own CPU in the 3D pass */
    if (o < (int)sizeof b - 24)
      snprintf(b + o, sizeof b - o, " glDraw=%.1f", (double)g_prof_draw_us / 1000.0 / n);
    log_printf("%s", b);
  }
  for (unsigned i = 0; i < ENG_N; i++) { g_eng[i].sum = 0; g_eng[i].max = 0; }
  for (unsigned i = 0; i < HOOK_N; i++) { g_hook[i].us = 0; g_hook[i].calls = 0; }
  g_prof_draw_us = 0;
  g_eng_n = 0;
}

#else
uint64_t g_prof_draw_us;
void gameprof_install(void) {}
void gameprof_after_update(void) {}
void gameprof_window_report(void) {}
#endif
