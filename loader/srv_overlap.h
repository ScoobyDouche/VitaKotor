/* srv_overlap.h -- run the server's AI tick on core 1 while core 0 renders.
 *
 * GameUpdate runs CClientExoApp::MainLoop (which renders) and then
 * CServerExoApp::MainLoop, one after the other, on the game thread. Nearly all
 * of the server step is CServerAIMaster::UpdateState (sAI ~5.3 of ~5.5 ms in
 * vanilla-prof). We defer that call by half a frame: the server loop's
 * UpdateState call only marks the tick as owed, and the next
 * Camera::RenderScene runs it on a worker thread while the 3D pass draws. The
 * game thread joins the worker when the render returns, so the two only ever
 * overlap inside the render pass.
 *
 * Safety: the server writes client and render state directly (camera rebuild,
 * head turning, fades), and the engine's virtual calls hide most such paths
 * from a static scan. So every render-side engine function (Gob, Scene, Part,
 * Mdl*, CAur*, GLRender, camera classes, CSWCModule, the resource manager, the
 * GL port and raw GL) is reached through a stub. On the worker, during the
 * render pass, the stub parks the AI until the render is done; after that the
 * game thread is parked in the join and the worker has the engine to itself.
 * Every lifecycle entry (save, load, area change, unload, quit) runs an owed
 * tick first, so nothing runs against a server that has gone away.
 *
 * swkotor.ini [Vita Options] ServerOverlap=0 turns it off (the old order). */
#pragma once

#include <stdint.h>

extern volatile int g_ov_active;
extern int g_srv_overlap_enabled;

void srv_overlap_install(void);
void srv_overlap_window_report(void);
/* Called from the GL queue when g_ov_active: parks the AI worker if it is the
 * caller. */
void srv_overlap_gl_guard(void);

#define SRV_OV_GL_GUARD() \
  do { if (__builtin_expect(g_ov_active, 0)) srv_overlap_gl_guard(); } while (0)
