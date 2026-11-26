/* crowd.h -- skip drawing distant creatures and objects, and count them.
 *
 * Busy areas (Taris Upper City South, the Undercity) draw 500-800 objects a
 * frame, and the engine's per-object work to draw them is the frame. The
 * engine only culls by the area's fog end, which in the cities is far beyond
 * where a pedestrian is more than a few pixels tall.
 *
 * Gob::Render is hooked for the dynamic pass (Scene::RenderDynamicGeometry):
 * a top-level object farther than CrowdDistance metres from the camera is not
 * drawn. Its AI, animation and shadow are untouched (ShadowDistance covers
 * the shadow), so it pops back the moment it comes into range.
 *
 * Nothing here is hard-coded from a disassembly:
 *   - the Gob's position field is read out of Gob::SetPosition's own code
 *     (three stores of the softfp Vector in r1-r3 relative to `this`);
 *   - each object's class comes from its vtable's C++ typeinfo, which also
 *     checks that the camera is a Gob, so its position is in the same field.
 * If either fails, the cull stays off and the log says why.
 *
 * swkotor.ini [Vita Options]:
 *   CrowdDistance=0     metres; 0 = off (default)
 *   CrowdClasses=       comma-separated parts of class names to cull (the
 *                       mangled names the census prints); empty = every class
 *   CrowdCensus=1       every 15 s, log objects drawn per class, time spent
 *                       drawing each class, and how far away they were
 */
#pragma once

extern int g_crowd_dist;
extern int g_crowd_census;
extern char g_crowd_classes[96];

/* Install after the ini is read. Chains onto any earlier body hooks. */
void crowd_install(void);
