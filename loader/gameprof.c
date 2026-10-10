/* gameprof.c -- see gameprof.h */

#include <vitasdk.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
  uint64_t frame_us;     /* this frame only, for the per-hitch blame line */
} hook_t;

static hook_t g_hook[] = {
  /* client side, inside CClientAIMaster::UpdateState. The per-object timers
   * (creature/placeable/door/... AIUpdate, Gob::Animate, particles) were
   * dropped after log208: hundreds of calls a frame, and their answer is in. */
  { "_ZN8CSWCArea6UpdateEf",                      "cArea"      },
  { "_ZN16CSWCAmbientSound6UpdateEv",             "cAmbient"   },
  { "_ZN5Scene7AnimateEf",                        "sceneAnim"  },
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
  /* log204: inside render, vitaGL's draws are only a third; the rest is the
   * engine. One call each a frame. log208 split the per-mesh path below
   * these (engine ~13 ms, port setup ~3, vitaGL ~10 of ~700 draws); those
   * timers cost ~17 ms a frame themselves and are gone. */
  { "_ZN5Scene6RenderEv",                         "scRender"   },
  { "_ZN5Scene16RenderSinglePassEv",              "singlePass" },
  { "_ZN5Scene20RenderStaticGeometryEv",          "staticGeo"  },
  { "_ZN5Scene21RenderDynamicGeometryEv",         "dynGeo"     },
  { "_Z14ManageSceneBSPP5Scene",                  "bsp"        },
  { "_ZN5Scene13RenderShadowsEiiii",              "shadows"    },
  /* log204: cMsg is 32 ms of an Upper City South frame -- as much as render.
   * Which kind of object update it is. */
  { "_ZN11CSWCMessage36HandleServerToPlayerGameObjectUpdateEh",            "objUpd"   },
  { "_ZN11CSWCMessage33HandleServerToPlayerUpdate_UpdateEv",               "upd"      },
  { "_ZN11CSWCMessage41HandleServerToPlayerCreatureUpdate_UpdateEmm",      "creUpd"   },
  { "_ZN11CSWCMessage42HandleServerToPlayerPlaceableUpdate_UpdateEmm",     "plcUpd"   },
  { "_ZN11CSWCMessage37HandleServerToPlayerDoorUpdate_UpdateEmm",          "doorUpd"  },
  { "_ZN11CSWCMessage46HandleServerToPlayerGenericObjectUpdate_UpdateEmm", "genUpd"   },
  { "_ZN11CSWCMessage30HandleServerToPlayerUpdate_AddEv",                  "updAdd"   },
  { "_ZN11CSWCMessage33HandleServerToPlayerUpdate_DeleteEv",               "updDel"   },
  { "_ZN11CSWCMessage37HandleServerToPlayerUpdate_AppearanceEv",           "updApp"   },
  { "_ZN11CSWCMessage38HandleServerToPlayerUpdate_GuiElementsEv",          "updGui"   },
  { "_ZN11CSWCMessage37HandleServerToPlayerUpdate_PlayerInfoEv",           "updPlayer"},
  { "_ZN11CSWCMessage39HandleServerToPlayerUpdate_GuiInventoryEv",         "updInv"   },
  { "_ZN11CSWCMessage39HandleServerToPlayerUpdateVisualEffectsEP10CSWCObject", "updVfx" },
  /* log237: every L/R tab switch in the in-game menu freezes ~1.1 s with no
   * card read, no model built and no texture upload, and Cross on the equip
   * screen ~1.5 s. These are the menu's entry points; the hitch blame line
   * says which of them the time was in. Prologues checked against the
   * disassembly; SetActiveControlID, Equip::SetActiveSlot, AttachModel,
   * Options::OnPanelAdded, GuiManager::AddPanel/RemovePanel and
   * 3DSceneView::Draw branch or read pc in the patched bytes and are left out. */
  { "_ZN16CSWGuiInGameMenu14OnShoulderLeftEP13CSWGuiControl",  "menuL"        },
  { "_ZN16CSWGuiInGameMenu15OnShoulderRightEP13CSWGuiControl", "menuR"        },
  { "_ZN17CSWGuiInGameEquip12OnPanelAddedEv",                  "equipAdd"     },
  { "_ZN17CSWGuiInGameEquip14OnPanelRemovedEv",                "equipRemove"  },
  { "_ZN17CSWGuiInGameEquip12SetCharacterEP12CSWCCreature",    "equipSetChar" },
  { "_ZN17CSWGuiInGameEquip15UpdateInventoryEv",               "equipInv"     },
  { "_ZN17CSWGuiInGameEquip11OnEnterSlotEP13CSWGuiControl",    "equipEnter"   },
  { "_ZN17CSWGuiInGameEquip14OnItemSelectedEP13CSWGuiControl", "equipItemSel" },
  { "_ZN17CSWGuiInGameEquip9EquipItemEP8CSWSItemii",           "equipItem"    },
  { "_ZN17CSWGuiInGameEquip11UnequipItemEmi",                  "unequipItem"  },
  { "_ZN17CSWGuiInGameEquip6UpdateEf",                         "equipUpdate"  },
  { "_ZN21CSWGuiInGameCharacter12OnPanelAddedEv",              "charAdd"      },
  { "_ZN21CSWGuiInGameCharacter14OnPanelRemovedEv",            "charRemove"   },
  { "_ZN21CSWGuiInGameCharacter8SetStatsEv",                   "charStats"    },
  { "_ZN21CSWGuiInGameInventory12OnPanelAddedEv",              "invAdd"       },
  { "_ZN21CSWGuiInGameInventory14OnPanelRemovedEv",            "invRemove"    },
  { "_ZN21CSWGuiInGameInventory19PopulateItemListBoxEv",       "invPopulate"  },
  { "_ZN21CSWGuiInGameAbilities12OnPanelAddedEv",              "abiAdd"       },
  { "_ZN19CSWGuiInGameJournal12OnPanelAddedEv",                "jouAdd"       },
  { "_ZN15CSWGuiInGameMap12OnPanelAddedEv",                    "mapAdd"       },
  { "_ZN20CSWGuiInGameMessages12OnPanelAddedEv",               "msgAdd"       },
  /* log238: equip hovers re-create 18-29 textures (up to 3 MB) and inventory
   * 89 icons (1.3 MB) every time, ~5 ms a texture with the bytes already in
   * RAM; and the equip tab's ~1 s is outside every menu hook above. The
   * texture path from resource to GL, and the per-frame GUI draw/update. */
  { "_ZN16CAurTextureBasic4InitEPc",                           "texInit"      },
  { "_ZN16CAurTextureBasic9LoadImageEv",                       "texLoadImage" },
  { "_ZN16CAurTextureBasic7glImageEb",                         "texGlImage"   },
  { "_ZN11CAurTexture7glImageEbPh",                            "texGlImage2"  },
  { "_ZN7CResTGA18OnResourceServicedEv",                       "tgaServiced"  },
  { "_ZN7CResTGA18ReadUnmappedRLETGAEv",                       "tgaRLE"       },
  { "_ZN7CResTGA21ReadColorMappedRLETGAEv",                    "tgaRLEmap"    },
  { "_ZN7CResTPC18OnResourceServicedEv",                       "tpcServiced"  },
  { "_ZN13CSWGuiManager4DrawEf",                               "guiDraw"      },
  { "_ZN13CSWGuiManager6UpdateEf",                             "guiUpdate"    },
  { "_ZN13CSWGuiManager16HandleInputEventEii",                 "guiInput"     },
  { "_ZN16CSWGuiInGameMenu16HandleInputEventEii",              "menuInput"    },
  { "_ZN17CSWGuiInGameEquip16HandleInputEventEii",             "equipInput"   },
  { "_ZN17CSWGuiInGameEquip4DrawEf",                           "equipDraw"    },
  { "_ZN21CSWGuiInGameCharacter4DrawEf",                       "charDraw"     },
  { "_ZN21CSWGuiInGameInventory4DrawEf",                       "invDraw"      },
  { "_ZN21CSWGuiInGameInventory15CreateItemEntryEP12CSWCCreatureRiR13CExoArrayListIP13CSWGuiControlEP8CSWSItemii", "invEntry" },
};
#define HOOK_N (sizeof g_hook / sizeof g_hook[0])

uint64_t g_prof_draw_us;
uint64_t g_prof_tex_frame_us;
unsigned g_prof_tex_frame_n;

static inline uint64_t timed(hook_t *h, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                             uint32_t e, uint32_t f) {
  if (h->depth++) {
    uint64_t r = h->orig(a, b, c, d, e, f);
    h->depth--;
    return r;
  }
  uint64_t t0 = sceKernelGetProcessTimeWide();
  uint64_t r = h->orig(a, b, c, d, e, f);
  uint64_t dt = sceKernelGetProcessTimeWide() - t0;
  h->us += dt;
  h->frame_us += dt;
  h->calls++;
  h->depth--;
  return r;
}

#define P(i) static uint64_t probe_##i(uint32_t a, uint32_t b, uint32_t c, uint32_t d, \
                                     uint32_t e, uint32_t f) \
  { return timed(&g_hook[i], a, b, c, d, e, f); }
P(0) P(1) P(2) P(3) P(4) P(5) P(6) P(7) P(8) P(9) P(10) P(11) P(12) P(14) P(15) P(16)
P(17) P(18) P(19) P(20) P(21) P(22) P(23) P(24) P(25) P(26) P(27) P(28) P(29) P(30) P(31)
P(32) P(33) P(34) P(35) P(36) P(37) P(38) P(39) P(40) P(41) P(42) P(43) P(44) P(45) P(46)
P(47) P(48) P(49) P(50) P(51) P(52) P(53) P(54) P(55) P(56) P(57) P(58) P(59) P(60) P(61)
P(62) P(63) P(64) P(65) P(66) P(67) P(68) P(69) P(70)
#undef P

/* log205: cMsg is 20-25 ms a frame in the cities but object updates are ~6 of
 * it, and it runs less than once a frame -- one message type is costing 40+ ms
 * a call. The buffer is 'P', major, minor, payload (CSWCMessage::
 * HandleServerToPlayerMessage reads exactly those three bytes and switches on
 * the major), so time it per type. */
#define CMSG_HOOK 13
static uint64_t g_cmsg_us[64][32];
static unsigned g_cmsg_n[64][32];

static uint64_t probe_cmsg(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                           uint32_t e, uint32_t f) {
  const unsigned char *m = (const unsigned char *)(uintptr_t)b;
  int outer = g_hook[CMSG_HOOK].depth == 0;
  uint64_t t0 = sceKernelGetProcessTimeWide();
  uint64_t r = timed(&g_hook[CMSG_HOOK], a, b, c, d, e, f);
  if (outer && m && c >= 3) {
    unsigned maj = m[1] & 63, min = m[2] & 31;
    g_cmsg_us[maj][min] += sceKernelGetProcessTimeWide() - t0;
    g_cmsg_n[maj][min]++;
  }
  return r;
}

static fn6_t const g_probe[] = {
  probe_0, probe_1, probe_2, probe_3, probe_4, probe_5, probe_6, probe_7, probe_8,
  probe_9, probe_10, probe_11, probe_12, probe_cmsg, probe_14, probe_15, probe_16,
  probe_17, probe_18, probe_19, probe_20, probe_21, probe_22, probe_23, probe_24,
  probe_25, probe_26, probe_27, probe_28, probe_29, probe_30, probe_31, probe_32,
  probe_33, probe_34, probe_35, probe_36, probe_37, probe_38, probe_39, probe_40,
  probe_41, probe_42, probe_43, probe_44, probe_45, probe_46, probe_47, probe_48,
  probe_49, probe_50, probe_51, probe_52, probe_53, probe_54, probe_55, probe_56,
  probe_57, probe_58, probe_59, probe_60, probe_61, probe_62, probe_63, probe_64,
  probe_65, probe_66, probe_67, probe_68, probe_69, probe_70,
};
_Static_assert(sizeof g_probe / sizeof g_probe[0] == HOOK_N, "one probe per hook");

void gameprof_install(void) {
  unsigned eng = 0, hooked = 0;
  if (strcmp(g_hook[CMSG_HOOK].tag, "cMsg")) {
    log_printf("[prof] CMSG_HOOK is %s, not cMsg -- per-type timing would be wrong; "
               "profiler not armed", g_hook[CMSG_HOOK].tag);
    return;
  }
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

    /* ms per update, then calls per update; functions that did not run are left
     * out. Wrapped so no line runs off the end of the buffer. */
    o = snprintf(b, sizeof b, "[prof]   timed ms(calls) per update:");
    for (unsigned i = 0; i < HOOK_N; i++) {
      if (!g_hook[i].calls) continue;
      if (o > (int)sizeof b - 48) {
        log_printf("%s", b);
        o = snprintf(b, sizeof b, "[prof]     ...");
      }
      o += snprintf(b + o, sizeof b - o, " %s=%.1f(%.0f)", g_hook[i].tag,
                    (double)g_hook[i].us / 1000.0 / n, (double)g_hook[i].calls / n);
    }
    /* time inside vitaGL's glDraw*, wherever it was called
     * from: render minus this is the engine's own CPU in the 3D pass */
    if (o < (int)sizeof b - 24)
      snprintf(b + o, sizeof b - o, " glDraw=%.1f", (double)g_prof_draw_us / 1000.0 / n);
    log_printf("%s", b);

    /* the eight most expensive message types this window, as major.minor */
    o = snprintf(b, sizeof b, "[prof]   cMsg by type ms(calls) per update:");
    for (int k = 0; k < 8; k++) {
      unsigned bm = 0, bn = 0;
      uint64_t best = 0;
      for (unsigned M = 0; M < 64; M++)
        for (unsigned N = 0; N < 32; N++)
          if (g_cmsg_us[M][N] > best) { best = g_cmsg_us[M][N]; bm = M; bn = N; }
      if (!best) break;
      o += snprintf(b + o, sizeof b - o, " %u.%u=%.1f(%.2f)", bm, bn,
                    (double)best / 1000.0 / n, (double)g_cmsg_n[bm][bn] / n);
      g_cmsg_us[bm][bn] = 0;            /* consumed; the rest is cleared below */
    }
    log_printf("%s", b);
  }
  memset(g_cmsg_us, 0, sizeof g_cmsg_us);
  memset(g_cmsg_n, 0, sizeof g_cmsg_n);
  for (unsigned i = 0; i < ENG_N; i++) { g_eng[i].sum = 0; g_eng[i].max = 0; }
  for (unsigned i = 0; i < HOOK_N; i++) { g_hook[i].us = 0; g_hook[i].calls = 0; }
  g_prof_draw_us = 0;
  g_eng_n = 0;
}

/* Which timed functions a hitch frame spent its time in: every hook with
 * 2 ms or more this frame, largest first. Times are inclusive, so a caller
 * and the callee under it both appear (menuL holds equipAdd, render holds
 * singlePass). */
void gameprof_hitch_blame(void) {
  char b[512];
  int o = snprintf(b, sizeof b, "[hitch]   in:");
  unsigned shown = 0;
  uint8_t done[HOOK_N];
  memset(done, 0, sizeof done);
  for (;;) {
    int best = -1;
    for (unsigned i = 0; i < HOOK_N; i++)
      if (!done[i] && g_hook[i].frame_us >= 2000 &&
          (best < 0 || g_hook[i].frame_us > g_hook[best].frame_us))
        best = (int)i;
    if (best < 0 || o > (int)sizeof b - 32) break;
    done[best] = 1;
    o += snprintf(b + o, sizeof b - o, " %s=%u", g_hook[best].tag,
                  (unsigned)(g_hook[best].frame_us / 1000u));
    shown++;
  }
  /* our GL texture wrappers + vitaGL under them, wherever they were called from */
  if (g_prof_tex_frame_us >= 2000 && o < (int)sizeof b - 32) {
    o += snprintf(b + o, sizeof b - o, " glTex=%u(%u calls)",
                  (unsigned)(g_prof_tex_frame_us / 1000u), g_prof_tex_frame_n);
    shown++;
  }
  if (shown) log_printf("%s ms", b);
  else log_printf("[hitch]   in: none of the timed functions (outside them all)");
}

void gameprof_frame_reset(void) {
  for (unsigned i = 0; i < HOOK_N; i++) g_hook[i].frame_us = 0;
  g_prof_tex_frame_us = 0;
  g_prof_tex_frame_n = 0;
}

#else
uint64_t g_prof_draw_us;
uint64_t g_prof_tex_frame_us;
unsigned g_prof_tex_frame_n;
void gameprof_hitch_blame(void) {}
void gameprof_frame_reset(void) {}
void gameprof_install(void) {}
void gameprof_after_update(void) {}
void gameprof_window_report(void) {}
#endif
