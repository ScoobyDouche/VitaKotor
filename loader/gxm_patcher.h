/* gxm_patcher.h -- keep vitaGL's vertex-program pool from filling up.
 *
 * vitaGL asks the GXM shader patcher for a patched vertex program on every
 * draw and never gives one back. Each new (shader, vertex layout) pair takes
 * vertex USSE memory from a fixed pool, so a long session walks through enough
 * areas to fill it. After that every create for a new layout fails, vitaGL
 * ignores the error and draws with the program it had, built for another
 * layout, and the GPU reads positions from the wrong bytes: the world
 * geometry spikes and stays spiked until relaunch (log192: pool at 1014 of
 * 1024 KB, 48k failed creates).
 *
 * This keeps one reference per variant, remembers the frame it was last bound,
 * and when a create fails, releases the ones no draw has bound for a while and
 * tries again.
 */
#pragma once

#include <stdint.h>

/* Bytes for vglSetupShaderPatcher, called before vglInitExtended. */
#define GXMP_BUFFER_MEM        (1 * 1024 * 1024)
#define GXMP_VERTEX_USSE_MEM   (4 * 1024 * 1024)
#define GXMP_FRAGMENT_USSE_MEM (1 * 1024 * 1024)

/* Start managing variants. Everything created before this (vitaGL's own
 * clear, blit and splash programs, made once and bound forever) stays put. */
void gxmp_arm(void);
void gxmp_on_swap(void);
void gxmp_window_report(void);
