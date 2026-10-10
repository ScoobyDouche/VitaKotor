/* gameprof.h -- where the game's own update time goes.
 *
 * log197: in the Taris cities and the Undercity a frame takes ~80 ms, and
 * ~60 ms of it is CClientAIMaster::UpdateState (the game's g_AIUpdateTime).
 * log198-200 split that: almost all of it is CSWCModule::Update, which ends
 * in Camera::RenderScene -- the 3D pass runs inside the "AI" stopwatch.
 *
 * Two sources, both reported once per 120-frame window:
 *   - the engine's own stopwatch globals (g_ClientUpdateTime, g_AIObjectTime,
 *     ...), which libKOTOR writes every frame and never reads outside its
 *     debug C_Stats/AI_Stats printers;
 *   - inclusive timers on the per-object update functions one level below,
 *     so the next step can target the function rather than the subsystem.
 */
#pragma once

#include <stdint.h>

/* Microseconds spent inside glDrawArrays/glDrawElements this window; the GL
 * wrappers add to it. */
extern uint64_t g_prof_draw_us;
/* This frame's time and calls inside the texture upload wrappers (ours plus
 * vitaGL), for the hitch blame line. */
extern uint64_t g_prof_tex_frame_us;
extern unsigned g_prof_tex_frame_n;

void gameprof_install(void);
/* Call after every GameUpdate: samples the engine's stopwatch globals. */
void gameprof_after_update(void);
void gameprof_window_report(void);
/* On a logged hitch: the timed functions that frame spent its time in. */
void gameprof_hitch_blame(void);
/* Every swap, after any hitch line: start the next frame's tally. */
void gameprof_frame_reset(void);
